/*
 * hrs3xxx: shim around the stock TYHX measurement code (external object, not
 * in this tree): the HX3918 hardware layer and HR/SpO2 algorithm (chip ID
 * 0x27), or the HRS3300 layer and algorithm (chip ID 0x21).
 *
 * The stock code expects a vendor "hrs3xxx_t" with the i2c client at offset 0
 * and its results at fixed offsets; it also calls back into register
 * accessors (HX3918: and two timer switches) that the stock glue provided.
 * This file provides exactly those, with the stock semantics, on top of our
 * own hrtimers and work items. The vendor code never sees a kernel hrtimer,
 * work_struct or mutex layout.
 *
 * One kernel links one blob, chosen at build time (Kconfig choice
 * HRS3XXX_TYHX_BLOB_HX3918 / _HRS3300). A per-chip ops table, looked up by the
 * chip ID, holds the five or four vendor calls that differ; the lock, the
 * live/suspended handling and the register accessors are common. A chip whose
 * ID the linked blob does not serve gets no ops: the core driver then keeps
 * working ID-only.
 *
 * Evidence for every offset and semantic below: devices/HRS3XXX_INTERFACE.md
 * (HX3918) and devices/HRS3300_BLOB.md (HRS3300).
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
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "hrs3xxx_blob.h"

#define HRSB_PERIOD_MS		40	/* stock: every timer, 0x2625a00 ns */

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

struct hrsb_ops;

struct hrsb {
	struct hrsb_vendor v;
	struct input_dev *input;
	struct mutex lock;		/* serialises all vendor code */
	const struct hrsb_ops *ops;
	struct hrtimer ppg_timer;	/* -> ppg_work: measurement */
	struct hrtimer agc_timer;	/* -> agc_work; only if ops->agc_tick */
	struct work_struct ppg_work;
	struct work_struct agc_work;
	atomic_t ppg_run;		/* timer re-arms itself while set */
	atomic_t agc_run;
	bool live;			/* probe finished, not removed */
	bool suspended;			/* between hrsb_suspend and resume */
	bool ppg_was_on;		/* timers that were on at suspend */
	bool agc_was_on;
};

/*
 * Per-chip back end. Every method runs with b->lock held and, for start, with
 * the chip powered and the previous run (if any) stopped.
 */
struct hrsb_ops {
	const char *name;
	u8 chip_id;
	/* Initialises the chip and algorithm and arms the timers it needs. */
	void (*start)(struct hrsb *b, unsigned int mode);
	/* Stock exit: puts the chip in its disabled state. */
	void (*stop)(struct hrsb *b);
	/* One 40 ms measurement tick (ppg_work), including the reporting. */
	void (*ppg_tick)(struct hrsb *b);
	/* Second timer's work; NULL when the chip has no separate AGC tick. */
	void (*agc_tick)(struct hrsb *b);
};

/* Vendor code (carved object); both blobs export the global pointer. */
extern struct hrsb_vendor *hrsblob_hrs3xxx_t;	/* its global vendor pointer */

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
 * Restart (on) or cancel (!on) one of the 40 ms timers: the stock timer switch.
 * The timer callback re-arms every 40 ms while the run flag is set. Callers
 * hold b->lock; hrtimer_cancel() only waits for the callback, which never
 * takes the lock.
 */
static void hrsb_timer_cfg(struct hrsb *b, struct hrtimer *t, atomic_t *run,
			   u8 on)
{
	lockdep_assert_held(&b->lock);

	hrtimer_cancel(t);
	atomic_set(run, !!on);
	if (on)
		hrtimer_start(t, ms_to_ktime(HRSB_PERIOD_MS), HRTIMER_MODE_REL);
}

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

static void hrsb_ppg_work(struct work_struct *work)
{
	struct hrsb *b = container_of(work, struct hrsb, ppg_work);

	mutex_lock(&b->lock);
	if (b->live && b->v.enabled && !b->suspended) {
		hrsblob_hrs3xxx_t = &b->v;
		b->ops->ppg_tick(b);
	}
	mutex_unlock(&b->lock);
}

