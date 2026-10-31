/*
 * Copyright (C) 2016 Spreadtrum Communications Inc.
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

#include <asm/cacheflush.h>
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sprd_iommu.h>
#include <linux/sprd_ion.h>
#include <linux/uaccess.h>
#include "compat_sprd_ion.h"
#include "../ion.h"
#include "../ion_priv.h"


struct ion_device *idev;
EXPORT_SYMBOL(idev);
static int num_heaps;
static struct ion_heap **heaps;
static u32 phys_offset;

#ifdef CONFIG_SPRD_ION_RESTRICT_PHYS
#define SPRD_ION_AID_APP 10000

/* Extra uids (>= 10000) that may use ION_SPRD_CUSTOM_PHYS. */
static unsigned int phys_allow_uids[8];
static unsigned int phys_allow_nr;
module_param_array(phys_allow_uids, uint, &phys_allow_nr, 0644);
MODULE_PARM_DESC(phys_allow_uids,
		 "app-range uids allowed to use ION_SPRD_CUSTOM_PHYS");

static bool sprd_ion_phys_allowed(void)
{
	unsigned int uid = from_kuid(&init_user_ns, current_uid());
	unsigned int i, n = min_t(unsigned int, phys_allow_nr,
				  ARRAY_SIZE(phys_allow_uids));

	if (uid < SPRD_ION_AID_APP || capable(CAP_SYS_RAWIO))
		return true;
	for (i = 0; i < n; i++)
		if (phys_allow_uids[i] == uid)
			return true;
	pr_err_ratelimited("%s: ION_SPRD_CUSTOM_PHYS denied comm=%s pid=%d uid=%u\n",
			   __func__, current->comm, task_pid_nr(current), uid);
	return false;
}
#else
static inline bool sprd_ion_phys_allowed(void) { return true; }
#endif

#ifndef CONFIG_64BIT
static unsigned long user_va2pa(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd = pgd_offset(mm, addr);
	unsigned long pa = 0;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;

	if (pgd_none(*pgd))
		return 0;

	pud = pud_offset(pgd, addr);

	if (pud_none(*pud))
		return 0;

	pmd = pmd_offset(pud, addr);

	if (pmd_none(*pmd))
		return 0;

	ptep = pte_offset_map(pmd, addr);
	pte = *ptep;
	if (pte_present(pte))
		pa = pte_val(pte) & PAGE_MASK;
	pte_unmap(ptep);

	return pa;
}
#endif

/*
 * Look up the ion buffer behind @fd or @dmabuf. With an fd the dma-buf
 * reference is returned in *held and the caller must dma_buf_put() it
 * once it is done with the buffer; with a caller-supplied dma-buf *held
 * is NULL because the caller already owns a reference.
 */
static struct ion_buffer *get_ion_buffer_ref(int fd, struct dma_buf *dmabuf,
					     struct dma_buf **held)
{
	struct ion_buffer *buffer;

	*held = NULL;

	if (fd < 0 && !dmabuf) {
		pr_err("%s, input fd: %d, dmabuf: %p error\n", __func__, fd,
		       dmabuf);
		return ERR_PTR(-EINVAL);
	}

	if (fd >= 0) {
		dmabuf = dma_buf_get(fd);
		if (IS_ERR_OR_NULL(dmabuf)) {
			pr_err("%s, dmabuf=%p dma_buf_get error!\n", __func__,
			       dmabuf);
			return ERR_PTR(-EBADF);
		}
		*held = dmabuf;
	}

	buffer = ion_dma_buf_to_buffer(dmabuf);
	if (!buffer) {
		if (*held) {
			dma_buf_put(*held);
			*held = NULL;
		}
		return ERR_PTR(-EINVAL);
	}

	return buffer;
}

/*
 * Legacy lookup for callers that keep the returned pointer (get_buffer,
 * get_sg_table): the fd reference is dropped here, see the note at those
 * functions.
 */
