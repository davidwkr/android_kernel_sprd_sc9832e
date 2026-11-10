/*
 * Stand-ins for the dispc/dsi/dphy debug sysfs attribute groups
 * (sysfs_dispc.c, sysfs_dsi.c, sysfs_dphy.c, sysfs_refnotify.c), not
 * ported in this skeleton: they only expose extra tuning/debug files
 * under /sys/class/display, not anything adf_device_post()/adf_blank()
 * or the panel brightness path (sysfs_panel.c, which is ported) depend
 * on. sprd_dispc.c, sprd_dsi.c and sprd_dphy.c call these unconditionally
 * from probe, so they still need to exist and succeed.
 */

#include <linux/device.h>
#include <linux/errno.h>

#include "sysfs_display.h"

int sprd_dispc_sysfs_init(struct device *dev)
{
	return 0;
}

int sprd_dsi_sysfs_init(struct device *dev)
{
	return 0;
}

int sprd_dphy_sysfs_init(struct device *dev)
{
	return 0;
}
