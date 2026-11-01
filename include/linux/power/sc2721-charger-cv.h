/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_POWER_SC2721_CHARGER_CV_H__
#define __LINUX_POWER_SC2721_CHARGER_CV_H__

#include <linux/kconfig.h>

/*
 * Generation of the SC2721 CV request/override, bumped on every change.
 * It wraps and is only compared for equality (gauge resync).
 */
#if IS_REACHABLE(CONFIG_CHARGER_SC2720)
unsigned int sc2721_charger_cv_generation(void);
#else
static inline unsigned int sc2721_charger_cv_generation(void)
{
	return 0;
}
#endif

#endif