static struct ion_buffer *get_ion_buffer(int fd, struct dma_buf *dmabuf)
{
	struct dma_buf *held;
	struct ion_buffer *buffer;

	buffer = get_ion_buffer_ref(fd, dmabuf, &held);
	if (held)
		dma_buf_put(held);

	return buffer;
}

struct ion_client *sprd_ion_client_create(const char *name)
{
	if (IS_ERR(idev)) {
		pr_err("%s, idev is illegal\n", __func__);
		return NULL;
	}
	return ion_client_create(idev, name);
}
EXPORT_SYMBOL(sprd_ion_client_create);

int sprd_ion_is_reserved(int fd, struct dma_buf *dmabuf, bool *reserved)
{
	struct ion_buffer *buffer;
	struct dma_buf *held;

	buffer = get_ion_buffer_ref(fd, dmabuf, &held);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	/* The range of system reserved memory is in 0x5 xxxxxxxx,*/
	/* so master must use IOMMU to access it*/

	if (buffer->heap->type == ION_HEAP_TYPE_CARVEOUT
	    && buffer->heap->id != ION_HEAP_ID_SYSTEM)
		*reserved = true;
	else
		*reserved = false;

	if (held)
		dma_buf_put(held);

	return 0;
}

/*
 * DEPRECATED: drops the fd's dma-buf reference before returning, so the
 * result is only valid while another reference exists. Use the _ref variants.
 */
int sprd_ion_get_sg_table(int fd, struct dma_buf *dmabuf,
			  struct sg_table **table, size_t *size)
{
	struct ion_buffer *buffer;

	buffer = get_ion_buffer(fd, dmabuf);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*table = buffer->sg_table;
	*size = buffer->size;

	return 0;
}

/* DEPRECATED: see sprd_ion_get_sg_table(). */
int sprd_ion_get_buffer(int fd, struct dma_buf *dmabuf,
			void **buf, size_t *size)
{
	struct ion_buffer *buffer;

	buffer = get_ion_buffer(fd, dmabuf);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*buf = (void *)buffer;
	*size = buffer->size;

	return 0;
}

/*
 * Reference-holding variants of sprd_ion_get_sg_table()/sprd_ion_get_buffer().
 * When @fd is used, *@held receives the dma-buf reference that keeps the
 * returned buffer alive; the caller must release it with sprd_ion_put()
 * once it no longer uses the buffer/sg_table (including every error and
 * teardown path). When a caller-supplied @dmabuf is used, *@held is NULL
 * and the caller's own reference must outlive the use.
 */
int sprd_ion_get_sg_table_ref(int fd, struct dma_buf *dmabuf,
			      struct sg_table **table, size_t *size,
			      struct dma_buf **held)
{
	struct ion_buffer *buffer;

	buffer = get_ion_buffer_ref(fd, dmabuf, held);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*table = buffer->sg_table;
	*size = buffer->size;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_sg_table_ref);

int sprd_ion_get_buffer_ref(int fd, struct dma_buf *dmabuf,
			    void **buf, size_t *size,
			    struct dma_buf **held)
{
	struct ion_buffer *buffer;

	buffer = get_ion_buffer_ref(fd, dmabuf, held);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*buf = (void *)buffer;
	*size = buffer->size;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_buffer_ref);

/* Drop the reference returned in *held; NULL is allowed. */
void sprd_ion_put(struct dma_buf *held)
{
	if (held)
		dma_buf_put(held);
}
EXPORT_SYMBOL(sprd_ion_put);

struct sprd_ion_pin {
	struct list_head node;
	struct dma_buf *dmabuf;
	unsigned long iova;
	size_t size;
};

void sprd_ion_pin_init(struct sprd_ion_pin_list *list, const char *name)
{
	list->name = name;
	INIT_LIST_HEAD(&list->head);
	spin_lock_init(&list->lock);
	list->count = 0;
}
EXPORT_SYMBOL(sprd_ion_pin_init);

