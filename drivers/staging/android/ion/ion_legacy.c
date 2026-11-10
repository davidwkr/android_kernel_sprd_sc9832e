// SPDX-License-Identifier: GPL-2.0
/*
 * Legacy (pre-4.12) ION ioctl ABI compatibility shim.
 *
 * The 4.14 ion driver dropped the ion_handle/ion_client model: ION_IOC_ALLOC
 * now returns a dma-buf fd directly and there is no handle, no custom-ioctl
 * hook, and no 32-bit struct translation beyond what the fixed-width new
 * uapi structs already get for free. Stock Android 8.1 vendor userspace
 * (libion.so, libmemion.so, and gralloc/camera libraries layered on top of
 * them) was built against the old, handle-based ABI and still issues the
 * old ioctl numbers and structures verbatim, in both 64-bit and 32-bit
 * (compat) form, including the sprd ION_IOC_CUSTOM sub-commands.
 *
 * This file recreates just enough of the old semantics for those binaries
 * to keep working: a per-open-file table mapping a legacy int handle to a
 * held struct dma_buf reference, plus translation of the handle-based
 * ioctls onto the fd-based ion_new_alloc()/ion_phys() core already present
 * in ion.c. Anything issuing the native 4.14 ABI (ION_IOC_ALLOC,
 * ION_IOC_HEAP_QUERY, ION_IOC_PHY, ION_IOC_VERSION) falls straight through
 * to the existing ion_ioctl(), unchanged.
 *
 * The set of legacy ioctls and sprd custom sub-commands implemented here
 * was verified against the actual stock libion.so / libmemion.so
 * disassembly; see devices/ION_414_LEGACY.md.
 */

#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/sprd_ion.h>
#include <linux/uaccess.h>

#include "ion.h"
#include "ion_legacy.h"

#ifdef CONFIG_SPRD_ION_RESTRICT_PHYS
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/ctype.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/uidgid.h>

#define SPRD_ION_AID_APP		10000
#define SPRD_ION_AID_USER_OFFSET	100000

/*
 * Full uids (>= 10000) that may query physical addresses. Read-only after
 * boot: set on the kernel command line as sprd_ion.phys_allow_uids=a,b
 * (same parameter name as the 4.4 tree).
 */
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "sprd_ion."
static unsigned int phys_allow_uids[8];
static unsigned int phys_allow_nr;
module_param_array(phys_allow_uids, uint, &phys_allow_nr, 0444);
MODULE_PARM_DESC(phys_allow_uids,
		 "app-range uids allowed to query ION physical addresses");

bool ion_phys_allowed(void)
{
	unsigned int uid = from_kuid(&init_user_ns, current_euid());
	/* app id within the Android user: secondary users add 100000 * N */
	unsigned int appid = uid % SPRD_ION_AID_USER_OFFSET;
	unsigned int i, n = min_t(unsigned int, phys_allow_nr,
				  ARRAY_SIZE(phys_allow_uids));
	char comm[TASK_COMM_LEN];

	if (appid < SPRD_ION_AID_APP || capable(CAP_SYS_RAWIO))
		return true;
	for (i = 0; i < n; i++)
		if (phys_allow_uids[i] == uid)
			return true;
	get_task_comm(comm, current);
	for (i = 0; comm[i]; i++)
		if (!isprint(comm[i]))
			comm[i] = '?';
	pr_err_ratelimited("%s: ION physical address query denied comm=%.*s pid=%d uid=%u\n",
			   __func__, TASK_COMM_LEN - 1, comm,
			   task_pid_nr(current), uid);
	return false;
}
#else
bool ion_phys_allowed(void)
{
	return true;
}
#endif

typedef int ion_user_handle_t;

struct ion_allocation_data_legacy {
	size_t len;
	size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	ion_user_handle_t handle;
};

struct ion_fd_data_legacy {
	ion_user_handle_t handle;
	int fd;
};

struct ion_handle_data_legacy {
	ion_user_handle_t handle;
};

struct ion_custom_data_legacy {
	unsigned int cmd;
	unsigned long arg;
};

#define ION_IOC_MAGIC_LEGACY	'I'

#define ION_IOC_ALLOC_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 0, \
				      struct ion_allocation_data_legacy)
#define ION_IOC_FREE_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 1, \
				      struct ion_handle_data_legacy)
#define ION_IOC_MAP_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 2, \
				      struct ion_fd_data_legacy)
