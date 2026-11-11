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

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sprd_iommu.h>
#include <uapi/linux/sched/types.h>
#include "gsp_core.h"
#include "gsp_dev.h"
#include "gsp_debug.h"
#include "gsp_interface.h"
#include "gsp_kcfg.h"
#include "gsp_sync.h"
#include "gsp_workqueue.h"

int gsp_core_to_id(struct gsp_core *core)
{
	return core->id;
}

void gsp_core_reg_write(void __iomem *addr, u32 value)
{
	writel(value, addr);
}

u32 gsp_core_reg_read(void __iomem *addr)
{
	return readl(addr);
}

void gsp_core_reg_update(void __iomem *addr, u32 value, u32 mask)
{
	u32 orig = gsp_core_reg_read(addr);

	gsp_core_reg_write(addr, (orig & ~mask) | (value & mask));
}

struct gsp_capability *gsp_core_get_capability(struct gsp_core *core)
{
	return core->capa;
}

/* ask the worker to start the next kcfg, if there is one and it is free */
void gsp_core_work(struct gsp_core *core)
{
	if (!gsp_workqueue_is_filled(&core->wq))
		return;

	kthread_queue_work(core->kworker, &core->trigger);
}

static void gsp_core_power_off(struct gsp_core *core)
{
	if (core->clk_on) {
		core->ops->disable(core);
		core->clk_on = false;
	}
	if (core->iommu_on) {
		sprd_iommu_suspend(core->dev);
		core->iommu_on = false;
	}
	if (core->prepared) {
		gsp_interface_unprepare(core->parent->interface);
		core->prepared = false;
	}
}

/* enable core must be invoked before iommu map */
static int gsp_core_power_on(struct gsp_core *core)
{
	int ret;

	ret = gsp_interface_prepare(core->parent->interface);
	if (ret) {
		GSP_ERR("core[%d] interface prepare failed\n", core->id);
		return ret;
	}
	core->prepared = true;

	ret = sprd_iommu_resume(core->dev);
	if (ret) {
		GSP_ERR("core[%d] iommu resume failed\n", core->id);
		goto err;
	}
	core->iommu_on = true;

	ret = core->ops->enable(core);
	if (ret) {
		GSP_ERR("core[%d] enable failed\n", core->id);
		goto err;
	}
	core->clk_on = true;

	return 0;

err:
	gsp_core_power_off(core);
	return ret;
}

/*
 * End the kcfg the core has pulled: release what it holds, tell the
 * waiters, and look for the next one. Only from the worker.
 */
static void gsp_core_finish(struct gsp_core *core, struct gsp_kcfg *kcfg,
			    int status)
{
	/* the hang timer only queues work, so it is safe to wait for it */
	del_timer_sync(&core->timer);
	atomic_set(&core->hw_running, 0);

	gsp_kcfg_iommu_unmap(kcfg);
	gsp_kcfg_cleanup(kcfg, status);
	gsp_kcfg_complete(kcfg, status);

	/* disable core must be invoked after iommu unmap */
	gsp_core_power_off(core);

	mutex_lock(&core->lock);
	core->current_kcfg = NULL;
	core->busy = false;
	mutex_unlock(&core->lock);
	wake_up_all(&core->idle_wait);

	GSP_DEBUG("kcfg[%d] done: %d\n", gsp_kcfg_to_tag(kcfg), status);
	gsp_kcfg_put(kcfg);

	gsp_core_work(core);
}

