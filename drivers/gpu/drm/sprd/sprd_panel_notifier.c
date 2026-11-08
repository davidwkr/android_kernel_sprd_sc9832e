// SPDX-License-Identifier: GPL-2.0
/*
 * Screen on/off notifier of the sprd DRM display driver, see
 * include/linux/sprd_panel_notifier.h.
 */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/sprd_panel_notifier.h>

#include "sprd_panel_notifier.h"

static BLOCKING_NOTIFIER_HEAD(sprd_panel_chain);
/*
 * Serialises a state change and its delivery against registration, so a
 * listener's initial state can never be overtaken by an older event.
 */
static DEFINE_MUTEX(sprd_panel_lock);
static bool screen_on = true;
/* ESD recovery disables and enables the encoder; it is not a screen change */
static struct task_struct *quiet_task;

static enum sprd_panel_event sprd_panel_state(void)
{
	return screen_on ? SPRD_PANEL_EVENT_ON : SPRD_PANEL_EVENT_OFF;
}

/* The callback is also called once, at registration, with the current state. */
int sprd_panel_notifier_register(struct notifier_block *nb)
{
	int ret;

	mutex_lock(&sprd_panel_lock);
	ret = blocking_notifier_chain_register(&sprd_panel_chain, nb);
	if (!ret)
		nb->notifier_call(nb, sprd_panel_state(), NULL);
	mutex_unlock(&sprd_panel_lock);

	return ret;
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

/*
 * Only the encoder calls made by the ESD worker itself are silenced; a real
 * screen change from another task during recovery is still delivered.
 */
void sprd_panel_notify_quiet(bool quiet_on)
{
	WRITE_ONCE(quiet_task, quiet_on ? current : NULL);
}

/* Process context only: the chain is blocking. Call without driver locks. */
void sprd_panel_notify(enum sprd_panel_event event)
{
	if (READ_ONCE(quiet_task) == current)
		return;

	mutex_lock(&sprd_panel_lock);
	WRITE_ONCE(screen_on, event == SPRD_PANEL_EVENT_ON);
	blocking_notifier_call_chain(&sprd_panel_chain, event, NULL);
	mutex_unlock(&sprd_panel_lock);
}
