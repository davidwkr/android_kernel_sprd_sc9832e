/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Screen on/off notifier of the sprd DRM display driver.
 *
 * SPRD_PANEL_EVENT_ON is sent once the DSI encoder has been enabled (panel
 * powered and initialised, display pipe running); SPRD_PANEL_EVENT_OFF once
 * it has been disabled (display pipe stopped, panel in sleep and powered
 * down). This is the same blank/unblank meaning as the 4.4 ADF_EVENT_BLANK
 * with DRM_MODE_DPMS_ON / DRM_MODE_DPMS_OFF. A panel low-power idle mode
 * (AOD) that keeps the pipe running does not send any event.
 *
 * Callbacks run in process context, may sleep, and must not call back into
 * the display driver or register/unregister notifiers. They are not called
 * for ESD recovery or for mode-only modesets. Registering calls the new
 * callback once with the current state (ON before the first event), in order
 * with every later event.
 */
#ifndef _LINUX_SPRD_PANEL_NOTIFIER_H
#define _LINUX_SPRD_PANEL_NOTIFIER_H

#include <linux/notifier.h>
#include <linux/types.h>

enum sprd_panel_event {
	SPRD_PANEL_EVENT_OFF = 1,
	SPRD_PANEL_EVENT_ON,
};

#if IS_ENABLED(CONFIG_SPRD_PANEL_NOTIFIER)
int sprd_panel_notifier_register(struct notifier_block *nb);
int sprd_panel_notifier_unregister(struct notifier_block *nb);
/* Last event sent: true if the screen is on (true before the first event). */
bool sprd_panel_screen_is_on(void);
#else
static inline int sprd_panel_notifier_register(struct notifier_block *nb)
{
	return -ENODEV;
}

static inline int sprd_panel_notifier_unregister(struct notifier_block *nb)
{
	return 0;
}

static inline bool sprd_panel_screen_is_on(void)
{
	return true;
}
#endif

#endif /* _LINUX_SPRD_PANEL_NOTIFIER_H */
