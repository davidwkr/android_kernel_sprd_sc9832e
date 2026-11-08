// SPDX-License-Identifier: GPL-2.0
/*
 * Screen on/off notifier of the sprd DRM display driver, see
 * include/linux/sprd_panel_notifier.h.
 */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/sprd_panel_notifier.h>

#include "sprd_panel_notifier.h"

static BLOCKING_NOTIFIER_HEAD(sprd_panel_chain);
static bool screen_on = true;
/* ESD recovery disables and enables the encoder; it is not a screen change */
static atomic_t quiet = ATOMIC_INIT(0);

int sprd_panel_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&sprd_panel_chain, nb);
}
EXPORT_SYMBOL_GPL(sprd_panel_notifier_register);

int sprd_panel_notifier_unregister(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&sprd_panel_chain, nb);
}
EXPORT_SYMBOL_GPL(sprd_panel_notifier_unregister);

bool sprd_panel_screen_is_on(void)
{
	return READ_ONCE(screen_on);
}
EXPORT_SYMBOL_GPL(sprd_panel_screen_is_on);

void sprd_panel_notify_quiet(bool quiet_on)
{
	if (quiet_on)
		atomic_inc(&quiet);
	else
		atomic_dec(&quiet);
}

/* Process context only: the chain is blocking. Call without driver locks. */
void sprd_panel_notify(enum sprd_panel_event event)
{
	if (atomic_read(&quiet))
		return;

	WRITE_ONCE(screen_on, event == SPRD_PANEL_EVENT_ON);
	blocking_notifier_call_chain(&sprd_panel_chain, event, NULL);
}
