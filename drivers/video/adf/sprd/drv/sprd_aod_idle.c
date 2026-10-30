// SPDX-License-Identifier: GPL-2.0
/*
 * sprd_aod_idle: panel low-power idle mode (AOD) switch and double-tap wake
 * for SC9832E watches with a CO5300 MIPI-DSI OLED panel (DM63).
 *
 * In-tree port of the out-of-tree aod_idle.ko of the DM63 stock firmware. The
 * userspace ABI is unchanged:
 *
 *   /sys/kernel/aod_idle/idle             0/1: panel idle mode off/on
 *                                         (DCS 0x38 / 0x39 on page 0)
 *   /sys/kernel/aod_idle/idle_brightness  0..255, DCS 0x51 level used while
 *                                         idle (0 = framework's doze level)
 *   /sys/kernel/aod_idle/dtap             0/1: double tap to wake
 *   /sys/kernel/aod_idle/dtap_ms          100..2000, max tap interval
 *   /sys/kernel/aod_idle/status           one-line state dump
 *   /sys/kernel/aod_idle/peek             debug: write a DCS reg, read value
 *   input device "aod_dtap"               KEY_WAKEUP press+release
 *
 * While idle is on, an input filter on the "cst0xx_ts" (DM63) or "hyn_ts"
 * (DW100U) touch screen swallows all contacts; two touch-downs within
 * dtap_ms emit KEY_WAKEUP.
 *
 * DSI commands are only sent while the panel and DSI platform devices are
 * runtime active (touching a powered-down DSI host can hang the SoC); both
 * are pinned for the whole transaction. Every transaction also holds the
 * OLED backlight's update_lock so it cannot interleave with brightness
 * updates, which end in the same DSI path.
 */
#include <linux/atomic.h>
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "disp_notify.h"
#include "sprd_panel.h"
#include "sprd_dsi.h"
#include "dsi/mipi_dsi_api.h"

#define DCS_GET_POWER_MODE	0x0a
#define DCS_IDLE_MODE_OFF	0x38
#define DCS_IDLE_MODE_ON	0x39
#define DCS_SET_BRIGHTNESS	0x51
#define POWER_MODE_IDLE		0x40	/* 0x9c normal -> 0xdc idle on co5300 */

/* touch screens that carry the double tap: DM63 (CST0xx), DW100U (Hynitron) */
static const char * const touch_names[] = { "cst0xx_ts", "hyn_ts" };

static struct panel_device *pd;
static struct backlight_device *bd;	/* may be NULL: no level restore */
static DEFINE_MUTEX(aod_lock);
static int idle_state;			/* last state we set */
static int last_err;
static int last_mode = -1;		/* last DCS 0x0a read-back */
static unsigned int n_on, n_off, n_refused;

/* panel brightness while idle (DCS 0x51), 0 = leave unchanged */
static int idle_brightness = 8;

/* double tap to wake */
static int dtap = 1, dtap_ms = 400;
static ktime_t dtap_last;
static bool dtap_have_last;
static unsigned int n_taps, n_dtaps;
static struct input_dev *wake_dev;
static struct wakeup_source *dtap_ws;
static struct work_struct dtap_work;
static bool dtap_registered;

static struct device *panel_pdev(void)
{
	return pd->dev.parent;		/* "sprd-panel-if", runtime PM owner */
}

static bool panel_powered(void)
{
	return READ_ONCE(panel_pdev()->power.runtime_status) == RPM_ACTIVE;
}

/*
 * Take a usage reference only if the device is RPM_ACTIVE (checked under
 * power.lock). rpm_suspend refuses while usage_count > 0, so it cannot power
 * down mid-sequence. Drop with pm_runtime_put().
 */
static bool pm_pin(struct device *d)
{
	unsigned long flags;
	bool ok;

	spin_lock_irqsave(&d->power.lock, flags);
	ok = d->power.runtime_status == RPM_ACTIVE;
	if (ok)
		atomic_inc(&d->power.usage_count);
	spin_unlock_irqrestore(&d->power.lock, flags);
	return ok;
}

/* pin the panel and the DSI host (dsi_runtime_suspend() uninits the DSI) */
static bool panel_get(void)
{
	if (!pm_pin(pd->intf))
		return false;
	if (!pm_pin(panel_pdev())) {
		pm_runtime_put(pd->intf);
		return false;
	}
	return true;
}

static void panel_put(void)
{
	/* async idle checks, like any other user */
	pm_runtime_put(panel_pdev());
	pm_runtime_put(pd->intf);
}

