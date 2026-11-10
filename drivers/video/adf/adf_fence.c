/*
 * Copyright (C) 2013 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Software completion-fence timeline used by adf_device_post() when the
 * display driver does not implement present_fence/release_fence/
 * advance_timeline itself.
 *
 * The 4.4 ADF core got this from drivers/staging/android's sw_sync
 * (sw_sync_timeline_create/sw_sync_pt_create/sync_fence_create): a
 * kernel-internal timeline object plus "sync points" that fire when the
 * timeline's counter reaches their target value. That API was removed
 * upstream well before 4.14 (replaced everywhere by dma_fence and
 * sync_file; the surviving drivers/dma-buf/sw_sync.c is a self-contained
 * ioctl-driven debug timeline, not an API other drivers can call). This
 * file reimplements the same two-counter semantics directly on dma_fence,
 * so adf_device_post()'s fence timing is unchanged:
 *
 *   - timeline_max is the next sequence number to hand out; every accepted
 *     post reserves one via adf_sw_fence_reserve(), independently of
 *     whether it actually requests a fence, so that timeline_max always
 *     stays in lock-step with the number of posts that will later run
 *     through adf_sw_advance_timeline().
 *   - timeline_cur is the watermark, advanced by one by
 *     adf_sw_advance_timeline() each time a post completes. A fence
 *     signals once timeline_cur reaches the target it was created with.
 *
 * Callers request a present fence targeting their own post's reserved
 * sequence number, or a release fence targeting the following sequence
 * number, so a present fence fires on the next advance and the matching
 * release fence fires one advance later, exactly as with the old sw_sync
 * pts.
 *
 * The timeline (its name and lock) is reference-counted separately from
 * adf_device: a struct adf_sw_fence may be kept alive by a userspace fd
 * long after adf_device_destroy() has torn down the owning adf_device, so
 * it may not hold a raw pointer back into adf_device.
 */

#include <linux/slab.h>
#include <linux/spinlock.h>

#include "adf.h"

struct adf_sw_timeline {
	struct kref kref;
	char name[ADF_NAME_LEN];
	spinlock_t lock;
	u64 fence_context;
	unsigned int timeline_max;
	unsigned int timeline_cur;
	struct list_head pending;
};

struct adf_sw_fence {
	struct dma_fence base;
	struct adf_sw_timeline *timeline;
	struct list_head head;
	unsigned int target;
};

/* wrap-safe "cur has reached target" test, as in TCP sequence comparisons */
static bool adf_sw_timeline_reached(unsigned int cur, unsigned int target)
{
	return (int)(cur - target) >= 0;
}

static void adf_sw_timeline_release(struct kref *kref)
{
	struct adf_sw_timeline *timeline =
			container_of(kref, struct adf_sw_timeline, kref);

	kfree(timeline);
}

static void adf_sw_timeline_put(struct adf_sw_timeline *timeline)
{
	kref_put(&timeline->kref, adf_sw_timeline_release);
}

static const char *adf_sw_fence_get_driver_name(struct dma_fence *fence)
{
	return "adf";
}

static const char *adf_sw_fence_get_timeline_name(struct dma_fence *fence)
{
	struct adf_sw_fence *sw_fence =
			container_of(fence, struct adf_sw_fence, base);

	return sw_fence->timeline->name;
}

/*
 * We never rely on the dma_fence core to notice a fence needs signaling;
 * adf_sw_advance_timeline() always walks the pending list and signals
 * explicitly. Nothing further to arm here.
 */
static bool adf_sw_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void adf_sw_fence_release(struct dma_fence *fence)
{
	struct adf_sw_fence *sw_fence =
			container_of(fence, struct adf_sw_fence, base);
	struct adf_sw_timeline *timeline = sw_fence->timeline;

	dma_fence_free(fence);
	adf_sw_timeline_put(timeline);
}

static const struct dma_fence_ops adf_sw_fence_ops = {
	.get_driver_name = adf_sw_fence_get_driver_name,
	.get_timeline_name = adf_sw_fence_get_timeline_name,
	.enable_signaling = adf_sw_fence_enable_signaling,
	.release = adf_sw_fence_release,
};

int adf_sw_fence_timeline_init(struct adf_device *dev)
{
	struct adf_sw_timeline *timeline;

	timeline = kzalloc(sizeof(*timeline), GFP_KERNEL);
	if (!timeline)
		return -ENOMEM;

	kref_init(&timeline->kref);
	strlcpy(timeline->name, dev->base.name, sizeof(timeline->name));
	timeline->fence_context = dma_fence_context_alloc(1);
	timeline->timeline_max = 1;
	timeline->timeline_cur = 0;
	spin_lock_init(&timeline->lock);
	INIT_LIST_HEAD(&timeline->pending);

	dev->sw_timeline = timeline;
	return 0;
}

