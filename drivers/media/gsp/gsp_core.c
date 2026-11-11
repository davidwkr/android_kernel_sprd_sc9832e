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
 * Stop the hardware. If it cannot be done the core is broken for good: the
 * hardware may still do DMA, so what the kcfg holds is leaked on purpose.
 */
static void gsp_core_reset_hw(struct gsp_core *core)
{
	int ret = -ENODEV;

	if (core->ops->reset)
		ret = core->ops->reset(core);
	if (!ret)
		return;

	if (!core->broken)
		GSP_ERR("core[%d] reset failed: %d, the core is out of service and its buffers are leaked\n",
			core->id, ret);
	mutex_lock(&core->lock);
	core->broken = true;
	mutex_unlock(&core->lock);
	/* nothing is queued behind the job any more: the worker drains it */
	gsp_workqueue_close(&core->wq, -EIO);
}

/*
 * Fail the kcfgs nobody started: the hardware never touched them, so they
 * are released the normal way. Only from the worker, after the queue closed.
 */
static void gsp_core_drain(struct gsp_core *core, int status)
{
	struct gsp_kcfg *kcfg;

	while ((kcfg = gsp_workqueue_pull(&core->wq))) {
		gsp_kcfg_cleanup(kcfg, gsp_kcfg_batch_status(kcfg, status));
		gsp_kcfg_complete(kcfg, status);
		gsp_kcfg_put(kcfg);
	}
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
	/* the handler may be reading the registers: wait before the clocks go */
	synchronize_irq(core->irq);

	/*
	 * A core which could not be reset may still do DMA: keep the mapping,
	 * the buffers and the clocks, and only tell the waiters, the release
	 * fence included. The queued kcfgs never ran: fail them.
	 */
	if (core->broken) {
		gsp_sync_fence_signal(&kcfg->data, -EIO);
		gsp_kcfg_complete(kcfg, -EIO);
		mutex_lock(&core->lock);
		core->current_kcfg = NULL;
		core->busy = false;
		mutex_unlock(&core->lock);
		wake_up_all(&core->idle_wait);
		gsp_core_drain(core, -EIO);
		return;
	}

	gsp_kcfg_iommu_unmap(kcfg);
	gsp_kcfg_cleanup(kcfg, gsp_kcfg_batch_status(kcfg, status));
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
	unsigned long flags;
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

	/* no job starts once the core is stopping */
	mutex_lock(&core->lock);
	if (core->stopping) {
		mutex_unlock(&core->lock);
		return -ECANCELED;
	}
	mutex_unlock(&core->lock);

	/* a new job: the events of the former ones are not for it */
	spin_lock_irqsave(&core->event_lock, flags);
	core->job_gen++;
	core->irq_status = 0;
	spin_unlock_irqrestore(&core->event_lock, flags);

	atomic_set(&core->hw_running, 1);
	ret = core->ops->trigger(core);
	if (ret) {
		atomic_set(&core->hw_running, 0);
		if (ret == GSP_K_HW_BUSY_ERR)
			gsp_core_reset_hw(core);
		return -EIO;
	}

	/*
	 * Never arm the timer once the core is stopping: the teardown ends
	 * the job itself, and the timer must stay dead after it.
	 */
	mutex_lock(&core->lock);
	if (!core->stopping)
		mod_timer(&core->timer,
			  jiffies + msecs_to_jiffies(GSP_CORE_TIMER_OUT));
	mutex_unlock(&core->lock);
	return 0;
}