static struct sprd_dsi *dsi_ctx(void)
{
	return dev_get_drvdata(pd->intf);
}

/*
 * DCS short writes, like the stock module. It picked DCS or generic from the
 * byte at panel_info + 0x08 (non-zero = DCS). In this tree that offset is
 * panel_info.type (u8, SPRD_PANEL_TYPE_MIPI = 2), so the stock module always
 * took the DCS branch for a MIPI panel. 0x38/0x39/0x51 are DCS commands (the
 * mt107 AOD path uses 0x38/0x39 the same way); the init/brightness tables use
 * generic types (0x23/0x29, mipi_dsi_gen_write), which the CO5300 accepts as
 * well, but DCS is what ran on the watch. If the idle read-back does not show
 * 0xdc on hardware, try mipi_dsi_gen_write() here.
 */
static int send(struct sprd_dsi *dsi, u8 *buf, u16 len, unsigned int wait_ms)
{
	int ret = mipi_dsi_dcs_write(dsi, buf, len);

	if (wait_ms)
		msleep(wait_ms);
	return ret;
}

static int read_mode(struct sprd_dsi *dsi)
{
	u8 v = 0;
	int ret;

	ret = mipi_dsi_set_max_return_size(dsi, 1);
	if (ret < 0)
		return ret;
	ret = mipi_dsi_dcs_read(dsi, DCS_GET_POWER_MODE, &v, 1);
	return ret == 1 ? v : (ret < 0 ? ret : -EIO);
}

static void bl_lock(void)
{
	if (bd)
		mutex_lock(&bd->update_lock);
}

static void bl_unlock(void)
{
	if (bd)
		mutex_unlock(&bd->update_lock);
}

/* resend the backlight's current level, as backlight_update_status() does;
 * bl_lock() must be held */
static int bl_resend_locked(void)
{
	if (!bd)
		return 0;
	if (!bd->ops || !bd->ops->update_status)
		return -ENODEV;
	return bd->ops->update_status(bd);
}

/*
 * The panel is power-cycled and re-initialised by panel_suspend()/resume(),
 * which leaves it in normal mode. Forget idle_state on DISPC_RUN (panel just
 * re-initialised, screen turning on) so the touch filter does not keep
 * swallowing touches when userspace's idle=0 is late or refused. Not on
 * DISPC_STOP: as in stock, idle_state stays set through AOD -> screen off,
 * so only a double tap wakes the watch from off (cst0xx keeps reporting
 * touches while the panel is off). The root-only sysfs_dsi debug attributes
 * also send STOP/RUN pairs and can clear the state during AOD.
 * Userspace may still see idle=0 fail with -EIO while the panel is powered
 * down (refused, nothing sent); that is harmless, it only needs to retry
 * after unblank if it wants the idle brightness path's restore. Only atomics
 * here: this runs from the panel runtime PM callbacks.
 */
static int aod_disp_event(struct notifier_block *nb, unsigned long action,
			  void *data);
static struct notifier_block aod_nb = { .notifier_call = aod_disp_event };

/* policy changes bump this; the filter (the only writer of the tap history)
 * drops its history when it sees a new generation */
static atomic_t policy_gen = ATOMIC_INIT(0);

static void set_idle_state(int on)
{
	WRITE_ONCE(idle_state, on);
	atomic_inc(&policy_gen);
}

static int aod_disp_event(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	if (action == DISP_EVENT_DISPC_RUN && READ_ONCE(idle_state))
		set_idle_state(0);
	return NOTIFY_DONE;
}

static int set_idle(int on)
{
	struct sprd_dsi *dsi;
	u8 page0[] = { 0xfe, 0x00 };
	u8 idm[] = { on ? DCS_IDLE_MODE_ON : DCS_IDLE_MODE_OFF };
	int ret;

	if (!panel_get()) {
		n_refused++;
		return -EIO;
	}
	bl_lock();
	dsi = dsi_ctx();
	if (!dsi) {
		ret = -ENODEV;
		goto out;
	}
	mipi_dsi_lp_cmd_enable(dsi, 1);
	ret = send(dsi, page0, sizeof(page0), 0);
	if (!ret)
		ret = send(dsi, idm, sizeof(idm), 10);
	if (ret)
		goto out;		/* panel mode unchanged */
	/* the panel is in the new mode from here on, whatever follows fails */
	set_idle_state(on);
	if (on)
		n_on++;
	else
		n_off++;
	if (on && idle_brightness > 0) {
		u8 br[2] = { DCS_SET_BRIGHTNESS, (u8)idle_brightness };

		ret = send(dsi, br, sizeof(br), 0);
	} else if (!on) {
		/* back on: the backlight driver resends its own level */
		ret = bl_resend_locked();
	}
	last_mode = read_mode(dsi);
out:
	last_err = ret;
	bl_unlock();
	panel_put();
	return ret;
}

