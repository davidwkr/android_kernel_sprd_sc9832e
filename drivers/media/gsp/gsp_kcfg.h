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

#ifndef _GSP_KCFG_H
#define _GSP_KCFG_H

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <video/gsp_cfg.h>
#include "gsp_sync.h"

struct gsp_core;
struct gsp_workqueue;

#define GSP_WAIT_COMPLETION_TIMEOUT msecs_to_jiffies(3000)

/*
 * One cfg of a trigger ioctl on its way through the core.
 *
 * The reference count decides when the kcfg goes back to the pool of its
 * core: the ioctl owns the first one, the core takes another when the kcfg
 * is pushed, and a sync ioctl keeps its own until it has read the status.
 * Whoever drops the last one releases what the kcfg still holds.
 */
struct gsp_kcfg {
	struct gsp_cfg *cfg;
	size_t cfg_size;

	/* pool or fill queue of the core, see gsp_workqueue */
	struct list_head list;
	/* all the kcfgs of the core, only to free them */
	struct list_head sibling;

	struct kref ref;
	int tag;

	bool async;
	/* the last one of the ioctl: it carries the release fence */
	bool last;

	struct gsp_core *bind_core;

	/*
	 * The last kcfg of the ioctl, which carries the release fence: the
	 * ones before it hand it their first error, as the core ends them
	 * before it, in the order of the queue.
	 */
	struct gsp_kcfg *tail;
	int batch_status;

	/* result of the run, valid after complete */
	int status;
	struct completion complete;

	struct gsp_fence_data data;
};

int gsp_kcfg_to_tag(struct gsp_kcfg *kcfg);
int gsp_kcfg_is_async(struct gsp_kcfg *kcfg);

void gsp_kcfg_init(struct gsp_kcfg *kcfg, struct gsp_core *core);
/* a kcfg taken from the pool: one reference for the ioctl */
void gsp_kcfg_reinit(struct gsp_kcfg *kcfg);

void gsp_kcfg_get(struct gsp_kcfg *kcfg);
void gsp_kcfg_put(struct gsp_kcfg *kcfg);

/* @arg is the user's kcfg array copied to the kernel */
int gsp_kcfg_fill(struct gsp_kcfg *kcfg, void *arg, int index,
		  bool async, bool last);

int gsp_kcfg_fence_wait(struct gsp_kcfg *kcfg);

int gsp_kcfg_iommu_map(struct gsp_kcfg *kcfg);
void gsp_kcfg_iommu_unmap(struct gsp_kcfg *kcfg);

/* drop the dma-bufs and the fences, signal the release fence with @status */
void gsp_kcfg_cleanup(struct gsp_kcfg *kcfg, int status);

/* what the release fence gets: the first error of the batch, or @status */
int gsp_kcfg_batch_status(struct gsp_kcfg *kcfg, int status);

void gsp_kcfg_complete(struct gsp_kcfg *kcfg, int status);
int gsp_kcfg_wait(struct gsp_kcfg *kcfg);

#endif
