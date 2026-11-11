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

#ifndef _GSP_CORE_H
#define _GSP_CORE_H

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <video/gsp_cfg.h>
#include "gsp_workqueue.h"

#define GSP_CORE_SUSPEND_WAIT 3000 /* 3000 ms */
#define GSP_CORE_RELEASE_WAIT 3000 /* 3000 ms */
#define GSP_CORE_TIMER_OUT 2800 /* 2800 ms */
#define GSP_CORE_MAX_KCFG_NUM 32

struct gsp_core_ops;
struct gsp_dev;
struct gsp_kcfg;
struct gsp_sync_timeline;

/* what the core ops return from irq_ack */
#define GSP_IRQ_DONE	BIT(0)
#define GSP_IRQ_ERR	BIT(1)

/* what the core ops return from trigger */
enum gsp_err_code {
	GSP_NO_ERR = 0,

	GSP_K_HW_BUSY_ERR = 0x200,
	GSP_K_HW_HANG_ERR,
};

/**
 * struct gsp_core - gsp core
 * @wq:		the pool and the fill queue of the kcfgs
 * @parent:	be part of struct gsp_dev
 * @ops:	indicate gsp core operations
 *
 * All the hardware work is done by one kthread worker: trigger starts the
 * oldest filled kcfg, release (from the irq) or recover (the hang timer)
 * ends it. They never run concurrently, and an event which is not of the
 * job in flight is dropped, so a kcfg is ended exactly once.
 */
struct gsp_core {
	char name[32];
	int id;
	int kcfg_num;

	struct gsp_capability *capa;

	struct gsp_dev *parent;
	/* linked with parent header */
	struct list_head list;

	struct device_node *node;
	struct device *dev;

	int irq;
	bool irq_requested;
	void __iomem *base;

	const struct gsp_core_ops *ops;

	/* size of the kernel side cfg and of the cfg the ioctl passes */
	size_t cfg_size;
	size_t user_cfg_size;

	struct gsp_workqueue wq;
	/* every kcfg of the core, to free them */
	struct list_head kcfgs;

	struct kthread_worker *kworker;
	struct kthread_work trigger;
	struct kthread_work release;
	struct kthread_work recover;
	struct timer_list timer;
	/* to keep the order of the release fences the order of the queue */
	struct mutex submit_lock;

	/* protects the fields below up to hw_running */
	struct mutex lock;
	/* the kcfg handled by the core, between trigger and release */
	struct gsp_kcfg *current_kcfg;
	bool busy;
	bool suspended;
	bool stopping;
	/* the reset failed: the hardware may still do DMA, so nothing is freed */
	bool broken;
	wait_queue_head_t idle_wait;

	/* the hardware may raise its irq: set from trigger to the end */
	atomic_t hw_running;
	/*
	 * The events of the hardware and of the hang timer name the job they
	 * belong to by its generation, which grows for every job started, so
	 * that the worker ignores the ones of a job which is over already.
	 * event_lock protects them.
	 */
	spinlock_t event_lock;
	u32 job_gen;
	/* GSP_IRQ_* of the irq, and the job it was for */
	u32 irq_status;
	u32 irq_gen;
	/* the job the hang timer fired for */
	u32 hang_gen;
	/* what the kcfg in flight has switched on, to undo it at the end */
	bool prepared;
	bool iommu_on;
	bool clk_on;

	struct gsp_sync_timeline *timeline;

	/* to indicate whether core kthread priority is real-time */
	bool rt;
	/* the coef cache is stale after the core lost power */
	int force_calc;
	bool inited;
};

/**
 * struct gsp_core_ops - gsp core operation
 * @alloc:	allocate the core and its capability
 * @parse_dt:	get the clocks
 * @init:	initialize the cfgs of the kcfgs and the capability
 * @free:	undo alloc and parse_dt
 * @copy:	copy and range-check the user's cfg, no buffer involved
 * @verify:	check the cfg against the sizes of the layers' buffers
 * @intercept:	user address of the release fence fd of the cfg
 */
struct gsp_core_ops {
	int (*alloc)(struct gsp_core **core, struct device_node *node);
	int (*parse_dt)(struct gsp_core *core);
	int (*init)(struct gsp_core *core);
	void (*free)(struct gsp_core *core);

	int (*copy)(struct gsp_kcfg *kcfg, void *arg, int index);
	int (*verify)(struct gsp_kcfg *kcfg);

	int (*trigger)(struct gsp_core *core);
	/* after the irq: nonzero if the hardware flagged an error */
	int (*release)(struct gsp_core *core);
	/*
	 * from the handler: clear and disable what the status says is
	 * raised, and return it as GSP_IRQ_*, or 0 if it is not ours
	 */
	int (*irq_ack)(struct gsp_core *core);
	/* mask the interrupts, keeping their status */
	void (*irq_mask)(struct gsp_core *core);
	/* after the mask: clear what is still pending, from the raw status */
	int (*irq_collect)(struct gsp_core *core);

	int (*enable)(struct gsp_core *core);
	void (*disable)(struct gsp_core *core);

	int __user *(*intercept)(void __user *arg, int index);
	void (*dump)(struct gsp_core *core);
	int (*reset)(struct gsp_core *core);
};

int gsp_core_to_id(struct gsp_core *core);

int gsp_core_is_idle(struct gsp_core *core);

void gsp_core_work(struct gsp_core *core);

int gsp_core_alloc(struct gsp_core **core,
		   const struct gsp_core_ops *ops,
		   struct device_node *node);
void gsp_core_free(struct gsp_core *core);
int gsp_core_init(struct gsp_core *core);
void gsp_core_deinit(struct gsp_core *core);

struct gsp_core *gsp_core_select(struct gsp_dev *gsp);

int gsp_core_suspend(struct gsp_core *core);
void gsp_core_resume(struct gsp_core *core);

void gsp_core_reg_write(void __iomem *addr, u32 value);
u32 gsp_core_reg_read(void __iomem *addr);
void gsp_core_reg_update(void __iomem *addr, u32 value, u32 mask);

struct gsp_capability *gsp_core_get_capability(struct gsp_core *core);

#endif
