/*
 * hrs3xxx: shim around the stock TYHX HX3918 hardware layer and HR/SpO2
 * algorithm (external object, not in this tree).
 *
 * The stock code expects a vendor "hrs3xxx_t" with the i2c client at offset 0
 * and its results at fixed offsets; it also calls back into three register
 * accessors and two timer switches that the stock glue provided. This file
 * provides exactly those, with the stock semantics, on top of our own
 * hrtimers and work items. The vendor code never sees a kernel hrtimer,
 * work_struct or mutex layout.
 *
 * Evidence for every offset and semantic below: devices/HRS3XXX_INTERFACE.md.
 *
 * All calls into the vendor code are serialised by hrsb->lock: it has global
 * state and a global pointer to the vendor struct.
 */

#include <linux/bug.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "hrs3xxx_blob.h"

#define HRSB_PERIOD_MS		40	/* stock: both timers, 0x2625a00 ns */

#define HRSB_INIT_MODE_HRS	0	/* stock: enable 1 and unknown values */
#define HRSB_INIT_MODE_SPO2	1	/* stock: enable 2 */
#define HRSB_INIT_MODE_FACTORY	6	/* stock: enable 3 */

/*
 * The only fields of the stock struct the vendor code touches. Results are
 * written by the vendor code and read by the report function (stock offsets
 * 304..332).
 */
struct hrsb_vendor {
	struct i2c_client *client;		/* +0x000 */
	u8 pad0[0x100 - sizeof(void *)];
	u8 enabled;				/* +0x100 (glue only) */
	u8 pad1[0x130 - 0x101];
	s32 rel_x;				/* +0x130 bpm */
	s32 rel_rx;				/* +0x134 SpO2 */
	s32 rel_ry;				/* +0x138 */
	s32 rel_rz;				/* +0x13c wear state */
	s32 rel_hwheel;				/* +0x140 */
	s32 rel_misc;				/* +0x144 */
	s32 rel_y;				/* +0x148 */
	s32 rel_z;				/* +0x14c */
};

struct hrsb {
	struct hrsb_vendor v;
	struct input_dev *input;
	struct mutex lock;		/* serialises all vendor code */
	struct hrtimer ppg_timer;	/* -> ppg_work: measurement */
	struct hrtimer agc_timer;	/* -> agc_work */
	struct work_struct ppg_work;
	struct work_struct agc_work;
	atomic_t ppg_run;		/* timer re-arms itself while set */
	atomic_t agc_run;
	bool live;			/* probe finished, not removed */
	bool suspended;			/* between hrsb_suspend and resume */
	bool ppg_was_on;		/* timers that were on at suspend */
	bool agc_was_on;
};

/* Vendor code (carved object). */
extern struct hrsb_vendor *hrsblob_hrs3xxx_t;	/* its global vendor pointer */
extern int hx3918_init(struct hrsb_vendor *hrs, int mode);
extern void hx3918_exit(struct hrsb_vendor *hrs);
extern void heart_rate_meas_timeout_handler(struct hrsb_vendor *hrs);
extern void agc_timeout_handler(struct hrsb_vendor *hrs);

/* Offsets must match the stock layout the vendor code was built against. */
static inline void hrsb_check_layout(void)
{
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, client) != 0x000);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, enabled) != 0x100);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_x) != 304);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_rx) != 308);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_ry) != 312);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_rz) != 316);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_hwheel) != 320);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_misc) != 324);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_y) != 328);
	BUILD_BUG_ON(offsetof(struct hrsb_vendor, rel_z) != 332);
	BUILD_BUG_ON(sizeof(struct hrsb_vendor) != 336);
}

/* Provided here for the vendor code (declared for -Wmissing-prototypes). */
u8 hrs3xxx_i2c_read(struct i2c_client *client, u8 reg, u8 *buf, u8 len);
u8 hrs3xxx_read_reg(struct i2c_client *client, u8 reg);
u8 hrs3xxx_write_reg(struct i2c_client *client, u8 reg, u8 val);
void hx3918_ppg_timer_cfg(u8 on);
void hx3918_agc_timer_cfg(u8 on);

/* The vendor code is global-state: one instance only. */
static struct hrsb *hrsb_inst;

/* ---- glue the vendor code calls (stock semantics) ------------------- */

/*
 * Stock: send reg (1 byte), then read len bytes. Returns len on success and
 * 0xff on any transfer error (a transfer count that differs from the request
 * counts as an error).
 */
