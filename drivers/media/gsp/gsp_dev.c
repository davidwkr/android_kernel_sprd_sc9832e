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

#include <linux/compat.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <video/gsp_cfg.h>
#include "gsp_core.h"
#include "gsp_dev.h"
#include "gsp_debug.h"
#include "gsp_interface.h"
#include "gsp_kcfg.h"
#include "gsp_sync.h"
#include "gsp_workqueue.h"
#include "gsp_lite_r2p0/gsp_lite_r2p0_core.h"

static const struct gsp_core_ops gsp_lite_r2p0_core_ops = {
	.alloc = gsp_lite_r2p0_core_alloc,
	.parse_dt = gsp_lite_r2p0_core_parse_dt,
	.init = gsp_lite_r2p0_core_init,
	.free = gsp_lite_r2p0_core_free,
	.copy = gsp_lite_r2p0_core_copy_cfg,
	.verify = gsp_lite_r2p0_core_verify_cfg,
	.trigger = gsp_lite_r2p0_core_trigger,
	.release = gsp_lite_r2p0_core_release,
	.irq_ack = gsp_lite_r2p0_core_irq_ack,
	.enable = gsp_lite_r2p0_core_enable,
	.disable = gsp_lite_r2p0_core_disable,
	.intercept = gsp_lite_r2p0_core_intercept,
	.irq_mask = gsp_lite_r2p0_core_irq_mask,
	.irq_collect = gsp_lite_r2p0_core_irq_collect,
	.reset = gsp_lite_r2p0_core_reset,
	.dump = gsp_lite_r2p0_core_dump,
};

static const struct of_device_id gsp_dt_ids[] = {
	{.compatible = "sprd,gsp-lite_r2p0-sharkle",
	 .data = &gsp_lite_r2p0_core_ops},
	{},
};
MODULE_DEVICE_TABLE(of, gsp_dt_ids);

struct gsp_core *gsp_dev_to_core(struct gsp_dev *gsp, int index)
{
	struct gsp_core *core;

	if (index < 0 || index >= gsp->core_cnt)
		return NULL;

	for_each_gsp_core(core, gsp) {
		if (index == 0)
			return core;
		index--;
	}

	return NULL;
}

struct device *gsp_dev_to_device(struct gsp_dev *gsp)
{
	return gsp->dev;
}

struct gsp_interface *gsp_dev_to_interface(struct gsp_dev *gsp)
{
	return gsp->interface;
}

static void gsp_dev_free(struct gsp_dev *gsp)
{
	struct gsp_core *core, *tmp;

	for_each_gsp_core_safe(core, tmp, gsp) {
		list_del(&core->list);
		gsp_core_deinit(core);
		gsp_core_free(core);
	}

	gsp_interface_detach(gsp->interface);
	gsp->interface = NULL;
}

static int gsp_dev_alloc(struct gsp_dev *gsp,
			 const struct gsp_core_ops *ops)
{
	struct device_node *np = gsp->dev->of_node;
	struct gsp_core *core;
	u32 cnt = 0;
	int ret;
	int i;

	ret = of_property_read_u32(np, "core-cnt", &cnt);
	if (ret || cnt < 1 || cnt > GSP_MAX_CORE_CNT) {
		GSP_ERR("read core count failed\n");
		return ret ? ret : -EINVAL;
	}
	GSP_INFO("core count: %d\n", cnt);
	gsp->core_cnt = cnt;

	ret = of_property_read_u32(np, "io-cnt", &cnt);
	if (ret || cnt < 1 || cnt > GSP_IO_MAX_CNT) {
		GSP_ERR("read io count failed\n");
		return ret ? ret : -EINVAL;
	}
	GSP_INFO("io count: %d\n", cnt);
	gsp->io_cnt = cnt;

	for (i = 0; i < gsp->core_cnt; i++) {
		struct device_node *child = of_parse_phandle(np, "cores", i);

		if (!child) {
			GSP_ERR("parse core[%d] phandle failed\n", i);
			return -EINVAL;
		}

		ret = gsp_core_alloc(&core, ops, child);
		of_node_put(child);
		if (ret)
			return ret;

		core->parent = gsp;
		list_add_tail(&core->list, &gsp->cores);
		GSP_INFO("core[%d] allocate success\n", core->id);
	}

	return 0;
}

static int gsp_dev_init(struct gsp_dev *gsp)
{
	struct gsp_core *core;
	int ret;

	ret = gsp_interface_attach(&gsp->interface, gsp);
	if (ret) {
		GSP_ERR("gsp interface attach failed\n");
		return ret;
	}

	for_each_gsp_core(core, gsp) {
		ret = gsp_core_init(core);
		if (ret) {
			GSP_ERR("init core[%d] failed\n", core->id);
			return ret;
		}
	}

	/*
	 * io_cnt and core_cnt are attributes of the device, the rest of the
	 * capability comes from core[0]
	 */
	core = gsp_dev_to_core(gsp, 0);
	core->capa->io_cnt = gsp->io_cnt;
	core->capa->core_cnt = gsp->core_cnt;

	return gsp_interface_init(gsp->interface);
}

