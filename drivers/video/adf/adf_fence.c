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
 *   - timeline_max is the next fence target to hand out; every call to
 *     adf_sw_complete_fence() returns a fence armed for timeline_max +
 *     timeline_offset, then bumps timeline_max by one.
 *   - timeline_cur is the watermark, advanced by one by
 *     adf_sw_advance_timeline() each time a post completes. A fence
 *     signals once timeline_cur reaches the target it was created with.
 *
 * Callers request timeline_offset 0 (ADF_COMPLETE_FENCE_PRESENT) or 1
 * (ADF_COMPLETE_FENCE_RELEASE), so a present fence fires on the next
 * advance and the matching release fence fires one advance later, exactly
 * as with the old sw_sync pts.
 */

#include <linux/slab.h>
#include <linux/spinlock.h>

#include "adf.h"

struct adf_sw_fence {
	struct dma_fence base;
	struct adf_device *dev;
	struct list_head head;
	unsigned int target;
};

static const char *adf_sw_fence_get_driver_name(struct dma_fence *fence)
{
	return "adf";
}

static const char *adf_sw_fence_get_timeline_name(struct dma_fence *fence)
{
	struct adf_sw_fence *sw_fence =
			container_of(fence, struct adf_sw_fence, base);

	return sw_fence->dev->base.name;
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

static const struct dma_fence_ops adf_sw_fence_ops = {
	.get_driver_name = adf_sw_fence_get_driver_name,
	.get_timeline_name = adf_sw_fence_get_timeline_name,
	.enable_signaling = adf_sw_fence_enable_signaling,
};

void adf_sw_fence_timeline_init(struct adf_device *dev)
{
	dev->fence_context = dma_fence_context_alloc(1);
	dev->timeline_max = 1;
	dev->timeline_cur = 0;
	spin_lock_init(&dev->sw_fence_lock);
	INIT_LIST_HEAD(&dev->sw_fence_pending);
}

struct dma_fence *adf_sw_complete_fence(struct adf_device *dev,
		unsigned int timeline_offset)
{
	struct adf_sw_fence *sw_fence;
	unsigned long flags;
	bool already_signaled;

	sw_fence = kzalloc(sizeof(*sw_fence), GFP_KERNEL);
	if (!sw_fence)
		return ERR_PTR(-ENOMEM);

	sw_fence->dev = dev;
	INIT_LIST_HEAD(&sw_fence->head);

	spin_lock_irqsave(&dev->sw_fence_lock, flags);

	sw_fence->target = dev->timeline_max + timeline_offset;
	dev->timeline_max++;

	dma_fence_init(&sw_fence->base, &adf_sw_fence_ops, &dev->sw_fence_lock,
			dev->fence_context, sw_fence->target);

	already_signaled = sw_fence->target <= dev->timeline_cur;
	if (!already_signaled) {
		/* list holds a reference until adf_sw_advance_timeline()
		 * signals and drops it */
		dma_fence_get(&sw_fence->base);
		list_add_tail(&sw_fence->head, &dev->sw_fence_pending);
	}

	spin_unlock_irqrestore(&dev->sw_fence_lock, flags);

	if (already_signaled)
		dma_fence_signal(&sw_fence->base);

	return &sw_fence->base;
}

void adf_sw_advance_timeline(struct adf_device *dev)
{
	struct adf_sw_fence *sw_fence, *next;
	unsigned long flags;
	LIST_HEAD(ready);

	spin_lock_irqsave(&dev->sw_fence_lock, flags);

	dev->timeline_cur++;
	list_for_each_entry_safe(sw_fence, next, &dev->sw_fence_pending, head) {
		if (sw_fence->target <= dev->timeline_cur)
			list_move_tail(&sw_fence->head, &ready);
	}

	spin_unlock_irqrestore(&dev->sw_fence_lock, flags);

	list_for_each_entry_safe(sw_fence, next, &ready, head) {
		list_del_init(&sw_fence->head);
		dma_fence_signal(&sw_fence->base);
		dma_fence_put(&sw_fence->base);
	}
}

void adf_sw_fence_timeline_destroy(struct adf_device *dev)
{
	struct adf_sw_fence *sw_fence, *next;
	unsigned long flags;
	LIST_HEAD(leftover);

	spin_lock_irqsave(&dev->sw_fence_lock, flags);
	list_splice_init(&dev->sw_fence_pending, &leftover);
	spin_unlock_irqrestore(&dev->sw_fence_lock, flags);

	/* Nothing should still be pending at device-destroy time; force any
	 * stragglers to fire so fd holders don't wait forever, and drop our
	 * list reference. */
	list_for_each_entry_safe(sw_fence, next, &leftover, head) {
		list_del_init(&sw_fence->head);
		pr_warn("%s: fence for target %u never reached (timeline at %u)\n",
				dev->base.name, sw_fence->target,
				dev->timeline_cur);
		dma_fence_signal(&sw_fence->base);
		dma_fence_put(&sw_fence->base);
	}
}