#define ION_IOC_SHARE_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 4, \
				      struct ion_fd_data_legacy)
#define ION_IOC_IMPORT_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 5, \
				      struct ion_fd_data_legacy)
#define ION_IOC_CUSTOM_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 6, \
				      struct ion_custom_data_legacy)
#define ION_IOC_SYNC_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 7, \
				      struct ion_fd_data_legacy)

/*
 * sprd ION_IOC_CUSTOM sub-commands. These were dropped from
 * uapi/linux/sprd_ion.h along with the handle-based custom-ioctl hook;
 * the numeric values are kept here unchanged since libmemion.so still
 * encodes them this way.
 */
enum ion_sprd_custom_cmd_legacy {
	ION_SPRD_CUSTOM_PHYS_LEGACY = 0,
	ION_SPRD_CUSTOM_MSYNC_LEGACY,
	ION_SPRD_CUSTOM_FENCE_CREATE_LEGACY,
	ION_SPRD_CUSTOM_FENCE_SIGNAL_LEGACY,
	ION_SPRD_CUSTOM_FENCE_DUP_LEGACY,
	ION_SPRD_CUSTOM_MAP_KERNEL_LEGACY,
	ION_SPRD_CUSTOM_UNMAP_KERNEL_LEGACY,
	ION_SPRD_CUSTOM_INVALIDATE_LEGACY,
};

#ifdef CONFIG_COMPAT
/*
 * 32-bit (compat) counterparts. ion_fd_data_legacy/ion_handle_data_legacy
 * are plain ints and already have the same layout in both ABIs, so only
 * the structs touching size_t/unsigned long need a compat twin.
 */
struct compat_ion_allocation_data_legacy {
	compat_size_t len;
	compat_size_t align;
	compat_uint_t heap_id_mask;
	compat_uint_t flags;
	compat_int_t handle;
};

struct compat_ion_custom_data_legacy {
	compat_uint_t cmd;
	compat_ulong_t arg;
};

struct compat_ion_phys_data_legacy {
	compat_int_t fd_buffer;
	compat_ulong_t phys;
	compat_size_t size;
};

struct compat_ion_msync_data_legacy {
	compat_int_t fd_buffer;
	compat_ulong_t vaddr;
	compat_ulong_t paddr;
	compat_size_t size;
};

struct compat_ion_kmap_data_legacy {
	compat_int_t fd_buffer;
	compat_u64 kaddr;
	compat_size_t size;
};

#define COMPAT_ION_IOC_ALLOC_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 0, \
					struct compat_ion_allocation_data_legacy)
#define COMPAT_ION_IOC_CUSTOM_LEGACY	_IOWR(ION_IOC_MAGIC_LEGACY, 6, \
					struct compat_ion_custom_data_legacy)
#endif /* CONFIG_COMPAT */

/*
 * Per-open-file legacy handle table. ion_fops had no .open of its own
 * upstream, so filp->private_data was always NULL and is free to reuse.
 */
struct ion_legacy_client {
	struct mutex lock;
	struct idr idr;
};

int ion_legacy_open(struct inode *inode, struct file *file)
{
	struct ion_legacy_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	mutex_init(&client->lock);
	idr_init(&client->idr);
	file->private_data = client;

	return 0;
}

int ion_legacy_release(struct inode *inode, struct file *file)
{
	struct ion_legacy_client *client = file->private_data;
	struct dma_buf *dmabuf;
	int id;

	if (!client)
		return 0;

	idr_for_each_entry(&client->idr, dmabuf, id)
		dma_buf_put(dmabuf);

	idr_destroy(&client->idr);
	kfree(client);
	file->private_data = NULL;

	return 0;
}

