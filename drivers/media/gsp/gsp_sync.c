/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
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
 * The 4.4 driver used the legacy sync framework (sync_timeline/sync_pt/
 * sync_fence); 4.14 only has dma_fence and sync_file. A release fence is
 * one dma_fence on the core's context, created with the next seqno when
 * the kcfgs are queued, so fences signal in the order of the core's FIFO.
 * The timeline is refcounted by its fences: an fd may outlive the core.
 */

#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sync_file.h>
#include "gsp_debug.h"
#include "gsp_sync.h"

struct gsp_sync_timeline {
	struct kref kref;
	char name[32];
	spinlock_t lock;
	u64 context;
	unsigned int seqno;
};

struct gsp_sync_fence {
	struct dma_fence base;
	struct gsp_sync_timeline *tl;
};

static void gsp_sync_timeline_release(struct kref *kref)
{
	kfree(container_of(kref, struct gsp_sync_timeline, kref));
}

static void gsp_sync_timeline_put(struct gsp_sync_timeline *tl)
{
	kref_put(&tl->kref, gsp_sync_timeline_release);
}

static const char *gsp_sync_get_driver_name(struct dma_fence *fence)
{
	return "gsp_sync";
}

static const char *gsp_sync_get_timeline_name(struct dma_fence *fence)
{
	struct gsp_sync_fence *f =
			container_of(fence, struct gsp_sync_fence, base);

	return f->tl->name;
}

static bool gsp_sync_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void gsp_sync_fence_release(struct dma_fence *fence)
{
	struct gsp_sync_fence *f =
			container_of(fence, struct gsp_sync_fence, base);
	struct gsp_sync_timeline *tl = f->tl;

	dma_fence_free(fence);
	gsp_sync_timeline_put(tl);
}

static const struct dma_fence_ops gsp_sync_fence_ops = {
	.get_driver_name = gsp_sync_get_driver_name,
	.get_timeline_name = gsp_sync_get_timeline_name,
	.enable_signaling = gsp_sync_enable_signaling,
	.wait = dma_fence_default_wait,
	.release = gsp_sync_fence_release,
};

struct gsp_sync_timeline *gsp_sync_timeline_create(const char *name)
{
	struct gsp_sync_timeline *tl;

	tl = kzalloc(sizeof(*tl), GFP_KERNEL);
	if (!tl)
		return NULL;

	kref_init(&tl->kref);
	strlcpy(tl->name, name, sizeof(tl->name));
	spin_lock_init(&tl->lock);
	tl->context = dma_fence_context_alloc(1);
	tl->seqno = 0;

	GSP_INFO("create timeline: %s\n", tl->name);
	return tl;
}

void gsp_sync_timeline_destroy(struct gsp_sync_timeline *tl)
{
	if (tl)
		gsp_sync_timeline_put(tl);
}

struct dma_fence *gsp_sync_sig_fence_create(struct gsp_sync_timeline *tl)
{
	struct gsp_sync_fence *f;
	unsigned long flags;
	unsigned int seqno;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	kref_get(&tl->kref);
	f->tl = tl;

	spin_lock_irqsave(&tl->lock, flags);
	seqno = ++tl->seqno;
	spin_unlock_irqrestore(&tl->lock, flags);

	dma_fence_init(&f->base, &gsp_sync_fence_ops, &tl->lock,
		       tl->context, seqno);
	return &f->base;
}

/* import the acquire fence of a layer, the reference is held in @data */
int gsp_sync_wait_fence_collect(struct gsp_fence_data *data, int fd)
{
	struct dma_fence *fence;

	if (fd < 0) {
		GSP_DEBUG("wait fd < 0: indicate no need to wait\n");
		return 0;
	}

	if (data->wait_cnt >= GSP_WAIT_FENCE_MAX) {
		GSP_ERR("wait fence overflow, cnt: %d\n", data->wait_cnt);
		return -EINVAL;
	}

	fence = sync_file_get_fence(fd);
	if (!fence) {
		GSP_ERR("wait fence get from fd: %d error\n", fd);
		return -EINVAL;
	}

	data->wait_fen_arr[data->wait_cnt++] = fence;
	return 0;
}

/* wait the acquire fences, GSP_WAIT_FENCE_TIMEOUT in total, then drop them */
int gsp_sync_fence_wait(struct gsp_fence_data *data)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), GSP_WAIT_FENCE_TIMEOUT);
	int ret = 0;
	int i;

	for (i = 0; i < data->wait_cnt; i++) {
		struct dma_fence *fence = data->wait_fen_arr[i];
		long left, remain;

		remain = ktime_to_ms(ktime_sub(deadline, ktime_get()));
		if (remain < 0)
			remain = 0;

		left = dma_fence_wait_timeout(fence, false,
					      msecs_to_jiffies(remain));
		if (left <= 0) {
			GSP_ERR("wait %d/%d fence failed: %ld\n",
				i + 1, data->wait_cnt, left);
			ret = left < 0 ? left : -ETIMEDOUT;
			break;
		}
	}

	for (i = 0; i < data->wait_cnt; i++) {
		dma_fence_put(data->wait_fen_arr[i]);
		data->wait_fen_arr[i] = NULL;
	}
	data->wait_cnt = 0;

	return ret;
}

/* signal and drop the release fence, @status < 0 flags the fence */
void gsp_sync_fence_signal(struct gsp_fence_data *data, int status)
{
	if (!data->sig_fen)
		return;

	if (status < 0)
		dma_fence_set_error(data->sig_fen, status);
	dma_fence_signal(data->sig_fen);
	dma_fence_put(data->sig_fen);
	data->sig_fen = NULL;
}
