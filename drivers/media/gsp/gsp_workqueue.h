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

#ifndef _GSP_WORKQUEUE_H
#define _GSP_WORKQUEUE_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

struct gsp_kcfg;

/*
 * The kcfgs of one core: the pool of free ones (empty) and the queue of
 * the ones the ioctl has filled and the core still has to run (fill).
 */
struct gsp_workqueue {
	spinlock_t lock;
	struct list_head fill_head;
	struct list_head empty_head;
	int fill_cnt;
	int empty_cnt;
	wait_queue_head_t empty_wait;
	/* nothing is accepted any more: the negative errno the callers get, or 0 */
	int closed;
};

void gsp_workqueue_init(struct gsp_workqueue *wq);
void gsp_workqueue_load(struct gsp_workqueue *wq, struct gsp_kcfg *kcfg);

/* take @num free kcfgs, waiting at most @timeout_ms for them */
int gsp_workqueue_acquire(struct gsp_workqueue *wq, int num,
			  struct gsp_kcfg **kcfgs, unsigned int timeout_ms);
void gsp_workqueue_put(struct gsp_workqueue *wq, struct gsp_kcfg *kcfg);

int gsp_workqueue_push(struct gsp_workqueue *wq, struct gsp_kcfg **kcfgs,
		       int cnt);
struct gsp_kcfg *gsp_workqueue_pull(struct gsp_workqueue *wq);

int gsp_workqueue_is_filled(struct gsp_workqueue *wq);
void gsp_workqueue_close(struct gsp_workqueue *wq, int err);

#endif