static int gsp_dev_open(struct inode *node, struct file *file)
{
	struct gsp_dev *gsp;

	gsp = container_of(file->private_data, struct gsp_dev, mdev);
	file->private_data = gsp;

	return 0;
}

static long gsp_dev_get_capability(struct gsp_dev *gsp, size_t size,
				   void __user *arg)
{
	struct gsp_core *core = gsp_dev_to_core(gsp, 0);
	struct gsp_capability *capa;

	if (!core)
		return -ENODEV;

	capa = gsp_core_get_capability(core);
	if (size < sizeof(*capa)) {
		GSP_ERR("size: %zu less than request: %zu.\n",
			size, sizeof(*capa));
		return -EINVAL;
	}

	/* the core's capability is bigger than the common one */
	if (copy_to_user(arg, capa, min_t(size_t, size, capa->capa_size)))
		return -EFAULT;

	return 0;
}

/*
 * Put the kcfgs on their core and tell the worker. Called with the kcfgs
 * filled; they take one more reference for the core. With an async
 * trigger the last kcfg carries the release fence, whose fd is handed to
 * userspace only now that nothing can fail any more.
 */
static int gsp_dev_submit(struct gsp_core *core, struct gsp_kcfg **kcfgs,
			  int cnt, bool async, int fd)
{
	struct gsp_kcfg *last = kcfgs[cnt - 1];
	struct sync_file *sf = NULL;
	struct dma_fence *fence = NULL;
	int ret;
	int i;

	mutex_lock(&core->submit_lock);

	if (async) {
		fence = gsp_sync_sig_fence_create(core->timeline);
		if (!fence) {
			mutex_unlock(&core->submit_lock);
			return -ENOMEM;
		}

		sf = sync_file_create(fence);
		if (!sf) {
			dma_fence_put(fence);
			mutex_unlock(&core->submit_lock);
			return -ENOMEM;
		}
		/* the kcfg takes over our reference */
		last->data.sig_fen = fence;
	}

	for (i = 0; i < cnt; i++) {
		kcfgs[i]->tail = last;
		gsp_kcfg_get(kcfgs[i]);
	}

	/* a core which stops or broke takes nothing more: checked on the queue */
	ret = gsp_workqueue_push(&core->wq, kcfgs, cnt);
	if (ret) {
		for (i = 0; i < cnt; i++)
			gsp_kcfg_put(kcfgs[i]);
		if (sf) {
			/* the fd was never installed: this drops the file's reference */
			fput(sf->file);
			last->data.sig_fen = NULL;
			dma_fence_put(fence);
		}
		mutex_unlock(&core->submit_lock);
		return ret;
	}

	if (sf)
		fd_install(fd, sf->file);
	mutex_unlock(&core->submit_lock);

	gsp_core_work(core);
	return 0;
}

static long gsp_dev_trigger(struct gsp_dev *gsp, unsigned int cmd,
			    void __user *arg)
{
	unsigned int nr = _IOC_NR(cmd);
	size_t size = _IOC_SIZE(cmd);
	bool async = (nr & GSP_ASYNC_MASK) ? true : false;
	int cnt = nr & GSP_CNT_MASK;
	struct gsp_kcfg *kcfgs[GSP_IO_MAX_CNT];
	int __user *ufd = NULL;
	struct gsp_core *core;
	void *cfgs;
	int fd = -1;
	int ret;
	int i;

	GSP_DEBUG("aysnc: %d, split: %d, cnt: %d\n", async,
		  (nr & GSP_SPLIT_MASK) ? 1 : 0, cnt);

	core = gsp_core_select(gsp);
	if (!core)
		return -ENODEV;
	if (core->broken)
		return -EIO;

	if (cnt < 1 || cnt > gsp->io_cnt || cnt > core->kcfg_num) {
		GSP_ERR("request error number kcfgs\n");
		return -EINVAL;
	}

	/* the ioctl passes @cnt cfgs back to back, each of the core's size */
	if (size != core->user_cfg_size) {
		GSP_ERR("error base cfg size: %zu\n", size);
		return -EINVAL;
	}

	cfgs = kmalloc_array(cnt, size, GFP_KERNEL);
	if (!cfgs)
		return -ENOMEM;

	if (copy_from_user(cfgs, arg, size * cnt)) {
		GSP_ERR("copy from user failed\n");
		ret = -EFAULT;
		goto free_cfgs;
	}

	ret = gsp_workqueue_acquire(&core->wq, cnt, kcfgs,
				    GSP_CORE_RELEASE_WAIT);
	if (ret) {
		GSP_ERR("kcfg acquire failed: %d\n", ret);
		goto free_cfgs;
	}

	for (i = 0; i < cnt; i++)
		gsp_kcfg_reinit(kcfgs[i]);

	for (i = 0; i < cnt; i++) {
		ret = gsp_kcfg_fill(kcfgs[i], cfgs, i, async, i == cnt - 1);
		if (ret) {
			GSP_ERR("kcfg list fill failed\n");
			goto put_kcfgs;
		}
	}

	if (async) {
		ufd = core->ops->intercept(arg, cnt - 1);

		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			ret = fd;
			goto put_kcfgs;
		}
		if (put_user(fd, ufd)) {
			GSP_ERR("signal fence fd copy to user failed\n");
			put_unused_fd(fd);
			ret = -EFAULT;
			goto put_kcfgs;
		}
	}

	ret = gsp_dev_submit(core, kcfgs, cnt, async, fd);
	if (ret) {
		if (async)
			put_unused_fd(fd);
		goto put_kcfgs;
	}

	if (!async) {
		for (i = 0; i < cnt; i++) {
			ret = gsp_kcfg_wait(kcfgs[i]);
			if (ret)
				break;
		}
	}