/* On success the list owns the caller's reference @dmabuf. */
int sprd_ion_pin_add(struct sprd_ion_pin_list *list, unsigned long iova,
		     size_t size, struct dma_buf *dmabuf)
{
	struct sprd_ion_pin *pin;

	pin = kzalloc(sizeof(*pin), GFP_KERNEL);
	if (!pin)
		return -ENOMEM;
	pin->dmabuf = dmabuf;
	pin->iova = iova;
	pin->size = size;

	spin_lock(&list->lock);
	if (list->count >= SPRD_ION_PIN_MAX) {
		spin_unlock(&list->lock);
		pr_warn_ratelimited("%s: pinned dma-buf list full (%d), mapping refused; is free_iova paired with get_iova?\n",
				    list->name ? list->name : "sprd_ion",
				    SPRD_ION_PIN_MAX);
		kfree(pin);
		return -ENOSPC;
	}
	list_add_tail(&pin->node, &list->head);
	list->count++;
	spin_unlock(&list->lock);

	return 0;
}
EXPORT_SYMBOL(sprd_ion_pin_add);

void sprd_ion_pin_release(struct sprd_ion_pin_list *list, unsigned long iova,
			  size_t size)
{
	struct sprd_ion_pin *pin, *found = NULL;

	spin_lock(&list->lock);
	list_for_each_entry(pin, &list->head, node) {
		if (pin->iova == iova && pin->size == size) {
			found = pin;
			list_del(&pin->node);
			list->count--;
			break;
		}
	}
	spin_unlock(&list->lock);

	if (found) {
		dma_buf_put(found->dmabuf);
		kfree(found);
	}
}
EXPORT_SYMBOL(sprd_ion_pin_release);

void sprd_ion_pin_release_all(struct sprd_ion_pin_list *list)
{
	LIST_HEAD(release);
	struct sprd_ion_pin *pin, *next;

	spin_lock(&list->lock);
	list_splice_init(&list->head, &release);
	list->count = 0;
	spin_unlock(&list->lock);

	list_for_each_entry_safe(pin, next, &release, node) {
		dma_buf_put(pin->dmabuf);
		kfree(pin);
	}
}
EXPORT_SYMBOL(sprd_ion_pin_release_all);

int sprd_ion_get_sg(void *buf, struct sg_table **table)
{
	struct ion_buffer *buffer;

	if (!buf) {
		pr_err("%s, buf==NULL", __func__);
		return -EINVAL;
	}

	buffer = (struct ion_buffer *)buf;
	*table = buffer->sg_table;

	return 0;
}

void sprd_ion_set_dma(void *buf, int id)
{
	struct ion_buffer *buffer = (struct ion_buffer *)buf;

	buffer->iomap_cnt[id]++;
}

void sprd_ion_put_dma(void *buf, int id)
{
	struct ion_buffer *buffer = (struct ion_buffer *)buf;

	buffer->iomap_cnt[id]--;
}

void sprd_ion_unmap_dma(void *buffer)
{
	int i;
	struct ion_buffer *buf = (struct ion_buffer *)buffer;
	struct sprd_iommu_unmap_data data;

	for (i = 0; i < SPRD_IOMMU_MAX; i++) {
		if (buf->iomap_cnt[i]) {
			buf->iomap_cnt[i] = 0;
			data.buf = buffer;
			data.table = buf->sg_table;
			data.iova_size = buf->size;
			data.dev_id = i;
			sprd_iommu_unmap_orphaned(&data);
		}
	}
}

