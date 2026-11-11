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

#ifndef _GSP_INTERFACE_H
#define _GSP_INTERFACE_H

#include <linux/of.h>
#include "gsp_core.h"

/*
 * The SoC specific glue around the gsp cores (module enable, reset, power
 * of the block the cores sit in). The 4.4 driver had one per SoC; only the
 * sharkle one is left, which has nothing to do.
 */
struct gsp_interface {
	char name[32];
	const struct gsp_interface_ops *ops;
	bool attached;
	struct gsp_dev *attached_dev;
};

struct gsp_interface_ops {
	int (*parse_dt)(struct gsp_interface *interface,
			struct device_node *node);
	int (*init)(struct gsp_interface *interface);
	int (*deinit)(struct gsp_interface *interface);
	int (*prepare)(struct gsp_interface *interface);
	int (*unprepare)(struct gsp_interface *interface);
	int (*reset)(struct gsp_interface *interface);
	void (*dump)(struct gsp_interface *interface);
};

int gsp_interface_is_attached(struct gsp_interface *interface);
char *gsp_interface_to_name(struct gsp_interface *interface);

int gsp_interface_attach(struct gsp_interface **interface,
			 struct gsp_dev *gsp);
void gsp_interface_detach(struct gsp_interface *interface);

int gsp_interface_prepare(struct gsp_interface *interface);
int gsp_interface_unprepare(struct gsp_interface *interface);
int gsp_interface_reset(struct gsp_interface *interface);
void gsp_interface_dump(struct gsp_interface *interface);

int gsp_interface_init(struct gsp_interface *interface);
int gsp_interface_deinit(struct gsp_interface *interface);

#endif