static int gsp_core_start(struct gsp_core *core, struct gsp_kcfg *kcfg)
{
	int ret;

	/* the acquire fences first: no clock runs while we wait for them */
	ret = gsp_kcfg_fence_wait(kcfg);
	if (ret) {
		GSP_ERR("acquire fence wait failed\n");
		return ret;
	}

	ret = gsp_core_power_on(core);
	if (ret)
		return ret;

	ret = gsp_kcfg_iommu_map(kcfg);
	if (ret) {
		GSP_ERR("kcfg[%d] iommu map failed\n", gsp_kcfg_to_tag(kcfg));
		return ret;
	}

	atomic_set(&core->hw_running, 1);
	ret = core->ops->trigger(core);
	if (ret) {
		atomic_set(&core->hw_running, 0);
		if (ret == GSP_K_HW_BUSY_ERR && core->ops->reset)
			core->ops->reset(core);
		return -EIO;
	}

	mod_timer(&core->timer,
		  jiffies + msecs_to_jiffies(GSP_CORE_TIMER_OUT));
	return 0;
}

static void gsp_core_trigger(struct kthread_work *work)
{
	struct gsp_core *core = container_of(work, struct gsp_core, trigger);
	struct gsp_kcfg *kcfg;
	int ret;

	mutex_lock(&core->lock);
	if (core->busy || core->suspended || core->stopping) {
		mutex_unlock(&core->lock);
		return;
	}

	kcfg = gsp_workqueue_pull(&core->wq);
	if (!kcfg) {
		mutex_unlock(&core->lock);
		return;
	}
	core->busy = true;
	core->current_kcfg = kcfg;
	mutex_unlock(&core->lock);

	GSP_DEBUG("gsp core[%d] start kcfg[%d]\n", core->id,
		  gsp_kcfg_to_tag(kcfg));

	ret = gsp_core_start(core, kcfg);
	if (ret)
		gsp_core_finish(core, kcfg, ret);
}

/* the irq came: the hardware is done with the kcfg */
static void gsp_core_release(struct kthread_work *work)
{
	struct gsp_core *core = container_of(work, struct gsp_core, release);
	struct gsp_kcfg *kcfg = core->current_kcfg;
	int ret = 0;

	/* the kcfg may have been ended by the hang timer already */
	if (!kcfg || !atomic_read(&core->hw_running))
		return;

	if (core->ops->release)
		ret = core->ops->release(core);

	gsp_core_finish(core, kcfg, ret ? -EIO : 0);
}

/* the hang timer fired: stop the hardware, then end the kcfg */
static void gsp_core_recover(struct kthread_work *work)
{
	struct gsp_core *core = container_of(work, struct gsp_core, recover);
	struct gsp_kcfg *kcfg = core->current_kcfg;

	if (!kcfg || !atomic_read(&core->hw_running))
		return;

	GSP_ERR("core[%d] kcfg[%d] hung, reset\n", core->id,
		gsp_kcfg_to_tag(kcfg));
	core->ops->dump(core);
	if (core->ops->reset)
		core->ops->reset(core);
	core->force_calc = 1;

	gsp_core_finish(core, kcfg, -ETIMEDOUT);
}

static void gsp_core_hang_handler(struct timer_list *t)
{
	struct gsp_core *core = from_timer(core, t, timer);

	kthread_queue_work(core->kworker, &core->recover);
}

static irqreturn_t gsp_core_irq_handler(int irq, void *data)
{
	struct gsp_core *core = data;

	/* the line is shared, and the hardware only runs for a kcfg */
	if (!atomic_read(&core->hw_running))
		return IRQ_NONE;

	core->ops->irq_ack(core);
	kthread_queue_work(core->kworker, &core->release);

	return IRQ_HANDLED;
}

void gsp_core_free(struct gsp_core *core)
{
	struct gsp_kcfg *kcfg, *tmp;

	if (!core)
		return;

	list_for_each_entry_safe(kcfg, tmp, &core->kcfgs, sibling) {
		list_del(&kcfg->sibling);
		kfree(kcfg->cfg);
		kfree(kcfg);
	}

	if (core->ops->free)
		core->ops->free(core);

	if (core->base)
		iounmap(core->base);
	if (core->dev)
		put_device(core->dev);
	kfree(core->capa);
	/* the specific core is a kvzalloc()ed container of it */
	kvfree(core);
}