static ssize_t idle_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", idle_state);
}

static ssize_t idle_store(struct kobject *k, struct kobj_attribute *a,
			  const char *buf, size_t count)
{
	int on, ret;

	if (kstrtoint(buf, 0, &on) || (on != 0 && on != 1))
		return -EINVAL;
	mutex_lock(&aod_lock);
	ret = set_idle(on);
	mutex_unlock(&aod_lock);
	return ret ? ret : count;
}

static ssize_t idle_brightness_show(struct kobject *k,
				    struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", idle_brightness);
}

static ssize_t idle_brightness_store(struct kobject *k,
				     struct kobj_attribute *a,
				     const char *buf, size_t count)
{
	struct sprd_dsi *dsi;
	int v;

	if (kstrtoint(buf, 0, &v) || v < 0 || v > 255)
		return -EINVAL;
	mutex_lock(&aod_lock);
	idle_brightness = v;
	/* live preview: apply now if the panel is already in AOD idle mode */
	if (idle_state && panel_get()) {
		bl_lock();
		dsi = dsi_ctx();
		if (dsi && v > 0) {
			u8 br[2] = { DCS_SET_BRIGHTNESS, (u8)v };

			mipi_dsi_lp_cmd_enable(dsi, 1);
			last_err = send(dsi, br, sizeof(br), 0);
		} else if (dsi) {
			/* 0 = back to Android's doze level */
			last_err = bl_resend_locked();
		}
		bl_unlock();
		panel_put();
	}
	mutex_unlock(&aod_lock);
	return count;
}

/* ---- double tap to wake --------------------------------------------- */

static void dtap_wake(struct work_struct *w)
{
	input_event(wake_dev, EV_KEY, KEY_WAKEUP, 1);
	input_event(wake_dev, EV_SYN, SYN_REPORT, 0);
	input_event(wake_dev, EV_KEY, KEY_WAKEUP, 0);
	input_event(wake_dev, EV_SYN, SYN_REPORT, 0);
}

/*
 * Swallow/pass is decided once per contact (at touch-down) and kept until the
 * frame that carries the release, so toggling idle or dtap mid-contact can't
 * pass a press and swallow its release (or the other way round).
 * Runs under the input device's event_lock: contact needs no other locking.
 */
static int frame = -1;		/* decision for the current report frame */
static int contact = -1;	/* -1 none, 0 passing, 1 swallowing */
static bool contact_ending;
static int seen_gen;

static bool dtap_filter(struct input_handle *handle, unsigned int type,
			unsigned int code, int value)
{
	ktime_t now;
	bool swallow;
	int gen = atomic_read(&policy_gen);

	if (gen != seen_gen) {
		seen_gen = gen;
		dtap_have_last = false;	/* no tap history across switches */
	}
	/* decide at the first event of a frame: the touch driver sends the MT
	 * slot/tracking events before BTN_TOUCH in the same frame */
	if (frame < 0)
		frame = contact >= 0 ? contact :
			(READ_ONCE(dtap) && READ_ONCE(idle_state));
	swallow = frame;
	if (type == EV_KEY && code == BTN_TOUCH) {
		if (value == 1)
			contact = frame;
		else if (contact >= 0)
			contact_ending = true;
	}
	if (type == EV_SYN && code == SYN_REPORT) {
		frame = -1;
		if (contact_ending) {
			contact = -1;
			contact_ending = false;
		}
	}
	if (!swallow)
		return false;		/* awake: pass everything */
	if (type == EV_KEY && code == BTN_TOUCH && value == 1) {
		now = ktime_get();
		n_taps++;
		if (dtap_have_last &&
		    ktime_to_ms(ktime_sub(now, dtap_last)) <=
		    READ_ONCE(dtap_ms)) {
			dtap_have_last = false;
			n_dtaps++;
			/* cover the work item until userspace takes the key */
			__pm_wakeup_event(dtap_ws, 200);
			schedule_work(&dtap_work);
		} else {
			dtap_last = now;
			dtap_have_last = true;
			/* keep the CPU up long enough for a second tap */
			__pm_wakeup_event(dtap_ws, READ_ONCE(dtap_ms) + 200);
		}
	}
	return true;			/* AOD: swallow the whole contact */
}