u8 hrs3xxx_i2c_read(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{
	int ret;

	if (!len)
		return 0xff;

	ret = i2c_master_send(client, &reg, 1);
	if ((u8)ret != 1) {
		dev_dbg(&client->dev, "i2c read reg 0x%02x: send failed\n", reg);
		return 0xff;
	}
	ret = i2c_master_recv(client, buf, len);
	if ((u8)ret != len) {
		dev_dbg(&client->dev, "i2c read reg 0x%02x: recv failed\n", reg);
		return 0xff;
	}

	return len;
}

/*
 * Stock: returns the byte read. The stock buffer is primed with the register
 * address, so a failed transfer returns the register number; kept as is.
 */
u8 hrs3xxx_read_reg(struct i2c_client *client, u8 reg)
{
	u8 val = reg;

	hrs3xxx_i2c_read(client, reg, &val, 1);
	return val;
}

/* Stock: one transfer {reg, val}; returns 0 on success, 0xff on error. */
u8 hrs3xxx_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if ((u8)ret != sizeof(buf)) {
		dev_dbg(&client->dev, "i2c write reg 0x%02x failed\n", reg);
		return 0xff;
	}

	return 0;
}

/*
 * Stock: on != 0 restarts the hrtimer with a 40 ms relative expiry, 0
 * cancels it. The timer callback re-arms every 40 ms while the switch is on.
 * Called by the vendor code with hrsb->lock held; hrtimer_cancel() only waits
 * for the callback, which never takes the lock.
 */
void hx3918_ppg_timer_cfg(u8 on)
{
	struct hrsb *b = hrsb_inst;

	if (!b)
		return;
	lockdep_assert_held(&b->lock);

	hrtimer_cancel(&b->ppg_timer);
	atomic_set(&b->ppg_run, !!on);
	if (on)
		hrtimer_start(&b->ppg_timer, ms_to_ktime(HRSB_PERIOD_MS),
			      HRTIMER_MODE_REL);
}

void hx3918_agc_timer_cfg(u8 on)
{
	struct hrsb *b = hrsb_inst;

	if (!b)
		return;
	lockdep_assert_held(&b->lock);

	hrtimer_cancel(&b->agc_timer);
	atomic_set(&b->agc_run, !!on);
	if (on)
		hrtimer_start(&b->agc_timer, ms_to_ktime(HRSB_PERIOD_MS),
			      HRTIMER_MODE_REL);
}

#ifndef CONFIG_CC_STACKPROTECTOR
/*
 * The vendor object is built with -fstack-protector; the kernel only
 * provides __stack_chk_fail() when it is built with it too
 * (__stack_chk_guard is always defined on arm64).
 */
void __stack_chk_fail(void)
{
	panic("stack-protector: corrupted stack in hrs3xxx vendor code: %p\n",
	      __builtin_return_address(0));
}
#endif

/* ---- timers and works -------------------------------------------------- */

static enum hrtimer_restart hrsb_ppg_timer_fn(struct hrtimer *t)
{
	struct hrsb *b = container_of(t, struct hrsb, ppg_timer);

	schedule_work(&b->ppg_work);
	if (!atomic_read(&b->ppg_run))
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ms_to_ktime(HRSB_PERIOD_MS));
	return HRTIMER_RESTART;
}

static enum hrtimer_restart hrsb_agc_timer_fn(struct hrtimer *t)
{
	struct hrsb *b = container_of(t, struct hrsb, agc_timer);

	schedule_work(&b->agc_work);
	if (!atomic_read(&b->agc_run))
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ms_to_ktime(HRSB_PERIOD_MS));
	return HRTIMER_RESTART;
}

/* Same events and order as the stock work function (all eight REL codes). */
static void hrsb_report(struct hrsb *b)
{
	struct input_dev *input = b->input;

	input_report_rel(input, REL_X, b->v.rel_x);		/* bpm */
	input_report_rel(input, REL_Y, b->v.rel_y);
	input_report_rel(input, REL_Z, b->v.rel_z);
	input_report_rel(input, REL_RX, b->v.rel_rx);		/* SpO2 */
	input_report_rel(input, REL_RY, b->v.rel_ry);
	input_report_rel(input, REL_RZ, b->v.rel_rz);		/* wear state */
	input_report_rel(input, REL_MISC, b->v.rel_misc);
	input_report_rel(input, REL_HWHEEL, b->v.rel_hwheel);
	input_sync(input);
}

static void hrsb_ppg_work(struct work_struct *work)
{
	struct hrsb *b = container_of(work, struct hrsb, ppg_work);

	mutex_lock(&b->lock);
	if (b->live && b->v.enabled && !b->suspended) {
		hrsblob_hrs3xxx_t = &b->v;
		heart_rate_meas_timeout_handler(&b->v);
		hrsb_report(b);
	}
	mutex_unlock(&b->lock);
}

static void hrsb_agc_work(struct work_struct *work)
{
	struct hrsb *b = container_of(work, struct hrsb, agc_work);

	mutex_lock(&b->lock);
	if (b->live && b->v.enabled && !b->suspended) {
		hrsblob_hrs3xxx_t = &b->v;
		agc_timeout_handler(&b->v);
	}
	mutex_unlock(&b->lock);
}

/* Timers first, then works: a timer cannot queue a work that was cancelled. */
static void hrsb_quiesce(struct hrsb *b)
{
	atomic_set(&b->ppg_run, 0);
	atomic_set(&b->agc_run, 0);
	hrtimer_cancel(&b->ppg_timer);
	hrtimer_cancel(&b->agc_timer);
	cancel_work_sync(&b->ppg_work);
	cancel_work_sync(&b->agc_work);
}

