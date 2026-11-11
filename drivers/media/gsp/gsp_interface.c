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

#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "gsp_core.h"
#include "gsp_dev.h"
#include "gsp_debug.h"
#include "gsp_interface.h"
#include "gsp_interface/gsp_interface_sharkle.h"

static const struct gsp_interface_ops gsp_interface_sharkle_ops = {
	.parse_dt = gsp_interface_sharkle_parse_dt,
	.init = gsp_interface_sharkle_init,
	.deinit = gsp_interface_sharkle_deinit,
	.prepare = gsp_interface_sharkle_prepare,
	.unprepare = gsp_interface_sharkle_unprepare,
	.reset = gsp_interface_sharkle_reset,
	.dump = gsp_interface_sharkle_dump,
};

int gsp_interface_is_attached(struct gsp_interface *interface)
{
	return interface && interface->attached;
}

char *gsp_interface_to_name(struct gsp_interface *interface)
{
	return interface->name;
}

int gsp_interface_attach(struct gsp_interface **interface,
			 struct gsp_dev *gsp)
{
	struct device *dev = gsp_dev_to_device(gsp);
	struct gsp_interface *intf;
	struct gsp_core *core;
	int ret;

	if (!of_device_is_compatible(dev->of_node,
				     "sprd,gsp-lite_r2p0-sharkle")) {
		GSP_WARN("no match interface for gsp\n");
		return -ENODEV;
	}

	intf = kzalloc(sizeof(struct gsp_interface_sharkle), GFP_KERNEL);
	if (!intf) {
		GSP_ERR("alloc interface[%s] failed\n", GSP_SHARKLE);
		return -ENOMEM;
	}
	intf->ops = &gsp_interface_sharkle_ops;
	strlcpy(intf->name, GSP_SHARKLE, sizeof(intf->name));

	/* here get core[0] device node as default */
	core = gsp_dev_to_core(gsp, 0);
	ret = intf->ops->parse_dt(intf, core->node);
	if (ret) {
		GSP_ERR("interface[%s] parse dt failed\n", intf->name);
		kfree(intf);
		return ret;
	}

	intf->attached = true;
	intf->attached_dev = gsp;
	*interface = intf;
	return 0;
}

void gsp_interface_detach(struct gsp_interface *interface)
{
	kfree(interface);
}

int gsp_interface_prepare(struct gsp_interface *interface)
{
	if (!gsp_interface_is_attached(interface) || !interface->ops->prepare)
		return -EINVAL;

	return interface->ops->prepare(interface);
}

int gsp_interface_unprepare(struct gsp_interface *interface)
{
	if (!gsp_interface_is_attached(interface) ||
	    !interface->ops->unprepare)
		return -EINVAL;

	return interface->ops->unprepare(interface);
}

int gsp_interface_reset(struct gsp_interface *interface)
{
	if (!gsp_interface_is_attached(interface) || !interface->ops->reset)
		return -EINVAL;

	return interface->ops->reset(interface);
}

void gsp_interface_dump(struct gsp_interface *interface)
{
	if (gsp_interface_is_attached(interface) && interface->ops->dump)
		interface->ops->dump(interface);
}

int gsp_interface_init(struct gsp_interface *interface)
{
	if (!gsp_interface_is_attached(interface) || !interface->ops->init)
		return -EINVAL;

	return interface->ops->init(interface);
}

int gsp_interface_deinit(struct gsp_interface *interface)
{
	if (!gsp_interface_is_attached(interface) || !interface->ops->deinit)
		return -EINVAL;

	return interface->ops->deinit(interface);
}