static void gsp_core_trigger(struct kthread_work *work)
{
	struct gsp_core *core = container_of(work, struct gsp_core, trigger);
	struct gsp_kcfg *kcfg;
	int ret;

	mutex_lock(&core->lock);
	if (core->busy || core->suspended || core->stopping || core->broken) {
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
	unsigned long flags;
	u32 status, gen;
	int ret = 0;

	/* the kcfg may have been ended already, and another one started */
	spin_lock_irqsave(&core->event_lock, flags);
	gen = core->irq_gen;
	spin_unlock_irqrestore(&core->event_lock, flags);
	if (!kcfg || !atomic_read(&core->hw_running) || gen != core->job_gen)
		return;

	/*
	 * Close the collection of events first: mask the interrupts and wait
	 * for the handler, so that an error after DONE is in the status that
	 * decides the result. Only the worker starts jobs, so it is still ours.
	 */
	if (core->ops->irq_mask)
		core->ops->irq_mask(core);
	atomic_set(&core->hw_running, 0);
	synchronize_irq(core->irq);

	/* what the hardware raised but the handler did not see: raw status only */
	status = core->ops->irq_collect(core);

	spin_lock_irqsave(&core->event_lock, flags);
	status |= core->irq_status;
	core->irq_status = 0;
	spin_unlock_irqrestore(&core->event_lock, flags);

	if (core->ops->release)
		ret = core->ops->release(core);
	if (status & GSP_IRQ_ERR) {
		GSP_ERR("core[%d] kcfg[%d] hardware error\n", core->id,
			gsp_kcfg_to_tag(kcfg));
		ret = -EIO;
	}

	gsp_core_finish(core, kcfg, ret ? -EIO : 0);
}

/* the hang timer fired: stop the hardware, then end the kcfg */
static void gsp_core_recover(struct kthread_work *work)
{
	struct gsp_core *core = container_of(work, struct gsp_core, recover);
	struct gsp_kcfg *kcfg = core->current_kcfg;
	unsigned long flags;
	u32 gen;

	spin_lock_irqsave(&core->event_lock, flags);
	gen = core->hang_gen;
	spin_unlock_irqrestore(&core->event_lock, flags);

	if (!kcfg || !atomic_read(&core->hw_running) || gen != core->job_gen)
		return;

	GSP_ERR("core[%d] kcfg[%d] hung, reset\n", core->id,
		gsp_kcfg_to_tag(kcfg));
	core->ops->dump(core);
	gsp_core_reset_hw(core);
	core->force_calc = 1;

	gsp_core_finish(core, kcfg, -ETIMEDOUT);
}

/* the removal: cancel the job in flight and fail the queued ones */
static void gsp_core_shutdown(struct kthread_work *work)
{
	struct gsp_core *core = container_of(work, struct gsp_core, shutdown);
	struct gsp_kcfg *kcfg = core->current_kcfg;

	if (kcfg) {
		GSP_ERR("core[%d] kcfg[%d] still busy at deinit, reset\n",
			core->id, gsp_kcfg_to_tag(kcfg));
		atomic_set(&core->hw_running, 0);
		gsp_core_reset_hw(core);
		core->force_calc = 1;
		gsp_core_finish(core, kcfg, -ECANCELED);
	}

	gsp_core_drain(core, -ECANCELED);
}

static void gsp_core_hang_handler(struct timer_list *t)
{
	struct gsp_core *core = from_timer(core, t, timer);
	unsigned long flags;

	spin_lock_irqsave(&core->event_lock, flags);
	core->hang_gen = core->job_gen;
	spin_unlock_irqrestore(&core->event_lock, flags);

	kthread_queue_work(core->kworker, &core->recover);
}

static irqreturn_t gsp_core_irq_handler(int irq, void *data)
{
	struct gsp_core *core = data;
	unsigned long flags;
	int status;

	/* the line is shared, and the hardware only runs for a kcfg */
	if (!atomic_read(&core->hw_running))
		return IRQ_NONE;

	status = core->ops->irq_ack(core);
	if (!status)
		return IRQ_NONE;

	spin_lock_irqsave(&core->event_lock, flags);
	core->irq_status |= status;
	core->irq_gen = core->job_gen;
	spin_unlock_irqrestore(&core->event_lock, flags);

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
	spin_lock_init(&core->event_lock);
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
	kthread_init_work(&core->shutdown, gsp_core_shutdown);

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
 * Stop the core. The producers go first: no ioctl queues, no job starts nor
 * arms the timer, the irq line is quiet and the timer is dead. Then the
 * worker, the only one which ends jobs, cancels the one in flight (the
 * hardware is reset) and fails the queued ones, and only then is anything
 * freed.
 */
void gsp_core_deinit(struct gsp_core *core)
{
	if (!core->inited)
		return;

	gsp_workqueue_close(&core->wq, -ESHUTDOWN);

	mutex_lock(&core->lock);
	core->stopping = true;
	mutex_unlock(&core->lock);

	/*
	 * The line is quiet from here: the hardware is reset by the worker
	 * if it runs, and the handler cannot queue a release any more.
	 */
	disable_irq(core->irq);
	/* the timer is armed under the lock which stopping is set under */
	del_timer_sync(&core->timer);

	kthread_queue_work(core->kworker, &core->shutdown);
	kthread_flush_worker(core->kworker);

	if (core->irq_requested) {
		free_irq(core->irq, core);
		core->irq_requested = false;
	}
	kthread_destroy_worker(core->kworker);
	core->kworker = NULL;

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

		/* a broken core is only picked when it is the last one */
		if (core->broken)
			free = -1;
		if (free > best || !result) {
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
