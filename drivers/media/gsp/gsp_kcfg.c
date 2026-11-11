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
 */

#include <linux/dma-buf.h>
#include <linux/list.h>
#include <linux/slab.h>
#include "gsp_core.h"
#include "gsp_dev.h"
#include "gsp_debug.h"
#include "gsp_kcfg.h"
#include "gsp_layer.h"
#include "gsp_workqueue.h"

#define for_each_gsp_layer(layer, kcfg) \
	list_for_each_entry((layer), &(kcfg)->cfg->layers, list)

int gsp_kcfg_to_tag(struct gsp_kcfg *kcfg)
{
	return kcfg->tag;
}

int gsp_kcfg_is_async(struct gsp_kcfg *kcfg)
{
	return kcfg->async;
}

void gsp_kcfg_init(struct gsp_kcfg *kcfg, struct gsp_core *core)
{
	kcfg->bind_core = core;
	INIT_LIST_HEAD(&kcfg->list);
	INIT_LIST_HEAD(&kcfg->sibling);
	init_completion(&kcfg->complete);
}

void gsp_kcfg_reinit(struct gsp_kcfg *kcfg)
{
	kref_init(&kcfg->ref);
	reinit_completion(&kcfg->complete);
	kcfg->async = false;
	kcfg->last = false;
	kcfg->status = 0;
	kcfg->tail = NULL;
	kcfg->batch_status = 0;
	memset(&kcfg->data, 0, sizeof(kcfg->data));
}

static void gsp_kcfg_release(struct kref *ref)
{
	struct gsp_kcfg *kcfg = container_of(ref, struct gsp_kcfg, ref);
	struct gsp_core *core = kcfg->bind_core;

	/* nobody ran it, or the core has already cleaned up: no-op then */
	gsp_kcfg_cleanup(kcfg, -ECANCELED);
	gsp_workqueue_put(&core->wq, kcfg);
}

void gsp_kcfg_get(struct gsp_kcfg *kcfg)
{
	kref_get(&kcfg->ref);
}

void gsp_kcfg_put(struct gsp_kcfg *kcfg)
{
	kref_put(&kcfg->ref, gsp_kcfg_release);
}

static int gsp_kcfg_get_dmabuf(struct gsp_kcfg *kcfg)
{
	struct gsp_layer *layer;
	int ret;

	for_each_gsp_layer(layer, kcfg) {
		if (!gsp_layer_is_enable(layer))
			continue;

		/* the gsp must never be given an address of the user's own */
		if (!gsp_layer_has_share_fd(layer)) {
			GSP_ERR("kcfg[%d] layer[%d] has no buffer\n",
				gsp_kcfg_to_tag(kcfg),
				gsp_layer_to_type(layer));
			return -EINVAL;
		}

		ret = gsp_layer_get_dmabuf(layer);
		if (ret) {
			GSP_ERR("kcfg[%d] layer[%d] get dma buf failed\n",
				gsp_kcfg_to_tag(kcfg),
				gsp_layer_to_type(layer));
			return ret;
		}
	}

	return 0;
}

static void gsp_kcfg_put_dmabuf(struct gsp_kcfg *kcfg)
{
	struct gsp_layer *layer;

	for_each_gsp_layer(layer, kcfg) {
		gsp_layer_put_dmabuf(layer);
		GSP_DEBUG("kcfg[%d] layer[%d] put dmabuf\n",
			  gsp_kcfg_to_tag(kcfg), gsp_layer_to_type(layer));
	}
}

static int gsp_kcfg_fence_collect(struct gsp_kcfg *kcfg)
{
	struct gsp_layer *layer;
	int ret;

	for_each_gsp_layer(layer, kcfg) {
		if (!gsp_layer_is_enable(layer))
			continue;

		ret = gsp_sync_wait_fence_collect(&kcfg->data,
						  gsp_layer_to_wait_fd(layer));
		if (ret) {
			GSP_ERR("collect layer[%d] wait fence failed\n",
				gsp_layer_to_type(layer));
			return ret;
		}
	}

	return 0;
}