static int legacy_alloc(struct ion_legacy_client *client, size_t len,
			unsigned int heap_id_mask, unsigned int flags,
			int *out_handle)
{
	struct dma_buf *dmabuf;
	int id;

	dmabuf = ion_new_alloc(len, heap_id_mask, flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	mutex_lock(&client->lock);
	id = idr_alloc(&client->idr, dmabuf, 1, 0, GFP_KERNEL);
	mutex_unlock(&client->lock);

	if (id < 0) {
		dma_buf_put(dmabuf);
		return id;
	}

	*out_handle = id;
	return 0;
}

static int legacy_free(struct ion_legacy_client *client, int handle)
{
	struct dma_buf *dmabuf;

	mutex_lock(&client->lock);
	dmabuf = idr_remove(&client->idr, handle);
	mutex_unlock(&client->lock);

	if (!dmabuf)
		return -EINVAL;

	dma_buf_put(dmabuf);
	return 0;
}

/*
 * ION_IOC_MAP and ION_IOC_SHARE are identical in the legacy ABI. The fd is
 * only reserved here, with a reference on the buffer: the caller installs
 * it (legacy_share_commit) once the fd number has reached userspace, or
 * drops both (legacy_share_abort), so a failed copy leaks nothing.
 */
static int legacy_share(struct ion_legacy_client *client, int handle,
			struct dma_buf **out)
{
	struct dma_buf *dmabuf;
	int fd;

	mutex_lock(&client->lock);
	dmabuf = idr_find(&client->idr, handle);
	if (dmabuf)
		get_dma_buf(dmabuf);
	mutex_unlock(&client->lock);

	if (!dmabuf)
		return -EINVAL;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}

	*out = dmabuf;
	return fd;
}

/* Consumes the reference taken by legacy_share(). */
static void legacy_share_commit(int fd, struct dma_buf *dmabuf)
{
	fd_install(fd, dmabuf->file);
}

static void legacy_share_abort(int fd, struct dma_buf *dmabuf)
{
	put_unused_fd(fd);
	dma_buf_put(dmabuf);
}

static int legacy_import(struct ion_legacy_client *client, int fd,
			 int *out_handle)
{
	struct dma_buf *dmabuf;
	int id;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	if (!ion_dma_buf_is_ion(dmabuf)) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	mutex_lock(&client->lock);
	id = idr_alloc(&client->idr, dmabuf, 1, 0, GFP_KERNEL);
	mutex_unlock(&client->lock);

	if (id < 0) {
		dma_buf_put(dmabuf);
		return id;
	}

	*out_handle = id;
	return 0;
}

static int legacy_sync(int fd)
{
	struct dma_buf *dmabuf;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	dma_buf_begin_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_put(dmabuf);

	return 0;
}

static int legacy_custom_phys(void __user *arg_user)
{
	struct ion_phys_data data;
	int ret;

	if (copy_from_user(&data, arg_user, sizeof(data)))
		return -EFAULT;

	ret = ion_phys(data.fd_buffer, &data.phys, &data.size);
	if (ret)
		return ret;

	if (copy_to_user(arg_user, &data, sizeof(data)))
		return -EFAULT;

	return 0;
}

static int legacy_custom_msync(void __user *arg_user)
{
	struct ion_msync_data data;
	struct dma_buf *dmabuf;

	if (copy_from_user(&data, arg_user, sizeof(data)))
		return -EFAULT;

	dmabuf = dma_buf_get(data.fd_buffer);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	dma_buf_begin_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_put(dmabuf);

	return 0;
}

/*
 * Unlike the other sprd custom commands, ION_SPRD_CUSTOM_INVALIDATE carries
 * the fd directly as the ion_custom_data arg (no sub-struct, no pointer
 * dereference) -- matches the historical sprd driver exactly.
 */
static int legacy_custom_invalidate(void __user *arg_user)
{
	int fd = (int)(unsigned long)arg_user;
	struct dma_buf *dmabuf;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
	dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
	dma_buf_put(dmabuf);

	return 0;
}

/*
 * The legacy driver never left a kernel mapping alive across the ioctl:
 * MAP_KERNEL mapped, read the size, and dropped the mapping again before
 * returning to userspace -- a kernel VA is unusable there anyway, and
 * handing one out would leak kernel layout, so kaddr is always zeroed.
 * UNMAP_KERNEL therefore has nothing left to tear down; it only validates
 * the fd, matching that behaviour.
 */
static int legacy_custom_map_kernel(void __user *arg_user)
{
	struct ion_kmap_data data;
	struct dma_buf *dmabuf;

	if (copy_from_user(&data, arg_user, sizeof(data)))
		return -EFAULT;

	dmabuf = dma_buf_get(data.fd_buffer);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/* the sprd kernel-map helpers assume an ion exporter */
	if (!ion_dma_buf_is_ion(dmabuf)) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	/*
	 * No mapping is made: the result userspace sees is only the size,
	 * with kaddr 0, so mapping and unmapping again bought nothing (and
	 * tied this built-in shim to the ION_SPRD module's symbols).
	 */
	data.size = dmabuf->size;
	dma_buf_put(dmabuf);
	data.kaddr = 0;

	if (copy_to_user(arg_user, &data, sizeof(data)))
		return -EFAULT;

	return 0;
}

