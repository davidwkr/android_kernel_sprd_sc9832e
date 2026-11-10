/*
 * Copyright (C) 2015-2016 Spreadtrum Communications Inc.
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
#include <linux/err.h>
#include <linux/sprd_iommu.h>
#include <linux/sprd_ion.h>
#include "cam_iommu.h"
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

struct fd_map_dma {
	struct list_head list;
	int fd;
	struct dma_buf *dma_buf;
};
static LIST_HEAD(dma_buffer_list);
static DEFINE_SPINLOCK(dma_buffer_lock);

static int dma_buffer_list_add(int fd, struct dma_buf *buf)
{
	struct fd_map_dma *fd_dma;
	struct fd_map_dma *entry;
	unsigned long flags;

	if (!buf)
		return -EINVAL;

	fd_dma = kzalloc(sizeof(struct fd_map_dma), GFP_KERNEL);
	if (!fd_dma)
		return -ENOMEM;

	fd_dma->fd = fd;
	fd_dma->dma_buf = buf;
	spin_lock_irqsave(&dma_buffer_lock, flags);
	list_for_each_entry(entry, &dma_buffer_list, list) {
		if (fd == entry->fd && buf == entry->dma_buf) {
			spin_unlock_irqrestore(&dma_buffer_lock, flags);
			kfree(fd_dma);
			return 0;
		}
	}

	get_dma_buf(buf);
	list_add_tail(&fd_dma->list, &dma_buffer_list);
	spin_unlock_irqrestore(&dma_buffer_lock, flags);
	pr_info("%s, add 0x%x 0x%p\n", __func__, fd, buf);

	return 0;
}

void dma_buffer_list_clear(void)
{
	struct fd_map_dma *fd_dma;
	struct fd_map_dma *fd_dma_next;
	unsigned long flags;
	LIST_HEAD(release_list);

	spin_lock_irqsave(&dma_buffer_lock, flags);
	list_splice_init(&dma_buffer_list, &release_list);
	spin_unlock_irqrestore(&dma_buffer_lock, flags);

	list_for_each_entry_safe(fd_dma, fd_dma_next, &release_list, list) {
		list_del_init(&fd_dma->list);
		pr_info("%s, del: 0x%x 0x%p\n", __func__,
			fd_dma->fd, fd_dma->dma_buf);
		dma_buf_put(fd_dma->dma_buf);
		kfree(fd_dma);
	}
}

int pfiommu_get_sg_table(struct pfiommu_info *pfinfo)
{
	int i, ret;

	for (i = 0; i < 2; i++) {
		if (pfinfo->mfd[i] > 0) {
			pfinfo->dmabuf_p[i] = dma_buf_get(pfinfo->mfd[i]);
			if (IS_ERR_OR_NULL(pfinfo->dmabuf_p[i])) {
				pr_err("failed to get dma buf %p\n",
				       pfinfo->dmabuf_p[i]);
				pfinfo->dmabuf_p[i] = NULL;
				return -EFAULT;
			}

			ret = sprd_ion_get_buffer(-1, pfinfo->dmabuf_p[i],
						    &pfinfo->buf[i],
						    &pfinfo->size[i]);
			if (ret) {
				pr_err("failed to get sg table %d mfd 0x%x\n",
					i, pfinfo->mfd[i]);
				dma_buf_put(pfinfo->dmabuf_p[i]);
				pfinfo->dmabuf_p[i] = NULL;
				return -EFAULT;
			}

			ret = dma_buffer_list_add(pfinfo->mfd[i],
						  pfinfo->dmabuf_p[i]);
			dma_buf_put(pfinfo->dmabuf_p[i]);
			if (ret) {
				pfinfo->dmabuf_p[i] = NULL;
				return ret;
			}
		}
	}

	return 0;
}

int  pfiommu_put_sg_table(void)
{
	dma_buffer_list_clear();
	return 0;
}

int pfiommu_get_addr(struct pfiommu_info *pfinfo)
{
	int i;
	int ret = 0;
	struct sprd_iommu_map_data iommu_data;
	pr_debug("%s, cb: %pS\n", __func__, __builtin_return_address(0));

	for (i = 0; i < 2; i++) {
		if (pfinfo->size[i] <= 0)
			continue;

		if (sprd_iommu_attach_device(pfinfo->dev) == 0) {
			memset(&iommu_data, 0x00, sizeof(iommu_data));
			iommu_data.buf = pfinfo->buf[i];
			iommu_data.iova_size = pfinfo->size[i];
			iommu_data.ch_type = SPRD_IOMMU_FM_CH_RW;
			iommu_data.sg_offset = pfinfo->offset[i];

			ret = sprd_iommu_map(pfinfo->dev, &iommu_data);
			if (ret) {
				pr_err("failed to get iommu kaddr %d\n", i);
				return -EFAULT;
			}

			pfinfo->iova[i] = iommu_data.iova_addr;
		} else {
			ret = sprd_ion_get_phys_addr(-1, pfinfo->dmabuf_p[i],
					       &pfinfo->iova[i],
					       &pfinfo->size[i]);
			pfinfo->iova[i] += pfinfo->offset[i];
		}
	}

	return ret;
}

int pfiommu_check_addr(struct pfiommu_info *pfinfo)
{
	struct fd_map_dma *fd_dma;
	struct dma_buf *dmabuf = NULL;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dma_buffer_lock, flags);
	list_for_each_entry(fd_dma, &dma_buffer_list, list) {
		if (fd_dma->fd == pfinfo->mfd[0] &&
		    fd_dma->dma_buf == pfinfo->dmabuf_p[0]) {
			dmabuf = fd_dma->dma_buf;
			get_dma_buf(dmabuf);
			break;
		}
	}
	spin_unlock_irqrestore(&dma_buffer_lock, flags);

	if (!dmabuf) {
		pr_err("invalid mfd: 0x%x, dma_buf:0x%p!\n",
		       pfinfo->mfd[0],
		       pfinfo->dmabuf_p[0]);
		return -1;
	}

	ret = sprd_ion_check_phys_addr(dmabuf);
	dma_buf_put(dmabuf);
	return ret;
}

int pfiommu_free_addr(struct pfiommu_info *pfinfo)
{
	int i, ret;
	struct sprd_iommu_unmap_data iommu_data;

	pr_debug("%s, cb: %pS, iova 0x%lx\n",
		 __func__, __builtin_return_address(0), pfinfo->iova[0]);
	for (i = 0; i < 2; i++) {
		if (pfinfo->size[i] <= 0 || pfinfo->iova[i] == 0)
			continue;

		if (sprd_iommu_attach_device(pfinfo->dev) == 0) {
			iommu_data.iova_addr = pfinfo->iova[i];
			iommu_data.table = pfinfo->table[i];
			iommu_data.iova_size = pfinfo->size[i];
			iommu_data.ch_type = SPRD_IOMMU_FM_CH_RW;
			iommu_data.buf = NULL;

			ret = sprd_iommu_unmap(pfinfo->dev, &iommu_data);
			if (ret) {
				pr_err("failed to free iommu %d\n", i);
				return -EFAULT;
			} else {
				pfinfo->iova[i] = 0;
				pfinfo->size[i] = 0;
			}
		}
	}

	return 0;
}

int pfiommu_free_addr_with_id(struct pfiommu_info *pfinfo,
	enum sprd_iommu_chtype ctype, unsigned int cid)
{
	int i, ret;
	struct sprd_iommu_unmap_data iommu_data;

	pr_debug("%s, cb: %pS, iova 0x%lx\n",
		 __func__, __builtin_return_address(0), pfinfo->iova[0]);
	for (i = 0; i < 2; i++) {
		if (pfinfo->size[i] <= 0 || pfinfo->iova[i] == 0)
			continue;

		if (sprd_iommu_attach_device(pfinfo->dev) == 0) {
			iommu_data.iova_addr = pfinfo->iova[i];
			iommu_data.iova_size = pfinfo->size[i];
			iommu_data.ch_type = ctype;
			iommu_data.buf = NULL;
			iommu_data.channel_id = cid;

			ret = sprd_iommu_unmap(pfinfo->dev,
					&iommu_data);
			if (ret) {
				pr_err("failed to free iommu %d\n", i);
				return -EFAULT;
			} else {
				pfinfo->iova[i] = 0;
				pfinfo->size[i] = 0;
			}
		}
	}

	return 0;
}