int gsp_core_alloc(struct gsp_core **pcore, const struct gsp_core_ops *ops,
		   struct device_node *node)
{
	struct gsp_core *core;
	struct gsp_kcfg *kcfg;
	u32 tmp = 0;
	int ret;
	int i;

	ret = ops->alloc(pcore, node);
	if (ret) {
		GSP_ERR("specific core allocate failed\n");
		return ret;
	}
	core = *pcore;
	core->ops = ops;
	INIT_LIST_HEAD(&core->kcfgs);
	gsp_workqueue_init(&core->wq);
	mutex_init(&core->lock);
	mutex_init(&core->submit_lock);
	init_waitqueue_head(&core->idle_wait);

	/* read core id from dts as an identifier to distinguish multi-cores */
	ret = of_property_read_u32(node, "core-id", &tmp);
	if (ret) {
		GSP_ERR("read core id failed\n");
		goto err;
	}
	core->id = tmp;
	snprintf(core->name, sizeof(core->name), "gsp-core[%d]", core->id);

	/* read max kcfg number possessed by core*/
	ret = of_property_read_u32(node, "kcfg-num", &tmp);
	if (ret || tmp < 1 || tmp > GSP_CORE_MAX_KCFG_NUM) {
		GSP_ERR("kcfg number read failed\n");
		ret = ret ? ret : -EINVAL;
		goto err;
	}
	core->kcfg_num = tmp;
	GSP_INFO("core[%d] kcfg number: %d\n", core->id, core->kcfg_num);

	/* read this property to set whether core work thread is real-time*/
	core->rt = of_property_read_bool(node, "real-time");

	/* allocate memory for kcfgs and set specific cfg size*/
	for (i = 0; i < core->kcfg_num; i++) {
		kcfg = kzalloc(sizeof(*kcfg), GFP_KERNEL);
		if (!kcfg) {
			ret = -ENOMEM;
			goto err;
		}
		gsp_kcfg_init(kcfg, core);
		list_add_tail(&kcfg->sibling, &core->kcfgs);

		kcfg->cfg_size = core->cfg_size;
		kcfg->cfg = kzalloc(kcfg->cfg_size, GFP_KERNEL);
		if (!kcfg->cfg) {
			ret = -ENOMEM;
			goto err;
		}
		gsp_workqueue_load(&core->wq, kcfg);
	}

	return 0;

err:
	gsp_core_free(core);
	*pcore = NULL;
	return ret;
}

static int gsp_core_parse_dt(struct gsp_core *core)
{
	core->base = of_iomap(core->node, 0);
	if (!core->base) {
		GSP_ERR("core[%d] map registers failed\n", core->id);
		return -ENOMEM;
	}

	core->irq = irq_of_parse_and_map(core->node, 0);
	if (core->irq <= 0) {
		GSP_ERR("core[%d] parse irq failed\n", core->id);
		return -EINVAL;
	}

	return core->ops->parse_dt(core);
}

int gsp_core_init(struct gsp_core *core)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct kthread_worker *worker;
	int ret;

	ret = gsp_core_parse_dt(core);
	if (ret) {
		GSP_ERR("core[%d] parse dt failed\n", core->id);
		return ret;
	}

	ret = core->ops->init(core);
	if (ret) {
		GSP_ERR("core[%d] init failed\n", core->id);
		return ret;
	}

	core->timeline = gsp_sync_timeline_create(core->name);
	if (!core->timeline) {
		GSP_ERR("core[%d] create timeline failed\n", core->id);
		return -ENOMEM;
	}

	timer_setup(&core->timer, gsp_core_hang_handler, 0);
	kthread_init_work(&core->trigger, gsp_core_trigger);
	kthread_init_work(&core->release, gsp_core_release);
	kthread_init_work(&core->recover, gsp_core_recover);

	worker = kthread_create_worker(0, "gsp-core%d", core->id);
	if (IS_ERR(worker)) {
		ret = PTR_ERR(worker);
		goto err_timeline;
	}
	core->kworker = worker;

	/*
	 * core dts will indicate if this core should run the core kthread
	 * with high (realtime) priority to reduce the cpu schedule time.
	 * Without this setting core kthread will remain at default priority.
	 */
	if (core->rt) {
		GSP_INFO("run core kthread with realtime priority\n");
		sched_setscheduler(worker->task, SCHED_FIFO, &param);
	}

	ret = request_irq(core->irq, gsp_core_irq_handler, IRQF_SHARED,
			  "gsp", core);
	if (ret) {
		GSP_ERR("core[%d] request irq failed\n", core->id);
		goto err_worker;
	}
	core->irq_requested = true;

	core->inited = true;
	GSP_INFO("initialize core[%d] success\n", core->id);
	return 0;