int sprd_ion_get_phys_addr(int fd, struct dma_buf *dmabuf,
			   unsigned long *phys_addr, size_t *size)
{
	int ret = 0;
	struct ion_buffer *buffer;
	struct dma_buf *held;

	/* the reference is held until the physical address is computed */
	buffer = get_ion_buffer_ref(fd, dmabuf, &held);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	if (buffer->heap->type == ION_HEAP_TYPE_CARVEOUT) {
		if (!buffer->heap->ops->phys) {
			pr_err("%s: ion_phys is not implemented by this heap\n",
			       __func__);
			ret = -ENODEV;
			goto out;
		}
		ret = buffer->heap->ops->phys(buffer->heap, buffer,
						phys_addr,
						size);
		if (ret)
			pr_err("%s, get phys error %d!\n", __func__, ret);
		else
			*phys_addr -= phys_offset;
	} else {
		pr_err("%s, buffer heap type:%d error\n", __func__,
		       buffer->heap->type);
		ret = -EPERM;
	}

out:
	if (held)
		dma_buf_put(held);

	return ret;
}

int sprd_ion_check_phys_addr(struct dma_buf *dmabuf)
{
	struct sg_table *table = NULL;
	struct scatterlist *sgl = NULL;

	if (dmabuf && ion_dma_buf_to_buffer(dmabuf))
		table = ion_dma_buf_to_buffer(dmabuf)->priv_virt;
	else {
		if (!dmabuf)
			pr_err("invalid dmabuf\n");
		else if (!dmabuf->priv)
			pr_err("invalid dmabuf->priv\n");

		return -1;
	}
	if (table && table->sgl)
		sgl = table->sgl;
	else {
		if (!table)
			pr_err("invalid table\n");
		else if (!table->sgl)
			pr_err("invalid table->sgl\n");

		return -1;
	}

#ifdef CONFIG_DEBUG_SG
	if (sgl->sg_magic != SG_MAGIC) {
		pr_err("the sg_magic isn't right, 0x%lx!\n", sgl->sg_magic);
		return -1;
	}
#endif

	return 0;
}

int sprd_ion_get_phys_addr_bydmabuf(int fd_buffer, struct dma_buf *dmabuf,
				    unsigned long *phys_addr, size_t *size)
{
	int ret = 0;
	struct ion_buffer *buffer;

	if (IS_ERR_OR_NULL(dmabuf)) {
		pr_err("%s, dmabuf=%p dma_buf_get error!\n", __func__, dmabuf);
		return -1;
	}

	buffer = ion_dma_buf_to_buffer(dmabuf);
	if (!buffer || !buffer->heap) {
		pr_err("%s, dmabuf is not an ion buffer\n", __func__);
		return -EINVAL;
	}
	if (buffer->heap->type == ION_HEAP_TYPE_CARVEOUT) {
		if (!buffer->heap->ops->phys) {
			pr_err("%s: ion_phys is not implemented by this heap\n",
			       __func__);
			return -ENODEV;
		}

		ret = buffer->heap->ops->phys(buffer->heap, buffer,
						phys_addr,
						size);
		if (ret)
			pr_err("%s, get phys error %d!\n", __func__, ret);
		else
			*phys_addr -= phys_offset;
	} else {
		pr_err("%s, buffer heap type:%d error\n", __func__,
		       buffer->heap->type);
		return -EPERM;
	}

	return ret;
}

/**
* sprd_ion_client_get - increases refcount of the ion_client
* @client:     [in]  client to reduce refcount of
*
* Uses file's refcounting done implicitly by fget()
*/
struct ion_client *sprd_ion_client_get(int fd)
{
	return ion_client_get(fd);
}

/**
* sprd_ion_client_put - decreases refcount of the ion_client
* @client:     [in]  client to reduce refcount of
*
* Uses file's refcounting done implicitly by fput()
*/
void sprd_ion_client_put(struct ion_client *client)
{
	ion_client_put(client);
}