static void hrsb_agc_work(struct work_struct *work)
{
	struct hrsb *b = container_of(work, struct hrsb, agc_work);

	mutex_lock(&b->lock);
	if (b->live && b->v.enabled && !b->suspended) {
		hrsblob_hrs3xxx_t = &b->v;
		b->ops->agc_tick(b);
	}
	mutex_unlock(&b->lock);
}

/*
 * Timers first, then works: a timer cannot queue a work that was cancelled.
 * The AGC timer and work exist (are initialised) only for chips with an
 * agc_tick.
 */
static void hrsb_quiesce(struct hrsb *b)
{
	atomic_set(&b->ppg_run, 0);
	hrtimer_cancel(&b->ppg_timer);
	if (b->ops->agc_tick) {
		atomic_set(&b->agc_run, 0);
		hrtimer_cancel(&b->agc_timer);
	}
	cancel_work_sync(&b->ppg_work);
	if (b->ops->agc_tick)
		cancel_work_sync(&b->agc_work);
}

/* ---- HX3918 back end (chip ID 0x27) ------------------------------------ */

#ifdef CONFIG_HRS3XXX_TYHX_BLOB_HX3918

extern int hx3918_init(struct hrsb_vendor *hrs, int mode);
extern void hx3918_exit(struct hrsb_vendor *hrs);
extern void heart_rate_meas_timeout_handler(struct hrsb_vendor *hrs);
extern void agc_timeout_handler(struct hrsb_vendor *hrs);

/* Provided here for the vendor code (declared for -Wmissing-prototypes). */
void hx3918_ppg_timer_cfg(u8 on);
void hx3918_agc_timer_cfg(u8 on);

/* Stock: on != 0 restarts the timer with a 40 ms relative expiry, 0 cancels. */
void hx3918_ppg_timer_cfg(u8 on)
{
	struct hrsb *b = hrsb_inst;

	if (b)
		hrsb_timer_cfg(b, &b->ppg_timer, &b->ppg_run, on);
}

void hx3918_agc_timer_cfg(u8 on)
{
	struct hrsb *b = hrsb_inst;

	if (b)
		hrsb_timer_cfg(b, &b->agc_timer, &b->agc_run, on);
}

#ifndef CONFIG_CC_STACKPROTECTOR
/*
 * The vendor object is built with -fstack-protector and reads the global
 * guard by value (adrp + ldr lo12), so it needs a plain global long and
 * __stack_chk_fail(). On 4.14 arm64 both exist only with
 * CONFIG_CC_STACKPROTECTOR (unlike 4.4, which always defines the guard), so
 * without it they are provided here. One global guard, as on arm64 4.14. It
 * is set in hrsb_create(), before the first call into the vendor code.
 */
unsigned long __stack_chk_guard;
void __stack_chk_fail(void);
void __stack_chk_fail(void)
{
	panic("stack-protector: corrupted stack in hrs3xxx vendor code: %p\n",
	      __builtin_return_address(0));
}

static void hrsb_init_guard(void)
{
	unsigned long canary;

	get_random_bytes(&canary, sizeof(canary));
	canary &= ~0xffUL;	/* NUL byte, as CANARY_MASK on little endian */
	__stack_chk_guard = canary;
}
#else
static inline void hrsb_init_guard(void) { }
#endif

/* Same events and order as the stock work function (all eight REL codes). */
static void hrsb_report_hx3918(struct hrsb *b)
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

#define HRSB_INIT_MODE_HRS	0	/* stock: enable 1 and unknown values */
#define HRSB_INIT_MODE_SPO2	1	/* stock: enable 2 */
#define HRSB_INIT_MODE_FACTORY	6	/* stock: enable 3 */