/*
 * Fill a kcfg of the pool from the user's array: copy and check the
 * params, take the dma-bufs, check the extents against them, and import
 * the acquire fences of an async trigger. On error nothing is held.
 */
int gsp_kcfg_fill(struct gsp_kcfg *kcfg, void *arg, int index,
		  bool async, bool last)
{
	struct gsp_core *core = kcfg->bind_core;
	int ret;

	GSP_DEBUG("kcfg: %p, arg: %p, index: %d\n", kcfg, arg, index);

	kcfg->async = async;
	kcfg->last = last;
	kcfg->status = 0;
	memset(&kcfg->data, 0, sizeof(kcfg->data));

	ret = core->ops->copy(kcfg, arg, index);
	if (ret) {
		GSP_ERR("copy cfg failed\n");
		return ret;
	}

	ret = gsp_kcfg_get_dmabuf(kcfg);
	if (ret)
		goto err;

	ret = core->ops->verify(kcfg);
	if (ret) {
		GSP_ERR("kcfg[%d] verify failed\n", gsp_kcfg_to_tag(kcfg));
		goto err;
	}

	if (async) {
		ret = gsp_kcfg_fence_collect(kcfg);
		if (ret)
			goto err;
	}

	return 0;

err:
	gsp_kcfg_cleanup(kcfg, -ECANCELED);
	return ret;
}

int gsp_kcfg_fence_wait(struct gsp_kcfg *kcfg)
{
	if (!kcfg->async)
		return 0;

	return gsp_sync_fence_wait(&kcfg->data);
}

int gsp_kcfg_iommu_map(struct gsp_kcfg *kcfg)
{
	struct gsp_core *core = kcfg->bind_core;
	struct gsp_layer *layer;
	int ret;

	for_each_gsp_layer(layer, kcfg) {
		if (!gsp_layer_need_iommu(layer)) {
			GSP_DEBUG("layer[%d] no need to iommu map\n",
				  gsp_layer_to_type(layer));
			continue;
		}

		ret = gsp_layer_iommu_map(layer, core->dev);
		if (ret) {
			gsp_kcfg_iommu_unmap(kcfg);
			return ret;
		}
	}

	return 0;
}

void gsp_kcfg_iommu_unmap(struct gsp_kcfg *kcfg)
{
	struct gsp_core *core = kcfg->bind_core;
	struct gsp_layer *layer;

	for_each_gsp_layer(layer, kcfg)
		gsp_layer_iommu_unmap(layer, core->dev);
}

/* idempotent: the layers and the fence data reset what they release */
void gsp_kcfg_cleanup(struct gsp_kcfg *kcfg, int status)
{
	struct gsp_fence_data *data = &kcfg->data;
	int i;

	gsp_kcfg_put_dmabuf(kcfg);

	for (i = 0; i < data->wait_cnt; i++) {
		dma_fence_put(data->wait_fen_arr[i]);
		data->wait_fen_arr[i] = NULL;
	}
	data->wait_cnt = 0;

	gsp_sync_fence_signal(data, status);
}

int gsp_kcfg_batch_status(struct gsp_kcfg *kcfg, int status)
{
	struct gsp_kcfg *tail = kcfg->tail;

	if (!tail)
		return status;

	if (kcfg != tail) {
		if (status && !tail->batch_status)
			tail->batch_status = status;
		return status;
	}

	return tail->batch_status ? tail->batch_status : status;
}

void gsp_kcfg_complete(struct gsp_kcfg *kcfg, int status)
{
	kcfg->status = status;
	complete(&kcfg->complete);
}

/* sync trigger: wait for the core to be done with the kcfg */
int gsp_kcfg_wait(struct gsp_kcfg *kcfg)
{
	long time;

	time = wait_for_completion_interruptible_timeout(&kcfg->complete,
						GSP_WAIT_COMPLETION_TIMEOUT);
	if (time < 0) {
		GSP_ERR("kcfg[%d] interrupt at wait\n", gsp_kcfg_to_tag(kcfg));
		return -EINTR;
	}
	if (time == 0) {
		GSP_ERR("kcfg[%d] wait timeout\n", gsp_kcfg_to_tag(kcfg));
		return -ETIMEDOUT;
	}

	return kcfg->status;
}
