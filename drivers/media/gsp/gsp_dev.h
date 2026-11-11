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

#ifndef _GSP_DEV_H
#define _GSP_DEV_H

#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/fs.h>

struct gsp_core;
struct gsp_dev;
struct gsp_interface;

#define for_each_gsp_core(core, gsp) \
	list_for_each_entry((core), &(gsp)->cores, list)

#define for_each_gsp_core_safe(core, tmp, gsp) \
	list_for_each_entry_safe((core), (tmp), &(gsp)->cores, list)

/* the kcfg count of an ioctl is a 3-bit field of its number */
#define GSP_IO_MAX_CNT 7
#define GSP_MAX_CORE_CNT 4

#define GSP_DEVICE_NAME "gsp"

struct gsp_dev {
	uint32_t core_cnt;
	uint32_t io_cnt;

	struct miscdevice mdev;
	struct device *dev;

	struct gsp_interface *interface;

	struct list_head cores;
};

struct device *gsp_dev_to_device(struct gsp_dev *gsp);
struct gsp_interface *gsp_dev_to_interface(struct gsp_dev *gsp);

struct gsp_core *gsp_dev_to_core(struct gsp_dev *gsp, int index);
#endif