static bool dtap_match(struct input_handler *handler, struct input_dev *dev)
{
	int i;

	if (!dev->name)
		return false;
	for (i = 0; i < ARRAY_SIZE(touch_names); i++)
		if (!strcmp(dev->name, touch_names[i]))
			return true;
	return false;
}

/* one touchscreen: a static handle, no allocation */
static struct input_handle dtap_handle;
static bool dtap_handle_used;

static int dtap_connect(struct input_handler *handler, struct input_dev *dev,
			const struct input_device_id *id)
{
	struct input_handle *h = &dtap_handle;
	int ret;

	if (dtap_handle_used)
		return -EBUSY;
	memset(h, 0, sizeof(*h));
	/* new device: drop any half-seen contact (runs before events flow) */
	frame = -1;
	contact = -1;
	contact_ending = false;
	h->dev = dev;
	h->handler = handler;
	h->name = "aod_dtap";
	ret = input_register_handle(h);
	if (ret)
		return ret;
	ret = input_open_device(h);
	if (ret) {
		input_unregister_handle(h);
		return ret;
	}
	dtap_handle_used = true;
	return 0;
}

static void dtap_disconnect(struct input_handle *h)
{
	input_close_device(h);
	input_unregister_handle(h);
	dtap_handle_used = false;
}

static const struct input_device_id dtap_ids[] = {
	{ .driver_info = 1 },	/* match all; dtap_match() picks the touch */
	{ },
};

static struct input_handler dtap_handler = {
	.filter		= dtap_filter,
	.match		= dtap_match,
	.connect	= dtap_connect,
	.disconnect	= dtap_disconnect,
	.name		= "aod_dtap",
	.id_table	= dtap_ids,
};

static int dtap_init(void)
{
	int ret;

	dtap_ws = wakeup_source_register("aod_dtap");
	if (!dtap_ws)
		return -ENOMEM;
	INIT_WORK(&dtap_work, dtap_wake);
	wake_dev = input_allocate_device();
	if (!wake_dev) {
		wakeup_source_unregister(dtap_ws);
		return -ENOMEM;
	}
	wake_dev->name = "aod_dtap";
	wake_dev->id.bustype = BUS_VIRTUAL;
	input_set_capability(wake_dev, EV_KEY, KEY_WAKEUP);
	ret = input_register_device(wake_dev);
	if (ret) {
		input_free_device(wake_dev);
		wakeup_source_unregister(dtap_ws);
		return ret;
	}
	ret = input_register_handler(&dtap_handler);
	if (ret) {
		input_unregister_device(wake_dev);
		wakeup_source_unregister(dtap_ws);
		return ret;
	}
	dtap_registered = true;
	return 0;
}

static void dtap_exit(void)
{
	if (!dtap_registered)
		return;
	input_unregister_handler(&dtap_handler);
	cancel_work_sync(&dtap_work);
	input_unregister_device(wake_dev);
	wakeup_source_unregister(dtap_ws);
}

static ssize_t dtap_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "%d\n", dtap);
}

static ssize_t dtap_store(struct kobject *k, struct kobj_attribute *a,
			  const char *buf, size_t count)
{
	int v;

	if (kstrtoint(buf, 0, &v) || (v != 0 && v != 1))
		return -EINVAL;
	WRITE_ONCE(dtap, v);
	atomic_inc(&policy_gen);
	return count;
}

static ssize_t dtap_ms_show(struct kobject *k, struct kobj_attribute *a,
			    char *buf)
{
	return sprintf(buf, "%d\n", dtap_ms);
}

static ssize_t dtap_ms_store(struct kobject *k, struct kobj_attribute *a,
			     const char *buf, size_t count)
{
	int v;

	if (kstrtoint(buf, 0, &v) || v < 100 || v > 2000)
		return -EINVAL;
	WRITE_ONCE(dtap_ms, v);
	return count;
}

static ssize_t status_show(struct kobject *k, struct kobj_attribute *a,
			   char *buf)
{
	return sprintf(buf,
		       "idle=%d panel_powered=%d last_err=%d dcs_0x0a=0x%02x%s on=%u off=%u refused=%u idle_brightness=%d backlight=%d dtap=%d taps=%u dtaps=%u\n",
		       idle_state, panel_powered(), last_err, last_mode,
		       last_mode >= 0 ?
		       ((last_mode & POWER_MODE_IDLE) ? "(idle)" : "(normal)") : "",
		       n_on, n_off, n_refused, idle_brightness,
		       bd ? bd->props.brightness : -1,
		       dtap && dtap_registered, n_taps, n_dtaps);
}