static long sprd_ion_ioctl(struct ion_client *client, unsigned int cmd,
			   unsigned long arg)
{
	int ret = 0;
	void __user *arg_user;

	if (is_compat_task())
		compat_get_custom_data(cmd, (void __user *)arg, &arg_user);
	else
		arg_user = (void __user *)arg;

	switch (cmd) {
	case ION_SPRD_CUSTOM_PHYS:
	{
		struct ion_phys_data data;
		struct ion_handle *handle;

		if (!sprd_ion_phys_allowed())
			return -EPERM;

		if (copy_from_user(&data, arg_user, sizeof(data))) {
			pr_err("%s, PHYS copy_from_user error!\n", __func__);
			return -EFAULT;
		}

		handle = ion_import_dma_buf(client, data.fd_buffer);
		if (IS_ERR(handle)) {
			pr_err("%s, PHYS ion_import_dma_buf error=%p, fd=%d\n",
			       __func__, handle, data.fd_buffer);
			return PTR_ERR(handle);
		}

		ret = ion_phys(client, handle, &data.phys, &data.size);
		ion_free(client, handle);

		if (ret) {
			pr_err("%s,  PHYS ion_phys error=0x%x\n",
			       __func__, ret);
			return ret;
		} else
			data.phys -= phys_offset;

		if (copy_to_user(arg_user, &data, sizeof(data))) {
			pr_err("%s, PHYS copy_to_user error!\n", __func__);
			return -EFAULT;
		}

		pr_debug("%s, PHYS paddress=0x%lx size=0x%zx\n", __func__,
			 data.phys, data.size);
		break;
	}
	case ION_SPRD_CUSTOM_MSYNC:
	{
		struct ion_msync_data data;
#ifndef CONFIG_64BIT
		unsigned long v_addr;
#endif
		if (copy_from_user(&data, arg_user, sizeof(data))) {
			pr_err("%s, MSYNC, copy_from_user error!\n", __func__);
			return -EFAULT;
		}

		if (data.vaddr & (PAGE_SIZE - 1)) {
			pr_err("%s, MSYNC, data.vaddr=0x%lx error!\n", __func__,
			       data.vaddr);
			return -EFAULT;
		}

#ifdef CONFIG_ARM64
		{
			struct dma_buf *dmabuf;
			struct ion_buffer *buffer;
			struct ion_heap *heap;

			dmabuf = dma_buf_get(data.fd_buffer);
			if (IS_ERR(dmabuf)) {
				pr_err("%s: MSYNC dma_buf_get failed, fd=%d\n",
				       __func__, data.fd_buffer);
				return PTR_ERR(dmabuf);
			}

			buffer = ion_dma_buf_to_buffer(dmabuf);
			if (!buffer || !buffer->heap || !buffer->sg_table ||
			    !buffer->heap->device) {
				dma_buf_put(dmabuf);
				return -EINVAL;
			}

			heap = buffer->heap;
			dma_sync_sg_for_device(heap->device,
					       buffer->sg_table->sgl,
					       buffer->sg_table->nents,
					       DMA_BIDIRECTIONAL);
			dma_buf_put(dmabuf);
		}
#elif defined CONFIG_ARM
		dmac_flush_range((void *)data.vaddr,
				(void *)(data.vaddr + data.size));
#elif defined CONFIG_X86
		clflush_cache_range((void *)data.vaddr, data.size);
#endif

#ifndef CONFIG_64BIT
		v_addr = data.vaddr;
		while (v_addr < data.vaddr + data.size) {
			unsigned long phy_addr;

			phy_addr = user_va2pa(current->mm, v_addr);
			if (phy_addr) {
				outer_flush_range(phy_addr,
						  phy_addr + PAGE_SIZE);
			}
			v_addr += PAGE_SIZE;
		}
#endif
		break;
	}
	case ION_SPRD_CUSTOM_INVALIDATE:
	{
		struct dma_buf *dmabuf;
		struct ion_buffer *buffer;
		struct ion_heap *heap;
		unsigned long fd = (unsigned long)arg_user;

		dmabuf = dma_buf_get((int)fd);
		if (IS_ERR(dmabuf)) {
			pr_err("%s: dmabuf is error and dmabuf is %p, fd=%d\n",
			       __func__, dmabuf, (int)fd);
			return PTR_ERR(dmabuf);
		}

		buffer = ion_dma_buf_to_buffer(dmabuf);
		if (!buffer || !buffer->heap || !buffer->sg_table ||
		    !buffer->heap->device) {
			dma_buf_put(dmabuf);
			return -EINVAL;
		}
		heap = buffer->heap;

		dma_sync_sg_for_cpu(heap->device, buffer->sg_table->sgl,
				    buffer->sg_table->nents,
				    DMA_FROM_DEVICE);
		dma_buf_put(dmabuf);
		break;
	}
	case ION_SPRD_CUSTOM_MAP_KERNEL:
	{
		struct ion_kmap_data data;
		struct ion_handle *handle;
		struct ion_buffer *buffer;
		void *kaddr;

		if (copy_from_user(&data, arg_user, sizeof(data))) {
			pr_err("%s, kernel map, copy_from_user error!\n",
			       __func__);
			return -EFAULT;
		}

		handle = ion_import_dma_buf(client, data.fd_buffer);
		if (IS_ERR(handle)) {
			pr_err("%s, kernel map, handle=0x%lx error!\n",
			       __func__, (unsigned long)handle);
			return PTR_ERR(handle);
		}

		buffer = ion_handle_buffer(handle);

		kaddr = ion_map_kernel(client, handle);
		if (IS_ERR(kaddr)) {
			pr_err("%s, kernel map, kaddr=%p error!\n",
			       __func__, kaddr);
			ion_free(client, handle);
			return PTR_ERR(kaddr);
		}
		/*
		 * Do not hand the kernel virtual address to user space: it
		 * cannot be used from there, the mapping is dropped by the
		 * ion_free() below, and the value would only defeat KASLR and
		 * disclose heap layout. Callers rely on the return value.
		 */
		data.kaddr = 0;
		data.size = buffer->size;

		ion_free(client, handle);

		if (copy_to_user(arg_user,
				&data, sizeof(data))) {
			pr_err("%s, kernel map, copy_to_user error!\n",
			       __func__);
			return -EFAULT;
		}
		break;
	}
	case ION_SPRD_CUSTOM_UNMAP_KERNEL:
	{
		struct ion_kunmap_data data;
		struct ion_handle *handle;

		if (copy_from_user(&data, arg_user, sizeof(data))) {
			pr_err("%s, kernel map, copy_from_user error!\n",
			       __func__);
			return -EFAULT;
		}

		handle = ion_import_dma_buf(client, data.fd_buffer);
		if (IS_ERR(handle)) {
			pr_err("%s, kernel map, handle=0x%lx error!\n",
			       __func__, (unsigned long)handle);
			return PTR_ERR(handle);
		}

		ion_unmap_kernel(client, handle);

		ion_free(client, handle);
		break;
	}
	default:
		pr_err("sprd_ion Do not support cmd: %d\n", cmd);
		return -ENOTTY;
	}

	if (is_compat_task())
		compat_put_custom_data(cmd, (void __user *)arg, arg_user);

	return ret;
}