static void hrsb_hx3918_start(struct hrsb *b, unsigned int mode)
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

	/*
	 * The stock return value is not uniform (0 = success for the
	 * measurement modes, non-zero for the factory mode) and stock ignores
	 * it; so do we. hx3918_init() arms the timers through the *_timer_cfg
	 * switches above.
	 */
	hx3918_init(&b->v, bmode);
}

static void hrsb_hx3918_stop(struct hrsb *b)
{
	/* Also puts the chip in standby and switches both timers off. */
	hx3918_exit(&b->v);
}

static void hrsb_hx3918_ppg_tick(struct hrsb *b)
{
	heart_rate_meas_timeout_handler(&b->v);
	hrsb_report_hx3918(b);	/* stock reports after every tick */
}

static void hrsb_hx3918_agc_tick(struct hrsb *b)
{
	agc_timeout_handler(&b->v);
}

static const struct hrsb_ops hrsb_ops_hx3918 = {
	.name = "hx3918",
	.chip_id = HRS3XXX_ID_HRS3918,
	.start = hrsb_hx3918_start,
	.stop = hrsb_hx3918_stop,
	.ppg_tick = hrsb_hx3918_ppg_tick,
	.agc_tick = hrsb_hx3918_agc_tick,
};

#else /* !CONFIG_HRS3XXX_TYHX_BLOB_HX3918 */

static inline void hrsb_init_guard(void) { }

#endif /* CONFIG_HRS3XXX_TYHX_BLOB_HX3918 */

/* ---- HRS3300 back end (chip ID 0x21) ----------------------------------- */

#ifdef CONFIG_HRS3XXX_TYHX_BLOB_HRS3300

extern void Hrs3300_chip_init(struct hrsb_vendor *hrs);	/* sets the global */
extern void Hrs3300_chip_enable(void);
extern void Hrs3300_alg_open(void);
extern void Hrs3300_chip_disable(void);
extern u8 hrs3300_Int_handle(struct hrsb_vendor *hrs);

/*
 * Stock init_heartrate() for enable > 0 (the mode is ignored: every positive
 * value behaves alike): chip_init, chip_enable, alg_open, start the single
 * 40 ms timer. The caller sets "enabled" afterwards, still under the lock, as
 * stock sets it after the timer start; the first tick is 40 ms away and takes
 * the lock anyway. No AGC timer: the AGC runs inside Hrs3300_alg_send_data.
 */
static void hrsb_hrs3300_start(struct hrsb *b, unsigned int mode)
{
	Hrs3300_chip_init(&b->v);
	Hrs3300_chip_enable();
	Hrs3300_alg_open();
	hrsb_timer_cfg(b, &b->ppg_timer, &b->ppg_run, 1);
}

/*
 * Stock disable: Hrs3300_chip_disable() (writes the chip to its disabled
 * state). The stock 320 ms sleep before it is not reproduced: see
 * devices/HRS3300_BLOB.md.
 */
static void hrsb_hrs3300_stop(struct hrsb *b)
{
	Hrs3300_chip_disable();
}

/*
 * Stock work function: report REL_X/Y/Z and SYN only when Int_handle wrote new
 * results (non-zero return). The other five REL codes are never sent.
 */
static void hrsb_hrs3300_ppg_tick(struct hrsb *b)
{
	struct input_dev *input = b->input;

	if (!hrs3300_Int_handle(&b->v))
		return;

	input_report_rel(input, REL_X, b->v.rel_x);		/* heart rate */
	input_report_rel(input, REL_Y, b->v.rel_y);		/* systolic est. */
	input_report_rel(input, REL_Z, b->v.rel_z);		/* diastolic est. */
	input_sync(input);
}

static const struct hrsb_ops hrsb_ops_hrs3300 = {
	.name = "hrs3300",
	.chip_id = HRS3XXX_ID_HRS3300,
	.start = hrsb_hrs3300_start,
	.stop = hrsb_hrs3300_stop,
	.ppg_tick = hrsb_hrs3300_ppg_tick,
	.agc_tick = NULL,
};