unsigned int adf_sw_fence_reserve(struct adf_device *dev)
{
	struct adf_sw_timeline *timeline = dev->sw_timeline;
	unsigned long flags;
	unsigned int seq;

	spin_lock_irqsave(&timeline->lock, flags);
	seq = timeline->timeline_max;
	timeline->timeline_max++;
	spin_unlock_irqrestore(&timeline->lock, flags);

	return seq;
}

/*
 * Undo the latest adf_sw_fence_reserve() when that post is rejected. Only
 * valid while still holding the dev->post_lock the reservation was made
 * under, so no other reservation can have followed it.
 */
void adf_sw_fence_unreserve(struct adf_device *dev)
{
	struct adf_sw_timeline *timeline = dev->sw_timeline;
	unsigned long flags;

	spin_lock_irqsave(&timeline->lock, flags);
	timeline->timeline_max--;
	spin_unlock_irqrestore(&timeline->lock, flags);
}

struct dma_fence *adf_sw_complete_fence(struct adf_device *dev,
		unsigned int target)
{
	struct adf_sw_timeline *timeline = dev->sw_timeline;
	struct adf_sw_fence *sw_fence;
	unsigned long flags;
	bool already_signaled;

	sw_fence = kzalloc(sizeof(*sw_fence), GFP_KERNEL);
	if (!sw_fence)
		return ERR_PTR(-ENOMEM);

	kref_get(&timeline->kref);
	sw_fence->timeline = timeline;
	sw_fence->target = target;
	INIT_LIST_HEAD(&sw_fence->head);

	spin_lock_irqsave(&timeline->lock, flags);

	dma_fence_init(&sw_fence->base, &adf_sw_fence_ops, &timeline->lock,
			timeline->fence_context, sw_fence->target);

	already_signaled = adf_sw_timeline_reached(timeline->timeline_cur,
			sw_fence->target);
	if (!already_signaled) {
		/* list holds a reference until adf_sw_advance_timeline()
		 * signals and drops it */
		dma_fence_get(&sw_fence->base);
		list_add_tail(&sw_fence->head, &timeline->pending);
	}

	spin_unlock_irqrestore(&timeline->lock, flags);

	if (already_signaled)
		dma_fence_signal(&sw_fence->base);

	return &sw_fence->base;
}

void adf_sw_advance_timeline(struct adf_device *dev)
{
	struct adf_sw_timeline *timeline = dev->sw_timeline;
	struct adf_sw_fence *sw_fence, *next;
	unsigned long flags;
	LIST_HEAD(ready);

	spin_lock_irqsave(&timeline->lock, flags);

	timeline->timeline_cur++;
	list_for_each_entry_safe(sw_fence, next, &timeline->pending, head) {
		if (adf_sw_timeline_reached(timeline->timeline_cur,
				sw_fence->target))
			list_move_tail(&sw_fence->head, &ready);
	}

	spin_unlock_irqrestore(&timeline->lock, flags);

	list_for_each_entry_safe(sw_fence, next, &ready, head) {
		list_del_init(&sw_fence->head);
		dma_fence_signal(&sw_fence->base);
		dma_fence_put(&sw_fence->base);
	}
}

void adf_sw_fence_timeline_destroy(struct adf_device *dev)
{
	struct adf_sw_timeline *timeline = dev->sw_timeline;
	struct adf_sw_fence *sw_fence, *next;
	unsigned long flags;
	LIST_HEAD(leftover);

	/* adf_device_destroy() unconditionally tears down everything
	 * adf_device_init() might have partially set up, including when
	 * adf_sw_fence_timeline_init() itself was never reached or failed. */
	if (!timeline)
		return;

	spin_lock_irqsave(&timeline->lock, flags);
	list_splice_init(&timeline->pending, &leftover);
	spin_unlock_irqrestore(&timeline->lock, flags);

	/* Nothing should still be pending at device-destroy time; force any
	 * stragglers to fire so fd holders don't wait forever, and drop our
	 * list reference. */
	list_for_each_entry_safe(sw_fence, next, &leftover, head) {
		list_del_init(&sw_fence->head);
		pr_warn("%s: fence for target %u never reached (timeline at %u)\n",
				timeline->name, sw_fence->target,
				timeline->timeline_cur);
		dma_fence_signal(&sw_fence->base);
		dma_fence_put(&sw_fence->base);
	}

	/* Drop the device's own reference; the timeline (name + lock) stays
	 * alive as long as any struct adf_sw_fence handed to userspace still
	 * references it. */
	adf_sw_timeline_put(timeline);
	dev->sw_timeline = NULL;
}