/* debug: echo 0x0a > peek; cat peek -> DCS read of one register */
static int peek_reg = DCS_GET_POWER_MODE, peek_val = -1;

static ssize_t peek_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sprintf(buf, "reg=0x%02x val=%d (0x%02x)\n", peek_reg, peek_val,
		       peek_val & 0xff);
}

static ssize_t peek_store(struct kobject *k, struct kobj_attribute *a,
			  const char *buf, size_t count)
{
	struct sprd_dsi *dsi;
	u8 v = 0;
	int reg, ret;

	if (kstrtoint(buf, 0, &reg) || reg < 0 || reg > 0xff)
		return -EINVAL;
	mutex_lock(&aod_lock);
	ret = -EIO;
	if (panel_get()) {
		bl_lock();
		dsi = dsi_ctx();
		if (dsi) {
			mipi_dsi_lp_cmd_enable(dsi, 1);
			ret = mipi_dsi_set_max_return_size(dsi, 1);
			if (ret >= 0) {
				ret = mipi_dsi_dcs_read(dsi, reg, &v, 1);
				ret = ret == 1 ? v : (ret < 0 ? ret : -EIO);
			}
			peek_reg = reg;
			peek_val = ret;
		}
		bl_unlock();
		panel_put();
	}
	mutex_unlock(&aod_lock);
	return ret < 0 ? ret : count;
}

static struct kobj_attribute idle_attr = __ATTR(idle, 0664, idle_show, idle_store);
static struct kobj_attribute status_attr = __ATTR_RO(status);
static struct kobj_attribute dtap_attr = __ATTR(dtap, 0664, dtap_show, dtap_store);
static struct kobj_attribute dtap_ms_attr =
	__ATTR(dtap_ms, 0664, dtap_ms_show, dtap_ms_store);
static struct kobj_attribute idle_brightness_attr =
	__ATTR(idle_brightness, 0664, idle_brightness_show, idle_brightness_store);
static struct kobj_attribute peek_attr = __ATTR(peek, 0600, peek_show, peek_store);
static struct attribute *aod_attrs[] = {
	&idle_attr.attr, &status_attr.attr, &peek_attr.attr,
	&idle_brightness_attr.attr, &dtap_attr.attr, &dtap_ms_attr.attr, NULL
};
static struct attribute_group aod_group = { .attrs = aod_attrs };
static struct kobject *aod_kobj;

static int __init sprd_aod_idle_init(void)
{
	int ret;

	pd = sprd_panel_get(SPRD_MAINLCD_ID);
	if (!pd || !pd->panel || !pd->intf || !pd->intf->driver ||
	    !dev_get_drvdata(pd->intf) || !pd->dev.parent) {
		pr_err("no MIPI panel\n");
		return -ENODEV;
	}
	if (pd->panel->type != SPRD_PANEL_TYPE_MIPI) {
		pr_err("panel is not MIPI DSI\n");
		return -ENODEV;
	}
	/*
	 * The ESD check reads DCS 0x0a and expects the normal-mode value;
	 * in idle mode it reads 0xdc and would trigger ESD recovery.
	 */
	if (pd->panel->esd_check_en && pd->panel->esd_reg == DCS_GET_POWER_MODE) {
		pr_err("panel ESD check reads 0x0a, incompatible with idle mode\n");
		return -EINVAL;
	}
	bd = sprd_panel_oled_backlight();
	if (!bd)
		pr_warn("no OLED backlight, brightness is not restored\n");

	aod_kobj = kobject_create_and_add("aod_idle", kernel_kobj);
	if (!aod_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(aod_kobj, &aod_group);
	if (ret) {
		kobject_put(aod_kobj);
		return ret;
	}
	disp_notifier_register(&aod_nb);
	if (dtap_init())
		pr_warn("double tap to wake unavailable\n");
	pr_info("panel %s, powered=%d\n", pd->panel->lcd_name ?
		pd->panel->lcd_name : "?", panel_powered());
	return 0;
}

static void __exit sprd_aod_idle_exit(void)
{
	disp_notifier_unregister(&aod_nb);
	dtap_exit();
	sysfs_remove_group(aod_kobj, &aod_group);
	kobject_put(aod_kobj);
	mutex_lock(&aod_lock);
	if (idle_state)
		set_idle(0);	/* back to normal if still powered */
	mutex_unlock(&aod_lock);
}

/* the panel platform driver probes at device_initcall time */
late_initcall(sprd_aod_idle_init);
module_exit(sprd_aod_idle_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Panel idle mode (AOD) switch and double-tap wake for the SC9832E CO5300 watch panel");
