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
#ifndef _GSP_SYNC_H
#define _GSP_SYNC_H

#include <linux/dma-fence.h>
#include <linux/types.h>

struct gsp_layer;
struct gsp_sync_timeline;

#define GSP_WAIT_FENCE_TIMEOUT 2900/* ms */
#define GSP_WAIT_FENCE_MAX 8

struct gsp_fence_data {
	/* release fence, signaled when the kcfg is done or dropped */
	struct dma_fence *sig_fen;
	/* acquire fences imported from the layers' wait_fd */
	struct dma_fence *wait_fen_arr[GSP_WAIT_FENCE_MAX];
	int wait_cnt;
};

struct gsp_sync_timeline *gsp_sync_timeline_create(const char *name);
void gsp_sync_timeline_destroy(struct gsp_sync_timeline *tl);

int gsp_sync_wait_fence_collect(struct gsp_fence_data *data, int fd);
int gsp_sync_fence_wait(struct gsp_fence_data *data);

struct dma_fence *gsp_sync_sig_fence_create(struct gsp_sync_timeline *tl);
void gsp_sync_fence_signal(struct gsp_fence_data *data, int status);

#endif