put_kcfgs:
	/* the core owns the pushed ones now: this only drops the ioctl's */
	for (i = 0; i < cnt; i++)
		gsp_kcfg_put(kcfgs[i]);
free_cfgs:
	kfree(cfgs);
	return ret;
}

static long gsp_dev_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct gsp_dev *gsp = file->private_data;

	if (_IOC_TYPE(cmd) != GSP_IO_MAGIC)
		return -ENOTTY;

	switch (_IOC_NR(cmd) & GSP_IO_MASK) {
	case GSP_GET_CAPABILITY:
		return gsp_dev_get_capability(gsp, _IOC_SIZE(cmd),
					      (void __user *)arg);
	case GSP_TRIGGER:
		return gsp_dev_trigger(gsp, cmd, (void __user *)arg);
	default:
		GSP_WARN("unknown cmd\n");
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
/* the cfgs have no pointers, so only the argument needs converting */
static long gsp_dev_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	return gsp_dev_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations gsp_dev_fops = {
	.owner = THIS_MODULE,
	.open = gsp_dev_open,
	.unlocked_ioctl = gsp_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gsp_dev_compat_ioctl,
#endif
};

static int gsp_dev_probe(struct platform_device *pdev)
{
	const struct gsp_core_ops *ops;
	struct gsp_dev *gsp;
	int ret;

	ops = of_device_get_match_data(&pdev->dev);
	if (!ops) {
		GSP_ERR("no core ops for the device\n");
		return -EINVAL;
	}

	gsp = devm_kzalloc(&pdev->dev, sizeof(*gsp), GFP_KERNEL);
	if (!gsp)
		return -ENOMEM;

	gsp->dev = &pdev->dev;
	INIT_LIST_HEAD(&gsp->cores);
	platform_set_drvdata(pdev, gsp);

	ret = gsp_dev_alloc(gsp, ops);
	if (ret) {
		GSP_ERR("dev alloc failed\n");
		goto err_free;
	}

	ret = gsp_dev_init(gsp);
	if (ret) {
		GSP_ERR("dev internal init failed\n");
		goto err_free;
	}

	gsp->mdev.minor = MISC_DYNAMIC_MINOR;
	gsp->mdev.name = GSP_DEVICE_NAME;
	gsp->mdev.fops = &gsp_dev_fops;
	ret = misc_register(&gsp->mdev);
	if (ret) {
		GSP_ERR("mdev register failed\n");
		goto err_free;
	}

	GSP_INFO("probe success\n");
	return 0;

err_free:
	gsp_dev_free(gsp);
	return ret;
}

static int gsp_dev_remove(struct platform_device *pdev)
{
	struct gsp_dev *gsp = platform_get_drvdata(pdev);

	GSP_INFO("remove gsp device\n");
	misc_deregister(&gsp->mdev);
	gsp_dev_free(gsp);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int gsp_dev_suspend(struct device *dev)
{
	struct gsp_dev *gsp = dev_get_drvdata(dev);
	struct gsp_core *core, *undo;
	int ret;

	for_each_gsp_core(core, gsp) {
		ret = gsp_core_suspend(core);
		if (ret)
			goto err;
	}

	return 0;

err:
	for_each_gsp_core(undo, gsp) {
		if (undo == core)
			break;
		gsp_core_resume(undo);
	}
	return ret;
}

static int gsp_dev_resume(struct device *dev)
{
	struct gsp_dev *gsp = dev_get_drvdata(dev);
	struct gsp_core *core;

	for_each_gsp_core(core, gsp)
		gsp_core_resume(core);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(gsp_dev_pmops, gsp_dev_suspend, gsp_dev_resume);

static struct platform_driver gsp_drv = {
	.probe = gsp_dev_probe,
	.remove = gsp_dev_remove,
	.driver = {
		.name = "sprd-gsp",
		.of_match_table = of_match_ptr(gsp_dt_ids),
		.pm = &gsp_dev_pmops,
		/*
		 * /dev/gsp has no way to wait for its open ioctls, so the
		 * device is not unbound: remove is only here for the error
		 * paths of a failed probe and for a modular build.
		 */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(gsp_drv);

MODULE_AUTHOR("yintian.tao <yintian.tao@spreadtrum.com>");
MODULE_DESCRIPTION("GSP Driver");
MODULE_LICENSE("GPL");