static int legacy_custom_unmap_kernel(void __user *arg_user)
{
	struct ion_kunmap_data data;
	struct dma_buf *dmabuf;

	if (copy_from_user(&data, arg_user, sizeof(data)))
		return -EFAULT;

	dmabuf = dma_buf_get(data.fd_buffer);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	dma_buf_put(dmabuf);
	return 0;
}

long ion_ioctl_dispatch(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ion_legacy_client *client = filp->private_data;
	void __user *arg_user = (void __user *)arg;
	struct dma_buf *dmabuf;
	int handle, fd, ret;

	switch (cmd) {
	case ION_IOC_ALLOC_LEGACY:
	{
		struct ion_allocation_data_legacy data;

		if (copy_from_user(&data, arg_user, sizeof(data)))
			return -EFAULT;

		ret = legacy_alloc(client, data.len, data.heap_id_mask,
				    data.flags, &handle);
		if (ret)
			return ret;

		data.handle = handle;
		if (copy_to_user(arg_user, &data, sizeof(data))) {
			/* userspace never learned the handle: drop it */
			legacy_free(client, handle);
			return -EFAULT;
		}

		return 0;
	}
	case ION_IOC_FREE_LEGACY:
	{
		struct ion_handle_data_legacy data;

		if (copy_from_user(&data, arg_user, sizeof(data)))
			return -EFAULT;

		return legacy_free(client, data.handle);
	}
	case ION_IOC_MAP_LEGACY:
	case ION_IOC_SHARE_LEGACY:
	{
		struct ion_fd_data_legacy data;

		if (copy_from_user(&data, arg_user, sizeof(data)))
			return -EFAULT;

		fd = legacy_share(client, data.handle, &dmabuf);
		if (fd < 0)
			return fd;

		data.fd = fd;
		if (copy_to_user(arg_user, &data, sizeof(data))) {
			legacy_share_abort(fd, dmabuf);
			return -EFAULT;
		}
		legacy_share_commit(fd, dmabuf);

		return 0;
	}
	case ION_IOC_IMPORT_LEGACY:
	{
		struct ion_fd_data_legacy data;

		if (copy_from_user(&data, arg_user, sizeof(data)))
			return -EFAULT;

		ret = legacy_import(client, data.fd, &handle);
		if (ret)
			return ret;

		data.handle = handle;
		if (copy_to_user(arg_user, &data, sizeof(data))) {
			/* userspace never learned the handle: drop it */
			legacy_free(client, handle);
			return -EFAULT;
		}

		return 0;
	}
	case ION_IOC_SYNC_LEGACY:
	{
		struct ion_fd_data_legacy data;

		if (copy_from_user(&data, arg_user, sizeof(data)))
			return -EFAULT;

		return legacy_sync(data.fd);
	}
	case ION_IOC_CUSTOM_LEGACY:
	{
		struct ion_custom_data_legacy data;
		void __user *sub_arg;

		if (copy_from_user(&data, arg_user, sizeof(data)))
			return -EFAULT;

		sub_arg = (void __user *)data.arg;

		switch (data.cmd) {
		case ION_SPRD_CUSTOM_PHYS_LEGACY:
			if (!ion_phys_allowed())
				return -EPERM;
			return legacy_custom_phys(sub_arg);
		case ION_SPRD_CUSTOM_MSYNC_LEGACY:
			return legacy_custom_msync(sub_arg);
		case ION_SPRD_CUSTOM_INVALIDATE_LEGACY:
			return legacy_custom_invalidate(sub_arg);
		case ION_SPRD_CUSTOM_MAP_KERNEL_LEGACY:
			return legacy_custom_map_kernel(sub_arg);
		case ION_SPRD_CUSTOM_UNMAP_KERNEL_LEGACY:
			return legacy_custom_unmap_kernel(sub_arg);
		default:
			return -ENOTTY;
		}
	}
	default:
		return ion_ioctl(filp, cmd, arg);
	}
}

#ifdef CONFIG_COMPAT
static int legacy_custom_phys_compat(void __user *arg_user)
{
	struct compat_ion_phys_data_legacy data32;
	unsigned long phys;
	size_t size;
	int ret;

	if (copy_from_user(&data32, arg_user, sizeof(data32)))
		return -EFAULT;

	ret = ion_phys(data32.fd_buffer, &phys, &size);
	if (ret)
		return ret;

	data32.phys = phys;
	data32.size = size;
	if (copy_to_user(arg_user, &data32, sizeof(data32)))
		return -EFAULT;

	return 0;
}