#endif /* CONFIG_HRS3XXX_TYHX_BLOB_HRS3300 */

/* ---- ops selection by chip ID ------------------------------------------ */

#if !defined(CONFIG_HRS3XXX_TYHX_BLOB_HX3918) && \
	!defined(CONFIG_HRS3XXX_TYHX_BLOB_HRS3300)
#error "HRS3XXX_TYHX_BLOB needs a variant (HRS3XXX_TYHX_BLOB_HX3918 or _HRS3300)"
#endif

/* Only the linked variant: the other one's vendor symbols do not exist. */
static const struct hrsb_ops *const hrsb_ops_tbl[] = {
#ifdef CONFIG_HRS3XXX_TYHX_BLOB_HX3918
	&hrsb_ops_hx3918,
#endif
#ifdef CONFIG_HRS3XXX_TYHX_BLOB_HRS3300
	&hrsb_ops_hrs3300,
#endif
};

static const struct hrsb_ops *hrsb_find_ops(u8 chip_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hrsb_ops_tbl); i++)
		if (hrsb_ops_tbl[i]->chip_id == chip_id)
			return hrsb_ops_tbl[i];
	return NULL;
}

bool hrsb_chip_matches(u8 chip_id)
{
	return hrsb_find_ops(chip_id) != NULL;
}

const char *hrsb_variant_name(void)
{
	return hrsb_ops_tbl[0]->name;
}

/* ---- API used by the core driver -------------------------------------- */

struct hrsb *hrsb_create(struct i2c_client *client, struct input_dev *input,
			 u8 chip_id)
{
	const struct hrsb_ops *ops = hrsb_find_ops(chip_id);
	struct hrsb *b;

	hrsb_check_layout();

	if (!ops)
		return ERR_PTR(-ENODEV);	/* blob is for another chip */
	if (hrsb_inst)
		return ERR_PTR(-EBUSY);	/* the vendor code is global-state */

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return ERR_PTR(-ENOMEM);

	hrsb_init_guard();

	b->ops = ops;
	b->v.client = client;
	b->input = input;
	mutex_init(&b->lock);
	hrtimer_init(&b->ppg_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	b->ppg_timer.function = hrsb_ppg_timer_fn;
	INIT_WORK(&b->ppg_work, hrsb_ppg_work);
	if (ops->agc_tick) {
		hrtimer_init(&b->agc_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		b->agc_timer.function = hrsb_agc_timer_fn;
		INIT_WORK(&b->agc_work, hrsb_agc_work);
	}

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
	/* Restart cleanly if already running. */
	hrsb_quiesce(b);

	mutex_lock(&b->lock);
	if (!b->live) {
		mutex_unlock(&b->lock);
		return -ENODEV;
	}
	b->suspended = false;	/* a fresh start supersedes a suspend */

	/*
	 * Needed by the HX3918 code before every call; the HRS3300 code sets
	 * the same value itself in Hrs3300_chip_init().
	 */
	hrsblob_hrs3xxx_t = &b->v;
	if (b->v.enabled) {
		b->v.enabled = 0;
		b->ops->stop(b);
	}
	b->v.rel_x = 0;
	b->v.rel_rx = 0;
	b->v.rel_ry = 0;
	b->v.rel_rz = 0;
	b->v.rel_y = 0;
	b->v.rel_z = 0;
	b->v.rel_hwheel = 0;
	b->v.rel_misc = 0;

	b->ops->start(b, mode);
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
			if (b->ppg_was_on)
				hrsb_timer_cfg(b, &b->ppg_timer, &b->ppg_run, 1);
			if (b->agc_was_on)
				hrsb_timer_cfg(b, &b->agc_timer, &b->agc_run, 1);
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
		b->ops->stop(b);
	}
	mutex_unlock(&b->lock);

	/* The stock exit may switch timers off itself; make sure nothing is left. */
	hrsb_quiesce(b);
}