/* ---- API used by the core driver -------------------------------------- */

struct hrsb *hrsb_create(struct i2c_client *client, struct input_dev *input)
{
	struct hrsb *b;

	hrsb_check_layout();

	if (hrsb_inst)
		return ERR_PTR(-EBUSY);	/* the vendor code is global-state */

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return ERR_PTR(-ENOMEM);

	b->v.client = client;
	b->input = input;
	mutex_init(&b->lock);
	hrtimer_init(&b->ppg_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	b->ppg_timer.function = hrsb_ppg_timer_fn;
	hrtimer_init(&b->agc_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	b->agc_timer.function = hrsb_agc_timer_fn;
	INIT_WORK(&b->ppg_work, hrsb_ppg_work);
	INIT_WORK(&b->agc_work, hrsb_agc_work);

	hrsb_inst = b;
	return b;
}

void hrsb_destroy(struct hrsb *b)
{
	if (!b)
		return;

	hrsb_set_live(b, false);
	hrsb_stop(b);
	hrsb_inst = NULL;
	hrsblob_hrs3xxx_t = NULL;
	kfree(b);
}

void hrsb_set_live(struct hrsb *b, bool live)
{
	mutex_lock(&b->lock);
	b->live = live;
	mutex_unlock(&b->lock);
}

bool hrsb_running(struct hrsb *b)
{
	bool r;

	mutex_lock(&b->lock);
	r = b->v.enabled;
	mutex_unlock(&b->lock);
	return r;
}

/* The caller has powered the chip (stock: GPIO high, 100 ms) beforehand. */
int hrsb_start(struct hrsb *b, unsigned int mode)
{
	int bmode;

	switch (mode) {
	case 2:
		bmode = HRSB_INIT_MODE_SPO2;
		break;
	case 3:
		bmode = HRSB_INIT_MODE_FACTORY;
		break;
	default:
		bmode = HRSB_INIT_MODE_HRS;
		break;
	}

	/* Restart cleanly if already running. */
	hrsb_quiesce(b);

	mutex_lock(&b->lock);
	if (!b->live) {
		mutex_unlock(&b->lock);
		return -ENODEV;
	}
	b->suspended = false;	/* a fresh start supersedes a suspend */

	hrsblob_hrs3xxx_t = &b->v;
	if (b->v.enabled) {
		b->v.enabled = 0;
		hx3918_exit(&b->v);
	}
	b->v.rel_x = 0;
	b->v.rel_rx = 0;
	b->v.rel_ry = 0;
	b->v.rel_rz = 0;
	b->v.rel_y = 0;
	b->v.rel_z = 0;
	b->v.rel_hwheel = 0;
	b->v.rel_misc = 0;

	/*
	 * The stock return value is not uniform (0 = success for the
	 * measurement modes, non-zero for the factory mode) and stock ignores
	 * it; so do we.
	 */
	hx3918_init(&b->v, bmode);
	b->v.enabled = 1;
	mutex_unlock(&b->lock);

	return 0;
}

void hrsb_suspend(struct hrsb *b)
{
	mutex_lock(&b->lock);
	if (b->suspended) {
		mutex_unlock(&b->lock);
		return;
	}
	b->suspended = true;
	b->ppg_was_on = atomic_read(&b->ppg_run);
	b->agc_was_on = atomic_read(&b->agc_run);
	mutex_unlock(&b->lock);

	/* Works that already queued see "suspended" and do nothing. */
	hrsb_quiesce(b);
}

void hrsb_resume(struct hrsb *b)
{
	mutex_lock(&b->lock);
	if (b->suspended) {
		b->suspended = false;
		if (b->live && b->v.enabled) {
			if (b->ppg_was_on) {
				atomic_set(&b->ppg_run, 1);
				hrtimer_start(&b->ppg_timer,
					      ms_to_ktime(HRSB_PERIOD_MS),
					      HRTIMER_MODE_REL);
			}
			if (b->agc_was_on) {
				atomic_set(&b->agc_run, 1);
				hrtimer_start(&b->agc_timer,
					      ms_to_ktime(HRSB_PERIOD_MS),
					      HRTIMER_MODE_REL);
			}
		}
		b->ppg_was_on = false;
		b->agc_was_on = false;
	}
	mutex_unlock(&b->lock);
}

void hrsb_stop(struct hrsb *b)
{
	hrsb_quiesce(b);

	mutex_lock(&b->lock);
	b->suspended = false;
	b->ppg_was_on = false;
	b->agc_was_on = false;
	if (b->v.enabled) {
		b->v.enabled = 0;
		hrsblob_hrs3xxx_t = &b->v;
		hx3918_exit(&b->v);
	}
	mutex_unlock(&b->lock);

	/* hx3918_exit() switches both timers off; make sure nothing is left. */
	hrsb_quiesce(b);
}