static int legacy_custom_msync_compat(void __user *arg_user)
{
	struct compat_ion_msync_data_legacy data32;
	struct dma_buf *dmabuf;

	if (copy_from_user(&data32, arg_user, sizeof(data32)))
		return -EFAULT;

	dmabuf = dma_buf_get(data32.fd_buffer);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	dma_buf_begin_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_put(dmabuf);

	return 0;
}

static int legacy_custom_map_kernel_compat(void __user *arg_user)
{
	struct compat_ion_kmap_data_legacy data32;
	struct dma_buf *dmabuf;

	if (copy_from_user(&data32, arg_user, sizeof(data32)))
		return -EFAULT;

	dmabuf = dma_buf_get(data32.fd_buffer);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/* the sprd kernel-map helpers assume an ion exporter */
	if (!ion_dma_buf_is_ion(dmabuf)) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	/*
	 * No mapping is made: the result userspace sees is only the size,
	 * with kaddr 0, so mapping and unmapping again bought nothing (and
	 * tied this built-in shim to the ION_SPRD module's symbols).
	 */
	data32.size = dmabuf->size;
	dma_buf_put(dmabuf);
	data32.kaddr = 0;

	if (copy_to_user(arg_user, &data32, sizeof(data32)))
		return -EFAULT;

	return 0;
}

long ion_compat_ioctl_dispatch(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct ion_legacy_client *client = filp->private_data;
	void __user *arg_user = compat_ptr(arg);
	int handle, ret;

	switch (cmd) {
	case COMPAT_ION_IOC_ALLOC_LEGACY:
	{
		struct compat_ion_allocation_data_legacy data32;

		if (copy_from_user(&data32, arg_user, sizeof(data32)))
			return -EFAULT;

		ret = legacy_alloc(client, data32.len, data32.heap_id_mask,
				    data32.flags, &handle);
		if (ret)
			return ret;

		data32.handle = handle;
		if (copy_to_user(arg_user, &data32, sizeof(data32))) {
			/* userspace never learned the handle: drop it */
			legacy_free(client, handle);
			return -EFAULT;
		}

		return 0;
	}
	case COMPAT_ION_IOC_CUSTOM_LEGACY:
	{
		struct compat_ion_custom_data_legacy data32;
		void __user *sub_arg;

		if (copy_from_user(&data32, arg_user, sizeof(data32)))
			return -EFAULT;

		sub_arg = compat_ptr(data32.arg);

		switch (data32.cmd) {
		case ION_SPRD_CUSTOM_PHYS_LEGACY:
			if (!ion_phys_allowed())
				return -EPERM;
			return legacy_custom_phys_compat(sub_arg);
		case ION_SPRD_CUSTOM_MSYNC_LEGACY:
			return legacy_custom_msync_compat(sub_arg);
		case ION_SPRD_CUSTOM_INVALIDATE_LEGACY:
			/*
			 * arg is the fd value itself, not a pointer; compat_ptr()
			 * already sign/zero-extended it back to the same bit
			 * pattern legacy_custom_invalidate() truncates to int.
			 */
			return legacy_custom_invalidate(sub_arg);
		case ION_SPRD_CUSTOM_MAP_KERNEL_LEGACY:
			return legacy_custom_map_kernel_compat(sub_arg);
		case ION_SPRD_CUSTOM_UNMAP_KERNEL_LEGACY:
			/* ion_kunmap_data is a single int, same layout in both ABIs */
			return legacy_custom_unmap_kernel(sub_arg);
		default:
			return -ENOTTY;
		}
	}
	/*
	 * ION_IOC_FREE/MAP/SHARE/IMPORT/SYNC use only ints (handle/fd), so
	 * their legacy struct layout and ioctl cmd encoding are identical in
	 * 32-bit and 64-bit; a 32-bit caller lands on the same cmd values
	 * ion_ioctl_dispatch() already handles.
	 */
	case ION_IOC_FREE_LEGACY:
	case ION_IOC_MAP_LEGACY:
	case ION_IOC_SHARE_LEGACY:
	case ION_IOC_IMPORT_LEGACY:
	case ION_IOC_SYNC_LEGACY:
		return ion_ioctl_dispatch(filp, cmd, arg);
	default:
		return ion_ioctl_dispatch(filp, cmd, arg);
	}
}
#endif /* CONFIG_COMPAT */