err_worker:
	kthread_destroy_worker(core->kworker);
	core->kworker = NULL;
err_timeline:
	gsp_sync_timeline_destroy(core->timeline);
	core->timeline = NULL;
	return ret;
}

/*
 * Stop the core: no new kcfg starts, the one in flight is given a chance
 * to finish (or the hang timer ends it), and the queued ones are dropped
 * with their fences signaled.
 */
void gsp_core_deinit(struct gsp_core *core)
{
	struct gsp_kcfg *kcfg;

	if (!core->inited)
		return;

	gsp_workqueue_shutdown(&core->wq);

	mutex_lock(&core->lock);
	core->stopping = true;
	mutex_unlock(&core->lock);

	if (!wait_event_timeout(core->idle_wait, !core->busy,
				msecs_to_jiffies(GSP_CORE_TIMER_OUT +
						 GSP_WAIT_FENCE_TIMEOUT)))
		GSP_ERR("core[%d] still busy at deinit\n", core->id);

	if (core->irq_requested) {
		free_irq(core->irq, core);
		core->irq_requested = false;
	}
	del_timer_sync(&core->timer);
	/* flushes the queued works, and stops the thread */
	kthread_destroy_worker(core->kworker);
	core->kworker = NULL;

	while ((kcfg = gsp_workqueue_pull(&core->wq))) {
		gsp_kcfg_cleanup(kcfg, -ECANCELED);
		gsp_kcfg_complete(kcfg, -ECANCELED);
		gsp_kcfg_put(kcfg);
	}

	gsp_sync_timeline_destroy(core->timeline);
	core->timeline = NULL;
	core->inited = false;
}

/* the least loaded core takes the kcfgs of an ioctl */
struct gsp_core *gsp_core_select(struct gsp_dev *gsp)
{
	struct gsp_core *core, *result = NULL;
	int best = -1;

	list_for_each_entry(core, &gsp->cores, list) {
		unsigned long flags;
		int free;

		spin_lock_irqsave(&core->wq.lock, flags);
		free = core->wq.empty_cnt;
		spin_unlock_irqrestore(&core->wq.lock, flags);

		if (free > best) {
			best = free;
			result = core;
		}
	}

	return result;
}

/*
 * System suspend: no kcfg starts any more; wait for the one in flight so
 * that the clocks are off and nothing is mapped when the system sleeps.
 */
int gsp_core_suspend(struct gsp_core *core)
{
	mutex_lock(&core->lock);
	core->suspended = true;
	mutex_unlock(&core->lock);

	if (!wait_event_timeout(core->idle_wait, !core->busy,
				msecs_to_jiffies(GSP_CORE_SUSPEND_WAIT))) {
		GSP_ERR("core[%d] suspend wait timeout\n", core->id);
		gsp_core_resume(core);
		return -EBUSY;
	}

	return 0;
}

void gsp_core_resume(struct gsp_core *core)
{
	/* the coef cache is stale after the core lost power */
	core->force_calc = 1;

	mutex_lock(&core->lock);
	core->suspended = false;
	mutex_unlock(&core->lock);

	gsp_core_work(core);
}
