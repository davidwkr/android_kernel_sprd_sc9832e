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

#include <linux/slab.h>
#include "gsp_debug.h"
#include "gsp_kcfg.h"
#include "gsp_workqueue.h"

void gsp_workqueue_init(struct gsp_workqueue *wq)
{
	spin_lock_init(&wq->lock);
	INIT_LIST_HEAD(&wq->fill_head);
	INIT_LIST_HEAD(&wq->empty_head);
	init_waitqueue_head(&wq->empty_wait);
	wq->fill_cnt = 0;
	wq->empty_cnt = 0;
	wq->closed = 0;
}

/* only while the core is set up: hand a kcfg to the pool and tag it */
void gsp_workqueue_load(struct gsp_workqueue *wq, struct gsp_kcfg *kcfg)
{
	list_add_tail(&kcfg->list, &wq->empty_head);
	kcfg->tag = wq->empty_cnt++;
	GSP_INFO("workqueue load kcfg[%d]\n", kcfg->tag);
}

static bool gsp_workqueue_ready(struct gsp_workqueue *wq, int num)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&wq->lock, flags);
	ready = wq->closed || wq->empty_cnt >= num;
	spin_unlock_irqrestore(&wq->lock, flags);

	return ready;
}

int gsp_workqueue_acquire(struct gsp_workqueue *wq, int num,
			  struct gsp_kcfg **kcfgs, unsigned int timeout_ms)
{
	long left = msecs_to_jiffies(timeout_ms);
	unsigned long flags;
	int i;

	for (;;) {
		spin_lock_irqsave(&wq->lock, flags);
		if (wq->closed) {
			spin_unlock_irqrestore(&wq->lock, flags);
			return wq->closed;
		}
		if (wq->empty_cnt >= num) {
			for (i = 0; i < num; i++) {
				kcfgs[i] = list_first_entry(&wq->empty_head,
							    struct gsp_kcfg,
							    list);
				list_del_init(&kcfgs[i]->list);
			}
			wq->empty_cnt -= num;
			spin_unlock_irqrestore(&wq->lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&wq->lock, flags);

		GSP_DEBUG("wait because workqueue is exhausted\n");
		left = wait_event_interruptible_timeout(wq->empty_wait,
					gsp_workqueue_ready(wq, num), left);
		if (left < 0)
			return left;
		if (left == 0)
			return -ETIMEDOUT;
	}
}

void gsp_workqueue_put(struct gsp_workqueue *wq, struct gsp_kcfg *kcfg)
{
	unsigned long flags;

	spin_lock_irqsave(&wq->lock, flags);
	list_add_tail(&kcfg->list, &wq->empty_head);
	wq->empty_cnt++;
	spin_unlock_irqrestore(&wq->lock, flags);

	wake_up(&wq->empty_wait);
}

/* all or none, and none once the queue is closed: nothing is left behind it */
int gsp_workqueue_push(struct gsp_workqueue *wq, struct gsp_kcfg **kcfgs,
		       int cnt)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&wq->lock, flags);
	if (wq->closed) {
		int err = wq->closed;

		spin_unlock_irqrestore(&wq->lock, flags);
		return err;
	}
	for (i = 0; i < cnt; i++) {
		list_add_tail(&kcfgs[i]->list, &wq->fill_head);
		wq->fill_cnt++;
	}
	spin_unlock_irqrestore(&wq->lock, flags);

	return 0;
}

struct gsp_kcfg *gsp_workqueue_pull(struct gsp_workqueue *wq)
{
	struct gsp_kcfg *kcfg = NULL;
	unsigned long flags;

	spin_lock_irqsave(&wq->lock, flags);
	if (!list_empty(&wq->fill_head)) {
		kcfg = list_first_entry(&wq->fill_head, struct gsp_kcfg, list);
		list_del_init(&kcfg->list);
		wq->fill_cnt--;
	}
	spin_unlock_irqrestore(&wq->lock, flags);

	return kcfg;
}

int gsp_workqueue_is_filled(struct gsp_workqueue *wq)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&wq->lock, flags);
	ret = !list_empty(&wq->fill_head);
	spin_unlock_irqrestore(&wq->lock, flags);

	return ret;
}

/*
 * Fail the waiting and the later acquires and pushes with @err (negative).
 * The first error stays: a core which broke is not shut down later.
 */
void gsp_workqueue_close(struct gsp_workqueue *wq, int err)
{
	unsigned long flags;

	spin_lock_irqsave(&wq->lock, flags);
	if (!wq->closed)
		wq->closed = err;
	spin_unlock_irqrestore(&wq->lock, flags);

	wake_up_all(&wq->empty_wait);
}