static struct ion_platform_data *sprd_ion_parse_dt(struct platform_device *pdev)
{
	int i = 0, ret = 0;
	const struct device_node *parent = pdev->dev.of_node;
	struct device_node *child = NULL;
	struct ion_platform_data *pdata = NULL;
	struct ion_platform_heap *ion_heaps = NULL;
	struct platform_device *new_dev = NULL;
	u32 val = 0, type = 0;
	const char *name;
	u32 out_values[4];
	struct device_node *np_memory;

	ret = of_property_read_u32(parent, "phys-offset", &phys_offset);
	if (ret)
		phys_offset = 0;
	else
		pr_info("%s: phys_offset=0x%x\n", __func__, phys_offset);

	for_each_child_of_node(parent, child)
		num_heaps++;
	if (!num_heaps)
		return ERR_PTR(-EINVAL);

	pr_info("%s: num_heaps=%d\n", __func__, num_heaps);

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	ion_heaps = kcalloc(num_heaps, sizeof(struct ion_platform_heap),
			    GFP_KERNEL);
	if (!ion_heaps) {
		kfree(pdata);
		return ERR_PTR(-ENOMEM);
	}

	pdata->heaps = ion_heaps;
	pdata->nr = num_heaps;

	for_each_child_of_node(parent, child) {
		new_dev = of_platform_device_create(child, NULL, &pdev->dev);
		if (!new_dev) {
			pr_err("Failed to create device %s\n", child->name);
			goto out;
		}

		pdata->heaps[i].priv = &new_dev->dev;

		ret = of_property_read_u32(child, "reg", &val);
		if (ret) {
			pr_err("%s: Unable to find reg key, ret=%d", __func__,
			       ret);
			goto out;
		}
		pdata->heaps[i].id = val;

		ret = of_property_read_string(child, "label", &name);
		if (ret) {
			pr_err("%s: Unable to find label key, ret=%d", __func__,
			       ret);
			goto out;
		}
		pdata->heaps[i].name = name;

		ret = of_property_read_u32(child, "type", &type);
		if (ret) {
			pr_err("%s: Unable to find type key, ret=%d", __func__,
			       ret);
			goto out;
		}
		pdata->heaps[i].type = type;

		np_memory = of_parse_phandle(child, "memory-region", 0);

		if (!np_memory) {
			pdata->heaps[i].base = 0;
			pdata->heaps[i].size = 0;
		} else {
#ifdef CONFIG_64BIT
			ret = of_property_read_u32_array(np_memory, "reg",
							 out_values, 4);
			if (!ret) {
				pdata->heaps[i].base = out_values[0];
				pdata->heaps[i].base =
						pdata->heaps[i].base << 32;
				pdata->heaps[i].base |= out_values[1];

				pdata->heaps[i].size = out_values[2];
				pdata->heaps[i].size =
						pdata->heaps[i].size << 32;
				pdata->heaps[i].size |= out_values[3];
			} else {
				pdata->heaps[i].base = 0;
				pdata->heaps[i].size = 0;
			}
#else
			ret = of_property_read_u32_array(np_memory, "reg",
							 out_values, 2);
			if (!ret) {
				pdata->heaps[i].base = out_values[0];
				pdata->heaps[i].size = out_values[1];
			} else {
				pdata->heaps[i].base = 0;
				pdata->heaps[i].size = 0;
			}
#endif
		}

		pr_info("%s: heaps[%d]: %s type: %d base: 0x%lx size 0x%zx\n",
			__func__, i,
			pdata->heaps[i].name,
			pdata->heaps[i].type,
			pdata->heaps[i].base,
			pdata->heaps[i].size);
		++i;
	}
	return pdata;
out:
	kfree(pdata->heaps);
	kfree(pdata);
	return ERR_PTR(ret);
}

