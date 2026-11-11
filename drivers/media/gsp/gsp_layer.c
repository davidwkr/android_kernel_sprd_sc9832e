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
#include <linux/sprd_iommu.h>
#include <linux/sprd_ion.h>
#include <video/gsp_cfg.h>
#include "gsp_debug.h"
#include "gsp_layer.h"

int gsp_layer_to_type(struct gsp_layer *layer)
{
	return layer->type;
}

static u32 gsp_layer_to_uv_offset(struct gsp_layer *layer)
{
	return layer->mem_data.uv_offset;
}

static u32 gsp_layer_to_v_offset(struct gsp_layer *layer)
{
	return layer->mem_data.v_offset;
}

int gsp_layer_to_share_fd(struct gsp_layer *layer)
{
	return layer->mem_data.share_fd;
}

struct gsp_buf *gsp_layer_to_buf(struct gsp_layer *layer)
{
	return &layer->mem_data.buf;
}

static struct gsp_addr_data *gsp_layer_to_addr(struct gsp_layer *layer)
{
	return &layer->src_addr;
}

int gsp_layer_to_wait_fd(struct gsp_layer *layer)
{
	return layer->wait_fd;
}

static int gsp_layer_to_sig_fd(struct gsp_layer *layer)
{
	return layer->sig_fd;
}

int gsp_layer_is_enable(struct gsp_layer *layer)
{
	return layer->enable;
}

int gsp_layer_has_share_fd(struct gsp_layer *layer)
{
	return gsp_layer_to_share_fd(layer) > 0;
}

int gsp_layer_is_filled(struct gsp_layer *layer)
{
	return layer->filled;
}

void gsp_layer_set_filled(struct gsp_layer *layer)
{
	layer->filled = 1;
}

/* an enabled layer with a buffer which the gsp reaches through the iommu */
int gsp_layer_need_iommu(struct gsp_layer *layer)
{
	struct gsp_buf *buf = gsp_layer_to_buf(layer);

	return gsp_layer_is_enable(layer) && buf->dmabuf && buf->is_iova;
}

static void gsp_layer_addr_set(struct gsp_layer *layer, u32 base)
{
	struct gsp_addr_data *addr = gsp_layer_to_addr(layer);

	addr->addr_y = base;
	addr->addr_uv = base + gsp_layer_to_uv_offset(layer);
	addr->addr_va = base + gsp_layer_to_v_offset(layer);
	GSP_DEBUG("addr y: %x, uv: %x, v: %x\n",
		  addr->addr_y, addr->addr_uv, addr->addr_va);
}

static void gsp_layer_addr_put(struct gsp_layer *layer)
{
	memset(gsp_layer_to_addr(layer), 0, sizeof(struct gsp_addr_data));
}

/*
 * Take a reference on the dma-buf behind the layer's share_fd. The buffer
 * is looked up once and everything else works on the held dma_buf, so the
 * fd may be closed or replaced by userspace without effect. Rejects fds
 * which are not ION buffers.
 */
int gsp_layer_get_dmabuf(struct gsp_layer *layer)
{
	int fd = gsp_layer_to_share_fd(layer);
	struct gsp_buf *buf = gsp_layer_to_buf(layer);
	struct dma_buf *held = NULL;
	unsigned long phys_addr = 0;
	size_t size = 0;
	bool reserved = false;
	void *ion_buf = NULL;
	int ret;

	ret = sprd_ion_get_buffer_ref(fd, NULL, &ion_buf, &size, &held);
	if (ret) {
		GSP_ERR("layer[%d] get ion buffer failed, fd: %d\n",
			gsp_layer_to_type(layer), fd);
		return ret;
	}

	if (!size) {
		ret = -EINVAL;
		goto put;
	}

	ret = sprd_ion_is_reserved(-1, held, &reserved);
	if (ret) {
		GSP_ERR("layer[%d] ion reserved judgement failed\n",
			gsp_layer_to_type(layer));
		goto put;
	}

	if (!reserved) {
		buf->is_iova = 1;
	} else {
		/* carveout buffer: the gsp gets the 32-bit physical address */
		ret = sprd_ion_get_phys_addr(-1, held, &phys_addr, &size);
		if (ret) {
			GSP_ERR("layer[%d] get ion phys addr failed\n",
				gsp_layer_to_type(layer));
			goto put;
		}
		if ((u64)phys_addr + size > U32_MAX) {
			GSP_ERR("layer[%d] phys addr %lx out of reach\n",
				gsp_layer_to_type(layer), phys_addr);
			ret = -EINVAL;
			goto put;
		}
		buf->is_iova = 0;
		gsp_layer_addr_set(layer, (u32)phys_addr);
	}

	buf->dmabuf = held;
	buf->ion_buf = ion_buf;
	buf->size = size;
	GSP_DEBUG("layer[%d] get dmabuf success\n", gsp_layer_to_type(layer));
	return 0;

put:
	sprd_ion_put(held);
	return ret;
}

