/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPRD_PANEL_NOTIFIER_INT_H_
#define _SPRD_PANEL_NOTIFIER_INT_H_

#include <linux/sprd_panel_notifier.h>

void sprd_panel_notify(enum sprd_panel_event event);
void sprd_panel_notify_quiet(bool quiet_on);

#endif