#ifdef CONFIG_E_SHOW_MEM
static int ion_e_show_mem_handler(struct notifier_block *nb,
				unsigned long val, void *data)
{
	int i;
	enum e_show_mem_type type = (enum e_show_mem_type)val;
	unsigned long total_used = 0;

	printk("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
	printk("Enhanced Mem-info :ION\n");
	for (i = 0; i < num_heaps; i++) {
		if ((E_SHOW_MEM_BASIC != type) ||
		    (ION_HEAP_TYPE_SYSTEM == heaps[i]->type ||
		     ION_HEAP_TYPE_SYSTEM_CONTIG == heaps[i]->type)) {
			ion_debug_heap_show_printk(heaps[i], type, &total_used);
		}
	}

	printk("Total allocated from Buddy: %lu kB\n", total_used / 1024);
	return 0;
}

static struct notifier_block ion_e_show_mem_notifier = {
	.notifier_call = ion_e_show_mem_handler,
};
#endif

static int sprd_ion_probe(struct platform_device *pdev)
{
	int i = 0, ret = -1;
	struct ion_platform_data *pdata = NULL;
	u32 need_free_pdata;

	num_heaps = 0;

	if (pdev->dev.of_node) {
		pdata = sprd_ion_parse_dt(pdev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
		need_free_pdata = 1;
	} else {
		pdata = pdev->dev.platform_data;

		if (!pdata) {
			pr_err("sprd_ion_probe failed: No platform data!\n");
			return -ENODEV;
		}

		num_heaps = pdata->nr;
		if (!num_heaps)
			return -EINVAL;
		need_free_pdata = 0;
	}

	heaps = kcalloc(pdata->nr, sizeof(struct ion_heap *), GFP_KERNEL);
	if (!heaps) {
		ret = -ENOMEM;
		goto out1;
	}

	idev = ion_device_create(&sprd_ion_ioctl);
	if (IS_ERR_OR_NULL(idev)) {
		pr_err("%s,idev is null\n", __func__);
		kfree(heaps);
		ret = PTR_ERR(idev);
		goto out1;
	}

	/* create the heaps as specified in the board file */
	for (i = 0; i < num_heaps; i++) {
		struct ion_platform_heap *heap_data = &pdata->heaps[i];

		if (!strcmp(heap_data->name, "carveout_fb"))
			heap_data->init_clean = false;
		else
			heap_data->init_clean = true;

		if (!pdev->dev.of_node)
			heap_data->priv = &pdev->dev;
		heaps[i] = ion_heap_create(heap_data);
		if (IS_ERR_OR_NULL(heaps[i])) {
			pr_err("%s,heaps is null, i:%d\n", __func__, i);
			ret = PTR_ERR(heaps[i]);
			goto out;
		}
		heaps[i]->device = &pdev->dev;
		ion_device_add_heap(idev, heaps[i]);
	}
	platform_set_drvdata(pdev, idev);
#ifdef CONFIG_E_SHOW_MEM
	register_e_show_mem_notifier(&ion_e_show_mem_notifier);
#endif

	if (need_free_pdata) {
		kfree(pdata->heaps);
		kfree(pdata);
	}
	return 0;
out:
	for (i = 0; i < num_heaps; i++) {
		if (heaps[i])
			ion_heap_destroy(heaps[i]);
	}
	kfree(heaps);
out1:
	if (need_free_pdata) {
		kfree(pdata->heaps);
		kfree(pdata);
	}
	return ret;
}

static int sprd_ion_remove(struct platform_device *pdev)
{
	struct ion_device *idev = platform_get_drvdata(pdev);
	int i;

#ifdef CONFIG_E_SHOW_MEM
	unregister_e_show_mem_notifier(&ion_e_show_mem_notifier);
#endif
	ion_device_destroy(idev);
	for (i = 0; i < num_heaps; i++)
		ion_heap_destroy(heaps[i]);
	kfree(heaps);

	return 0;
}

static const struct of_device_id sprd_ion_ids[] = {
	{ .compatible = "sprd,ion"},
	{},
};

static struct platform_driver ion_driver = {
	.probe = sprd_ion_probe,
	.remove = sprd_ion_remove,
	.driver = {
		.name = "ion",
		.of_match_table = of_match_ptr(sprd_ion_ids),
	}
};

static int __init sprd_ion_init(void)
{
	int result = 0;

	result = platform_driver_register(&ion_driver);
	pr_info("%s,result:%d\n", __func__, result);
	return result;
}

static void __exit sprd_ion_exit(void)
{
	platform_driver_unregister(&ion_driver);
}

subsys_initcall(sprd_ion_init);
module_exit(sprd_ion_exit);