void gsp_layer_put_dmabuf(struct gsp_layer *layer)
{
	struct gsp_buf *buf = gsp_layer_to_buf(layer);

	if (!buf->dmabuf)
		return;

	WARN_ON(buf->mapped);
	sprd_ion_put(buf->dmabuf);

	buf->dmabuf = NULL;
	buf->ion_buf = NULL;
	buf->size = 0;
	GSP_DEBUG("gsp layer put dmabuf\n");
}

int gsp_layer_iommu_map(struct gsp_layer *layer, struct device *dev)
{
	struct gsp_buf *buf = gsp_layer_to_buf(layer);
	struct sprd_iommu_map_data iommu_data;
	int ret;

	if (buf->mapped)
		return 0;

	if (!buf->dmabuf || !buf->size) {
		GSP_ERR("layer[%d] buf is invalid\n", gsp_layer_to_type(layer));
		return -EINVAL;
	}

	memset(&iommu_data, 0, sizeof(iommu_data));
	iommu_data.buf = buf->ion_buf;
	iommu_data.iova_size = buf->size;
	iommu_data.ch_type = SPRD_IOMMU_FM_CH_RW;

	ret = sprd_iommu_map(dev, &iommu_data);
	if (ret) {
		GSP_ERR("layer[%d] iommu map failed: %d\n",
			gsp_layer_to_type(layer), ret);
		return ret;
	}

	if (!iommu_data.iova_addr ||
	    (u64)iommu_data.iova_addr + buf->size > U32_MAX) {
		struct sprd_iommu_unmap_data unmap;

		GSP_ERR("iommu allocate iova addr error\n");
		memset(&unmap, 0, sizeof(unmap));
		unmap.iova_addr = iommu_data.iova_addr;
		unmap.iova_size = buf->size;
		unmap.ch_type = SPRD_IOMMU_FM_CH_RW;
		if (iommu_data.iova_addr)
			sprd_iommu_unmap(dev, &unmap);
		return -EINVAL;
	}

	buf->iova = iommu_data.iova_addr;
	buf->mapped = true;
	gsp_layer_addr_set(layer, (u32)buf->iova);
	GSP_DEBUG("layer[%d] iova %lx size %zu\n",
		  gsp_layer_to_type(layer), buf->iova, buf->size);
	return 0;
}

void gsp_layer_iommu_unmap(struct gsp_layer *layer, struct device *dev)
{
	struct gsp_buf *buf = gsp_layer_to_buf(layer);
	struct sprd_iommu_unmap_data iommu_data;

	if (!buf->mapped)
		return;

	memset(&iommu_data, 0, sizeof(iommu_data));
	iommu_data.iova_addr = buf->iova;
	iommu_data.iova_size = buf->size;
	iommu_data.ch_type = SPRD_IOMMU_FM_CH_RW;
	if (sprd_iommu_unmap(dev, &iommu_data))
		GSP_ERR("layer[%d] iommu unmap failed\n",
			gsp_layer_to_type(layer));

	buf->mapped = false;
	buf->iova = 0;
	gsp_layer_addr_put(layer);
}

void gsp_layer_common_print(struct gsp_layer *layer)
{
	struct gsp_addr_data *addr = gsp_layer_to_addr(layer);

	GSP_DEBUG("layer type[%d]\n", gsp_layer_to_type(layer));
	GSP_DEBUG("layer enable: %d\n", gsp_layer_is_enable(layer));
	GSP_DEBUG("layer share_fd: %d\n", gsp_layer_to_share_fd(layer));
	GSP_DEBUG("layer wait_fd: %d\n", gsp_layer_to_wait_fd(layer));
	GSP_DEBUG("layer sig_fd: %d\n", gsp_layer_to_sig_fd(layer));
	GSP_DEBUG("layer src addr y: %x\n", addr->addr_y);
	GSP_DEBUG("layer src addr u: %x\n", addr->addr_uv);
	GSP_DEBUG("layer src addr v: %x\n", addr->addr_va);
}
