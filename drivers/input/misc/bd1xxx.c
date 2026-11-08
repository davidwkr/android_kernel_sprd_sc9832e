/*
 * Rohm BD1xxx (BD1820) PPG heart-rate sensor: userspace ABI of the stock
 * driver plus the raw-sample measurement path.
 *
 * The vendor Android sensors HAL expects:
 *
 *  - an input device named "heartrate" (EV_KEY KEY_63, EV_REL X/Y/Z) with the
 *    sysfs attributes "enable" and "chipinfo" (read as "bd1820", no newline),
 *  - a misc device /dev/bd1662rate: write(2) of up to 20 bytes with byte 0 =
 *    test mode and byte 1 = enable, read(2) of the raw 16-bit PPG samples.
 *
 * The HAL computes heart rate and SpO2 from the raw samples.  This driver
 * programs the chip (BD_Sensor_Init/Close of the stock driver), polls it
 * every 50 ms and publishes one frame of 20 samples per channel per second
 * in the stock layout: three arrays of 35 little-endian u16 at byte offsets
 * 0, 70 and 140 of a 400-byte buffer.  Every register access is taken from
 * the disassembly of the stock 4.4.83 kernel; comments name the stock
 * function and the offset inside it ("Init+0x2dc").
 *
 * Not implemented: the wear-detection state machine (Wear_judge*, driven by
 * the SC7A22 accelerometer) and BD_finger_math (test mode 15), which only
 * processes accelerometer data.  Mode 15 still programs the chip and runs the
 * 200 ms timer like stock but never produces events.
 *
 * Interface details and evidence: devices/BD1XXX_INTERFACE.md
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define BD1XXX_DRV_NAME		"bd1xxx"
#define BD1XXX_INPUT_NAME	"heartrate"
#define BD1XXX_MISC_NAME	"bd1662rate"	/* stock name for all chips */

#define BD1XXX_REG_CTRL		0x00	/* 0x4b run; 0x00, 0x80, 0x00 close */
#define BD1XXX_REG_GREEN_I	0x04	/* stock "green_current" */
#define BD1XXX_REG_RED_I	0x0e	/* stock "red_current" */
#define BD1XXX_REG_IR_I		0x0f	/* stock "ir_current" */
#define BD1XXX_REG_DATA_AMB	0x18	/* u16 LE, low byte first */
#define BD1XXX_REG_DATA_GREEN	0x1a
#define BD1XXX_REG_DATA_RED	0x1c
#define BD1XXX_REG_DATA_IR	0x1e
#define BD1XXX_REG_FIFO_GREEN	0x20	/* HRV modes: 0x20/0x22/0x24 */
#define BD1XXX_REG_FIFO_RED	0x22
#define BD1XXX_REG_FIFO_IR	0x24
#define BD1XXX_REG_FIFO_LEVEL	0x30
#define BD1XXX_REG_DC_BASE	0x32	/* 0x32 green, 0x33 red, 0x34 ir */
#define BD1XXX_REG_ID_LO	0xbc
#define BD1XXX_REG_ID_HI	0xbd
#define BD1XXX_REG_REV_LO	0xbe
#define BD1XXX_REG_REV_HI	0xbf
#define BD1XXX_ID_HI_BD1820	0x70
#define BD1XXX_ID_LO_BD1820	0x11

#define BD1XXX_VDD_UV		2800000
#define BD1XXX_POWERUP_DELAY_MS	10
#define BD1XXX_STOP_DELAY_MS	2

#define BD1XXX_KEY_CODE		63
#define BD1XXX_RAW_SIZE		400	/* stock raw buffer */
#define BD1XXX_WRITE_MAX	20	/* HAL write size: mode, enable, ... */

#define BD1XXX_MODE_SPO2	2
#define BD1XXX_MODE_FINGER	15
#define BD1XXX_POLL_MS		50
#define BD1XXX_POLL_FINGER_MS	200

/*
 * Frame layout (stock): three channel arrays of 35 u16 at byte offsets 0, 70
 * and 140 of the 400-byte buffer.  Entries 0..29 are samples, 27..34 are the
 * trailer words the HAL reads (BD_health_pro+0x44).
 */
#define BD1XXX_NCH		3
#define BD1XXX_ARR_LEN		35
#define BD1XXX_NSAMPLES		30	/* entries 0..29 */
#define BD1XXX_FRAME_MIN	20	/* frame complete when count > 19 */
#define BD1XXX_IDX_AMB		27	/* ch0: reg 0x18/0x19 */
#define BD1XXX_IDX_WEAR		28	/* ch0: wear state, constant 2 here */
#define BD1XXX_IDX_T0		29	/* ch0 green dc, ch1 id lo */
#define BD1XXX_IDX_T1		30	/* ch0 last ch2, ch1 id hi */
#define BD1XXX_IDX_T2		31	/* ch0 last ch1, ch1 ref0, ch2 grn_i */
#define BD1XXX_IDX_T3		32	/* ch0 red dc, ch1 ref1, ch2 red_i */
#define BD1XXX_IDX_T4		33	/* ch0 ir dc, ch1 ref2, ch2 ir_i */
#define BD1XXX_IDX_COUNT	34	/* all channels: samples in frame */

/* DC cancel loop thresholds (BD_Sensor_Data_Read, all three paths) */
#define BD1XXX_DC_HIGH		0xb3b0
#define BD1XXX_DC_SAT		0xffdc
#define BD1XXX_DC_LOW		0x2327
#define BD1XXX_DC_ZERO		9
#define BD1XXX_BAND_HI		0xc350	/* re-arm the loop outside the band */
#define BD1XXX_BAND_LO		0x1b57

/* Stock drains the HRV FIFO without a bound; keep one for a stuck chip. */
#define BD1XXX_DRAIN_MAX	32

#define BD1XXX_MAX_SEQ		32

enum bd1xxx_path {
	BD1XXX_PATH_HR,		/* modes 0, 1, 5 and up (and 15) */
	BD1XXX_PATH_SPO2,	/* mode 2 */
	BD1XXX_PATH_HRV,	/* modes 3, 4 */
};

struct bd1xxx_reg {
	u8 reg;
	u8 val;
};

struct bd1xxx_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct miscdevice misc;
	struct regulator *vdd;	/* NULL when the DT has no vdd_name */
	struct kref ref;	/* probe + one per open misc descriptor */
	/*
	 * ctl serialises enable, disable, suspend, resume and remove and is
	 * held across cancel_work_sync().  lock is taken after ctl and by the
	 * sampling work and the misc read, so the work never waits for ctl.
	 */
	struct mutex ctl;
	struct mutex lock;	/* protects everything below except ctl state */
	struct hrtimer timer;
	struct work_struct work;
	ktime_t period;
	int power_gpio;		/* < 0 when the board has no power switch */
	unsigned int mode;	/* test mode written by the HAL */
	unsigned int run_mode;	/* mode the chip was programmed for */
	bool enabled;		/* ctl */
	bool powered;
	bool suspended;		/* ctl */
	bool removed;		/* client gone: vdd and power_gpio released */
	bool programmed;	/* Init wrote registers: Close is due */
	bool running;		/* poller active */

	/* measurement state, reset by bd1xxx_program() */
	__le16 frame[BD1XXX_NCH][BD1XXX_ARR_LEN];
	unsigned int count;	/* stock data_count, saturates at 30 */
	u8 dc[BD1XXX_NCH];	/* regs 0x32..0x34 */
	bool dc_pending[2];	/* stock red_setflag / ir_setflag */
	u8 green_i, red_i, ir_i;
	u16 wear_ref[3];	/* Start() parameters, trailer words */
	u32 chip_id;		/* regs 0xbc..0xbf, big endian */
	u16 amb;

	u8 raw[BD1XXX_RAW_SIZE];	/* last complete frame, for read(2) */
};

static int bd1xxx_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret;

	/* Stock: one transfer, write the register, repeated start, read. */
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;

	return 0;
}

static int bd1xxx_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = client->addr, .flags = 0, .len = sizeof(buf),
		.buf = buf,
	};
	int ret;

	/* Stock: one two-byte message [reg, value]. */
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	return 0;
}

/* u16 little endian: the low byte is at @reg, the high byte at @reg + 1. */
static int bd1xxx_read_word(struct i2c_client *client, u8 reg, u16 *val)
{
	u8 lo, hi;
	int ret;

	ret = bd1xxx_read_reg(client, reg, &lo);
	if (!ret)
		ret = bd1xxx_read_reg(client, reg + 1, &hi);
	if (!ret)
		*val = lo | (hi << 8);

	return ret;
}

/* Must be called with data->lock held (or before the device is visible). */
static int bd1xxx_power_on(struct bd1xxx_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;

	if (data->powered)
		return 0;

	if (data->vdd) {
		ret = regulator_enable(data->vdd);
		if (ret) {
			dev_err(dev, "cannot enable regulator: %d\n", ret);
			return ret;
		}
	}
	if (gpio_is_valid(data->power_gpio))
		gpio_set_value_cansleep(data->power_gpio, 1);

	msleep(BD1XXX_POWERUP_DELAY_MS);
	data->powered = true;
	return 0;
}

static void bd1xxx_power_off(struct bd1xxx_data *data)
{
	if (!data->powered)
		return;

	if (gpio_is_valid(data->power_gpio))
		gpio_set_value_cansleep(data->power_gpio, 0);
	if (data->vdd)
		regulator_disable(data->vdd);

	data->powered = false;
}

static void bd1xxx_free(struct kref *ref)
{
	kfree(container_of(ref, struct bd1xxx_data, ref));
}

static enum bd1xxx_path bd1xxx_mode_path(unsigned int mode)
{
	/* Init+0x2dc/0x990/0x102c and Data_Read+0x64/0xb8c/0x1704 */
	if (mode == BD1XXX_MODE_SPO2)
		return BD1XXX_PATH_SPO2;
	if (mode >= 3 && mode <= 4)
		return BD1XXX_PATH_HRV;
	return BD1XXX_PATH_HR;
}

/*
 * BD_Sensor_Init (stock 0x56d2f8) and BD_Sensor_Start (0x56cefc).  Start
 * itself writes no register: it only resets state and re-reads the ID.
 * Stock ignores write errors and carries on; stop at the first one here.
 * Must be called with data->lock held and the chip powered.
 */
static int bd1xxx_program(struct bd1xxx_data *data)
{
	struct i2c_client *client = data->client;
	struct bd1xxx_reg seq[BD1XXX_MAX_SEQ];
	unsigned int mode = data->mode;
	u8 id[4], dc_red, dc_ir, tail;
	bool variant_b, spo2 = mode == BD1XXX_MODE_SPO2;
	int n = 0, i, ret;

	/* Init+0x3c: regs 0xbc, 0xbd, 0xbe, 0xbf in this order. */
	ret = bd1xxx_read_reg(client, BD1XXX_REG_ID_LO, &id[0]);
	if (!ret)
		ret = bd1xxx_read_reg(client, BD1XXX_REG_ID_HI, &id[1]);
	if (!ret)
		ret = bd1xxx_read_reg(client, BD1XXX_REG_REV_LO, &id[2]);
	if (!ret)
		ret = bd1xxx_read_reg(client, BD1XXX_REG_REV_HI, &id[3]);
	if (ret)
		return ret;
	/* Init+0x19c: any other ID returns before the first write. */
	if (id[0] != BD1XXX_ID_LO_BD1820 || id[1] != BD1XXX_ID_HI_BD1820)
		return -ENODEV;

	/*
	 * Init+0x1b4: revision 0x0101 (stock "Id_flag") picks the second
	 * parameter set of every pair.  Values per variant (Init+0x1d4..0x2b0):
	 *   green_i      reg 0x04 value: 0x51 (rev 0101) or 0x59
	 *   dc_red/dc_ir initial red/ir DC values: 3 or 0x0a
	 *   red_i/ir_i   bytes 1 and 2 of the u16 pair passed to Init, from
	 *                the tables at kernel data 0xffffff800914ac30..0xac68
	 *                (mode 2: 0x78/0x68 or 0x60/0x58; others 0x00/0x10 or
	 *                0x00/0x38)
	 * ref[] are the u16 triples passed to Start (Start+0x5c..0xd4, same
	 * tables): mode 2 {32, 30000, 6000}; others {110, 2000 or 15000, 5}.
	 */
	variant_b = id[2] == 1 && id[3] == 1;
	data->green_i = variant_b ? 0x51 : 0x59;
	dc_red = dc_ir = variant_b ? 3 : 0x0a;
	if (spo2) {
		data->red_i = variant_b ? 0x60 : 0x78;
		data->ir_i = variant_b ? 0x58 : 0x68;
		data->wear_ref[0] = 32;
		data->wear_ref[1] = 30000;
		data->wear_ref[2] = 6000;
	} else {
		data->red_i = 0x00;
		data->ir_i = variant_b ? 0x38 : 0x10;
		data->wear_ref[0] = 110;
		data->wear_ref[1] = variant_b ? 15000 : 2000;
		data->wear_ref[2] = 5;
	}
	data->chip_id = (id[0] << 24) | (id[1] << 16) | (id[2] << 8) | id[3];

#define SEQ(r, v) do { seq[n].reg = (r); seq[n].val = (v); n++; } while (0)
	if (spo2) {
		/* Init+0x2dc: mode 2 block */
		SEQ(0x01, 0x08);
		SEQ(0x03, 0x14);
		SEQ(BD1XXX_REG_GREEN_I, 0x00);
		SEQ(0x05, 0xa0);
	} else {
		/* Init+0x990: modes 3, 4; Init+0x102c: all other modes */
		SEQ(0x01, 0x01);
		SEQ(0x03, bd1xxx_mode_path(mode) == BD1XXX_PATH_HRV ?
				 0x02 : 0x14);
		SEQ(BD1XXX_REG_GREEN_I, data->green_i);
		SEQ(0x05, 0xa4);
	}
	SEQ(0x06, 0x21);
	SEQ(0x07, 0x78);
	SEQ(0x08, 0x08);
	SEQ(0x09, 0x29);
	SEQ(0x0a, 0xb1);
	SEQ(0x0b, 0x00);
	SEQ(0x26, 0x8f);
	SEQ(0x27, 0x81);
	SEQ(0x2a, 0x20);
	SEQ(0x2b, 0x00);
	SEQ(BD1XXX_REG_RED_I, spo2 ? data->red_i : 0x00);
	SEQ(BD1XXX_REG_IR_I, data->ir_i);
	SEQ(0x16, spo2 ? 0x63 : 0xa0);
	SEQ(0x2f, 0x33);
	SEQ(BD1XXX_REG_DC_BASE, 0x00);
	SEQ(BD1XXX_REG_DC_BASE + 1, spo2 ? dc_red : 0x00);
	SEQ(BD1XXX_REG_DC_BASE + 2, spo2 ? dc_ir : 0x00);
	tail = variant_b ? 0x00 : 0x03;
	SEQ(0x38, tail);
	SEQ(0x39, tail);
	SEQ(0x3a, tail);
	SEQ(BD1XXX_REG_CTRL, 0x4b);	/* Init+0x95c, +0xff8, +0x1694 */
#undef SEQ

	data->programmed = true;
	for (i = 0; i < n; i++) {
		ret = bd1xxx_write_reg(client, seq[i].reg, seq[i].val);
		if (ret)
			return ret;
	}

	/* Start(): running flag, DC loops armed, counters cleared. */
	memset(data->frame, 0, sizeof(data->frame));
	memset(data->raw, 0, sizeof(data->raw));
	data->count = 0;
	data->amb = 0;
	data->dc[0] = 0;
	data->dc[1] = dc_red;
	data->dc[2] = dc_ir;
	data->dc_pending[0] = true;
	data->dc_pending[1] = true;
	data->run_mode = mode;
	data->period = ms_to_ktime(mode == BD1XXX_MODE_FINGER ?
				   BD1XXX_POLL_FINGER_MS : BD1XXX_POLL_MS);
	data->running = true;

	dev_info(&client->dev, "mode %u rev %02x%02x programmed\n", mode,
		 id[2], id[3]);
	return 0;
}

/* BD_Sensor_Close (stock 0x56d218): reg 0x00 = 0x00, 0x80, 0x00. */
static void bd1xxx_close(struct bd1xxx_data *data)
{
	static const u8 seq[] = { 0x00, 0x80, 0x00 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(seq); i++)
		bd1xxx_write_reg(data->client, BD1XXX_REG_CTRL, seq[i]);
}

/* Caller holds ctl. */
static void bd1xxx_poll_cancel(struct bd1xxx_data *data)
{
	mutex_lock(&data->lock);
	data->running = false;
	mutex_unlock(&data->lock);

	/* Timer first: it queues the work.  Neither needs data->lock here. */
	hrtimer_cancel(&data->timer);
	cancel_work_sync(&data->work);
}

/*
 * Stock stop path (bd1xxx_init_heartrate(0) at 0x56bb64): cancel the timer,
 * msleep(2), BD_Sensor_Close, clear the buffer, power off.
 * Caller holds ctl.
 */
static void bd1xxx_stop(struct bd1xxx_data *data)
{
	bool programmed;

	bd1xxx_poll_cancel(data);

	mutex_lock(&data->lock);
	programmed = data->programmed;
	mutex_unlock(&data->lock);
	if (programmed)
		msleep(BD1XXX_STOP_DELAY_MS);

	mutex_lock(&data->lock);
	if (data->programmed && data->powered)
		bd1xxx_close(data);
	data->programmed = false;
	memset(data->frame, 0, sizeof(data->frame));
	data->count = 0;
	bd1xxx_power_off(data);
	mutex_unlock(&data->lock);
}

/*
 * Stock start path (bd1xxx_init_heartrate.part.8 at 0x56b9e0): power on,
 * msleep(10), clear the buffer, Init, Start, arm the hrtimer (first sample
 * one period later).  Caller holds ctl.
 */
static int bd1xxx_start(struct bd1xxx_data *data)
{
	int ret;

	/* Restart with a possibly new mode: stock simply re-runs Init. */
	bd1xxx_poll_cancel(data);

	mutex_lock(&data->lock);
	ret = bd1xxx_power_on(data);
	if (!ret)
		ret = bd1xxx_program(data);
	mutex_unlock(&data->lock);
	if (ret) {
		dev_err(&data->client->dev, "cannot start sampling: %d\n", ret);
		bd1xxx_stop(data);
		return ret;
	}

	hrtimer_start(&data->timer, data->period, HRTIMER_MODE_REL);
	return 0;
}

/* Stock 0x56b170: queue the work, re-arm relative to now. */
static enum hrtimer_restart bd1xxx_timer_fn(struct hrtimer *timer)
{
	struct bd1xxx_data *data = container_of(timer, struct bd1xxx_data,
						timer);

	schedule_work(&data->work);
	hrtimer_forward_now(timer, data->period);

	return HRTIMER_RESTART;
}

static int bd1xxx_set_enable(struct bd1xxx_data *data, bool enable)
{
	int ret = 0;

	mutex_lock(&data->ctl);
	if (data->removed) {
		ret = -ENODEV;
	} else if (enable) {
		if (!data->suspended)
			ret = bd1xxx_start(data);
		data->enabled = !ret;
	} else {
		data->enabled = false;
		bd1xxx_stop(data);
	}
	mutex_unlock(&data->ctl);

	return ret;
}

static u16 bd1xxx_fget(struct bd1xxx_data *data, int ch, int idx)
{
	return le16_to_cpu(data->frame[ch][idx]);
}

static void bd1xxx_fput(struct bd1xxx_data *data, int ch, int idx, u16 val)
{
	data->frame[ch][idx] = cpu_to_le16(val);
}

/*
 * DC cancel loop for one channel: raise the DC register while the sample is
 * above 0xb3b0, lower it while the sample is at or below 0x2327, and stop
 * adjusting (until the frame check re-arms it) once neither applies.
 * Data_Read+0x360 (mode 2, ch1 -> 0x33 and ch2 -> 0x34), +0xe84 (modes 3, 4)
 * and +0x1a00 (others), both of the latter ch0 -> 0x32.
 */
static int bd1xxx_dc_loop(struct bd1xxx_data *data, unsigned int pending,
			  unsigned int ch, u16 val)
{
	u8 *dc = &data->dc[ch];
	int ret = 0;

	if (val > BD1XXX_DC_HIGH && *dc != 0xff) {
		*dc += (val > BD1XXX_DC_SAT && *dc <= 0xfc) ? 2 : 1;
		return bd1xxx_write_reg(data->client, BD1XXX_REG_DC_BASE + ch,
					*dc);
	}

	if (val <= BD1XXX_DC_LOW && *dc) {
		*dc -= (val <= BD1XXX_DC_ZERO && *dc > 2) ? 2 : 1;
		ret = bd1xxx_write_reg(data->client, BD1XXX_REG_DC_BASE + ch,
				       *dc);
	}
	data->dc_pending[pending] = false;

	return ret;
}

/* Data_Read+0x758 (mode 2), +0x1348 (modes 3, 4), +0x1c04 (others) */
static void bd1xxx_store(struct bd1xxx_data *data, u16 ch0, u16 ch1, u16 ch2)
{
	bd1xxx_fput(data, 0, data->count, ch0);
	bd1xxx_fput(data, 1, data->count, ch1);
	bd1xxx_fput(data, 2, data->count, ch2);
	if (data->count < BD1XXX_NSAMPLES)
		data->count++;
}

static bool bd1xxx_out_of_band(u16 val)
{
	return val > BD1XXX_BAND_HI || val <= BD1XXX_BAND_LO;
}

/*
 * Frame end: Data_Read+0x8e4 (mode 2), +0x1500 (3, 4), +0x1d90 (others), then
 * the trailer at +0x20a8..+0x21d8.  The wear state is a constant 2 because
 * Wear_judge* is not implemented, so the stock reg 0x04 / 0x0e toggle at
 * +0x7f0 / +0x1ca0 (state 1) never happens either.
 */
static bool bd1xxx_frame_done(struct bd1xxx_data *data, enum bd1xxx_path path)
{
	unsigned int n = data->count, i;
	u16 last0, last1, last2, f08, f0c;

	if (n < BD1XXX_FRAME_MIN)
		return false;

	last0 = bd1xxx_fget(data, 0, n - 1);
	last1 = bd1xxx_fget(data, 1, n - 1);
	last2 = bd1xxx_fget(data, 2, n - 1);
	if (path == BD1XXX_PATH_SPO2) {
		f08 = last1;
		f0c = last0;
		if (bd1xxx_out_of_band(last1))
			data->dc_pending[0] = true;
		if (bd1xxx_out_of_band(last2))
			data->dc_pending[1] = true;
	} else {
		f08 = last2;
		f0c = last1;
		if (bd1xxx_out_of_band(last0))
			data->dc_pending[0] = true;
	}

	for (i = n; i < BD1XXX_NSAMPLES; i++) {
		bd1xxx_fput(data, 0, i, 0);
		bd1xxx_fput(data, 1, i, 0);
		bd1xxx_fput(data, 2, i, 0);
	}

	bd1xxx_fput(data, 0, BD1XXX_IDX_T0, data->dc[0]);
	bd1xxx_fput(data, 0, BD1XXX_IDX_T1, f08);
	bd1xxx_fput(data, 0, BD1XXX_IDX_T2, f0c);
	bd1xxx_fput(data, 0, BD1XXX_IDX_T3, data->dc[1]);
	bd1xxx_fput(data, 0, BD1XXX_IDX_T4, data->dc[2]);
	bd1xxx_fput(data, 0, BD1XXX_IDX_COUNT, n);
	bd1xxx_fput(data, 0, BD1XXX_IDX_WEAR, 2);
	bd1xxx_fput(data, 0, BD1XXX_IDX_AMB, data->amb);

	bd1xxx_fput(data, 1, BD1XXX_IDX_T0, data->chip_id & 0xffff);
	bd1xxx_fput(data, 1, BD1XXX_IDX_T1, data->chip_id >> 16);
	bd1xxx_fput(data, 1, BD1XXX_IDX_T2, data->wear_ref[0]);
	bd1xxx_fput(data, 1, BD1XXX_IDX_T3, data->wear_ref[1]);
	bd1xxx_fput(data, 1, BD1XXX_IDX_T4, data->wear_ref[2]);
	bd1xxx_fput(data, 1, BD1XXX_IDX_COUNT, n);

	bd1xxx_fput(data, 2, BD1XXX_IDX_T2, data->green_i);
	bd1xxx_fput(data, 2, BD1XXX_IDX_T3, data->red_i);
	bd1xxx_fput(data, 2, BD1XXX_IDX_T4, data->ir_i);
	bd1xxx_fput(data, 2, BD1XXX_IDX_COUNT, n);

	data->count = 0;
	return true;
}

/*
 * One poll of BD_Sensor_Data_Read.  Returns 1 when a frame is complete, 0
 * when more samples are needed and < 0 on an I2C error.  A read error leaves
 * the state untouched; stock stores the garbage and drops the frame.
 * Called with data->lock held.
 */
static int bd1xxx_sample(struct bd1xxx_data *data)
{
	struct i2c_client *client = data->client;
	enum bd1xxx_path path = bd1xxx_mode_path(data->run_mode);
	u16 ch0, ch1, ch2, amb;
	int ret, err = 0, i;

	/* Data_Read+0x64 / +0xb8c / +0x1704: regs 0x1a, 0x18, 0x1c, 0x1e. */
	ret = bd1xxx_read_word(client, BD1XXX_REG_DATA_GREEN, &ch0);
	if (!ret)
		ret = bd1xxx_read_word(client, BD1XXX_REG_DATA_AMB, &amb);
	if (!ret)
		ret = bd1xxx_read_word(client, BD1XXX_REG_DATA_RED, &ch1);
	if (!ret)
		ret = bd1xxx_read_word(client, BD1XXX_REG_DATA_IR, &ch2);
	if (ret)
		return ret;
	data->amb = amb;

	switch (path) {
	case BD1XXX_PATH_SPO2:
		if (data->dc_pending[0])
			err = bd1xxx_dc_loop(data, 0, 1, ch1);
		if (data->dc_pending[1])
			err |= bd1xxx_dc_loop(data, 1, 2, ch2);
		bd1xxx_store(data, ch0, ch1, ch2);
		break;
	case BD1XXX_PATH_HR:
		if (data->dc_pending[0])
			err = bd1xxx_dc_loop(data, 0, 0, ch0);
		bd1xxx_store(data, ch0, ch1, ch2);
		break;
	case BD1XXX_PATH_HRV:
		if (data->dc_pending[0])
			err = bd1xxx_dc_loop(data, 0, 0, ch0);
		/*
		 * Samples come from the FIFO registers 0x20..0x25 while the
		 * level in reg 0x30 is above 2 (Data_Read+0x13e4, +0x10c8).
		 */
		for (i = 0; i < BD1XXX_DRAIN_MAX; i++) {
			u8 level;

			ret = bd1xxx_read_reg(client, BD1XXX_REG_FIFO_LEVEL,
					      &level);
			if (ret)
				return ret;
			if (level <= 2)
				break;
			ret = bd1xxx_read_word(client, BD1XXX_REG_FIFO_GREEN,
					       &ch0);
			if (!ret)
				ret = bd1xxx_read_word(client,
						       BD1XXX_REG_FIFO_RED,
						       &ch1);
			if (!ret)
				ret = bd1xxx_read_word(client,
						       BD1XXX_REG_FIFO_IR,
						       &ch2);
			if (ret)
				return ret;
			bd1xxx_store(data, ch0, ch1, ch2);
		}
		break;
	}

	if (!bd1xxx_frame_done(data, path))
		return err;

	return err ? err : 1;
}

/* Stock 0x56afd4.  Mode 15 would call BD_finger_math, which is not here. */
static void bd1xxx_work(struct work_struct *work)
{
	struct bd1xxx_data *data = container_of(work, struct bd1xxx_data, work);
	int ret;

	mutex_lock(&data->lock);
	if (!data->running || data->run_mode == BD1XXX_MODE_FINGER)
		goto out;

	ret = bd1xxx_sample(data);
	if (ret < 0) {
		dev_warn_ratelimited(&data->client->dev, "sample failed: %d\n",
				     ret);
		goto out;
	}
	if (ret != 1)
		goto out;

	/* Work+0x11c: copy the frame, then REL_X/Y/Z = sample 0 and SYN. */
	memcpy(data->raw, data->frame, sizeof(data->frame));
	input_report_rel(data->input, REL_X, bd1xxx_fget(data, 0, 0));
	input_report_rel(data->input, REL_Y, bd1xxx_fget(data, 1, 0));
	input_report_rel(data->input, REL_Z, bd1xxx_fget(data, 2, 0));
	input_sync(data->input);
out:
	mutex_unlock(&data->lock);
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	/* Stock prints a state field it never updates: always 0. */
	return scnprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct bd1xxx_data *data = dev_get_drvdata(dev);
	long val;
	int ret;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;

	ret = bd1xxx_set_enable(data, val != 0);
	if (ret)
		return ret;

	return count;
}

/* The HAL strcmp()s the content: no trailing newline, like stock. */
static ssize_t chipinfo_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "bd1820");
}

static DEVICE_ATTR(enable, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH,
		   enable_show, enable_store);
static DEVICE_ATTR(chipinfo, S_IRUSR | S_IRGRP | S_IROTH, chipinfo_show, NULL);

static struct attribute *bd1xxx_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_chipinfo.attr,
	NULL,
};

static const struct attribute_group bd1xxx_attr_group = {
	.attrs = bd1xxx_attrs,
};

static const struct attribute_group *bd1xxx_groups[] = {
	&bd1xxx_attr_group,
	NULL,
};

/*
 * Descriptors can outlive the client (adapter unbind): each one holds a
 * reference to data, and I/O after remove returns -ENODEV.
 */
static int bd1xxx_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct bd1xxx_data *data = container_of(misc, struct bd1xxx_data, misc);

	/* misc_open() holds misc_mtx: remove has not deregistered us yet. */
	kref_get(&data->ref);
	file->private_data = data;

	return nonseekable_open(inode, file);
}

static int bd1xxx_misc_release(struct inode *inode, struct file *file)
{
	struct bd1xxx_data *data = file->private_data;

	kref_put(&data->ref, bd1xxx_free);
	return 0;
}

static ssize_t bd1xxx_misc_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct bd1xxx_data *data = file->private_data;
	ssize_t len = min_t(size_t, count, sizeof(data->raw));

	/* The last complete frame; zero until the first one. */
	mutex_lock(&data->lock);
	if (data->removed)
		len = -ENODEV;
	else if (copy_to_user(buf, data->raw, len))
		len = -EFAULT;
	mutex_unlock(&data->lock);

	return len;
}

static ssize_t bd1xxx_misc_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct bd1xxx_data *data = file->private_data;
	u8 cmd[BD1XXX_WRITE_MAX] = { };
	int ret;

	if (count > sizeof(cmd))
		count = sizeof(cmd);
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;

	/*
	 * byte 0 = test mode, byte 1 = enable. Stock returns the
	 * copy_from_user remainder (0) here; return the byte count instead.
	 */
	mutex_lock(&data->lock);
	data->mode = cmd[0];
	mutex_unlock(&data->lock);

	ret = bd1xxx_set_enable(data, cmd[1] != 0);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations bd1xxx_misc_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.open = bd1xxx_misc_open,
	.release = bd1xxx_misc_release,
	.read = bd1xxx_misc_read,
	.write = bd1xxx_misc_write,
};

static int bd1xxx_parse_dt(struct bd1xxx_data *data)
{
	struct device *dev = &data->client->dev;
	const char *vdd_name;
	int gpio;

	data->power_gpio = -ENOENT;
	data->vdd = NULL;

	if (!dev->of_node)
		return 0;

	if (!of_property_read_string(dev->of_node, "vdd_name", &vdd_name)) {
		data->vdd = devm_regulator_get(dev, vdd_name);
		if (IS_ERR(data->vdd)) {
			int ret = PTR_ERR(data->vdd);

			data->vdd = NULL;
			if (ret == -EPROBE_DEFER)
				return ret;
			dev_warn(dev, "regulator %s unavailable: %d\n",
				 vdd_name, ret);
		} else if (regulator_set_voltage(data->vdd, BD1XXX_VDD_UV,
						 BD1XXX_VDD_UV)) {
			dev_warn(dev, "cannot set %s voltage\n", vdd_name);
		}
	}

	gpio = of_get_named_gpio(dev->of_node, "power-gpio", 0);
	if (gpio == -EPROBE_DEFER)
		return gpio;
	if (gpio_is_valid(gpio))
		data->power_gpio = gpio;

	return 0;
}

static int bd1xxx_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct bd1xxx_data *data;
	struct input_dev *input;
	u8 id_hi, id_lo;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "i2c function not supported\n");
		return -ENODEV;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	kref_init(&data->ref);
	mutex_init(&data->ctl);
	mutex_init(&data->lock);
	hrtimer_init(&data->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	data->timer.function = bd1xxx_timer_fn;
	INIT_WORK(&data->work, bd1xxx_work);

	ret = bd1xxx_parse_dt(data);
	if (ret)
		goto err_free;

	if (gpio_is_valid(data->power_gpio)) {
		ret = devm_gpio_request_one(dev, data->power_gpio,
					    GPIOF_OUT_INIT_LOW,
					    "bd1xxx-power-gpio");
		if (ret) {
			dev_err(dev, "cannot request power gpio %d: %d\n",
				data->power_gpio, ret);
			goto err_free;
		}
	}

	ret = bd1xxx_power_on(data);
	if (ret)
		goto err_free;

	/*
	 * Read-only ID check. Stock also writes reg 0x03 = 0x0a and reads it
	 * back first; the meaning of that register is unknown, so skip it.
	 */
	ret = bd1xxx_read_reg(client, BD1XXX_REG_ID_HI, &id_hi);
	if (!ret)
		ret = bd1xxx_read_reg(client, BD1XXX_REG_ID_LO, &id_lo);
	if (ret) {
		dev_err(dev, "cannot read chip id: %d\n", ret);
		goto err_off;
	}
	if (!IS_ENABLED(CONFIG_HEARTRATE_BD1820) ||
	    id_hi != BD1XXX_ID_HI_BD1820 || id_lo != BD1XXX_ID_LO_BD1820) {
		dev_err(dev, "unsupported chip id 0x%02x%02x\n", id_hi, id_lo);
		ret = -ENODEV;
		goto err_off;
	}
	dev_info(dev, "found bd1820 (id 0x%02x%02x)\n", id_hi, id_lo);

	/* The sensor stays off until userspace enables it. */
	bd1xxx_power_off(data);

	/* No parent, as stock: /devices/virtual/input/inputN. */
	input = input_allocate_device();
	if (!input) {
		ret = -ENOMEM;
		goto err_off;
	}

	input->name = BD1XXX_INPUT_NAME;
	input->id.bustype = BUS_HOST;
	input_set_capability(input, EV_KEY, BD1XXX_KEY_CODE);
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);
	input_set_capability(input, EV_REL, REL_Z);
	input_set_drvdata(input, data);
	input->dev.groups = bd1xxx_groups;
	data->input = input;

	i2c_set_clientdata(client, data);
	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "cannot register input device: %d\n", ret);
		input_free_device(input);
		goto err_off;
	}

	data->misc.minor = MISC_DYNAMIC_MINOR;
	data->misc.name = BD1XXX_MISC_NAME;
	data->misc.fops = &bd1xxx_misc_fops;
	ret = misc_register(&data->misc);
	if (ret) {
		dev_err(dev, "cannot register misc device: %d\n", ret);
		input_unregister_device(input);
		goto err_off;
	}

	return 0;

err_off:
	bd1xxx_power_off(data);
err_free:
	kref_put(&data->ref, bd1xxx_free);
	return ret;
}

static int bd1xxx_remove(struct i2c_client *client)
{
	struct bd1xxx_data *data = i2c_get_clientdata(client);

	misc_deregister(&data->misc);

	/*
	 * Stop the poller before the input device goes away: the work reports
	 * events.  Once removed is set no enable can restart it.
	 */
	mutex_lock(&data->ctl);
	mutex_lock(&data->lock);
	data->removed = true;
	data->enabled = false;
	mutex_unlock(&data->lock);
	bd1xxx_stop(data);
	mutex_unlock(&data->ctl);

	input_unregister_device(data->input);

	kref_put(&data->ref, bd1xxx_free);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int bd1xxx_suspend(struct device *dev)
{
	struct bd1xxx_data *data = dev_get_drvdata(dev);

	/*
	 * Stock keeps the chip programmed across suspend (its hrtimer just
	 * stops).  Do the same: stop polling only, so the DC-cancel state is
	 * not lost on every wake; resume picks up where it left.
	 */
	mutex_lock(&data->ctl);
	data->suspended = true;
	bd1xxx_poll_cancel(data);
	mutex_unlock(&data->ctl);

	return 0;
}

static int bd1xxx_resume(struct device *dev)
{
	struct bd1xxx_data *data = dev_get_drvdata(dev);
	bool resume_poll;
	int ret = 0;

	mutex_lock(&data->ctl);
	data->suspended = false;
	if (data->enabled && !data->removed) {
		mutex_lock(&data->lock);
		resume_poll = data->programmed && data->powered;
		data->running = resume_poll;
		mutex_unlock(&data->lock);
		if (resume_poll) {
			hrtimer_start(&data->timer, data->period,
				      HRTIMER_MODE_REL);
		} else {
			/* enabled while suspended: start from scratch */
			ret = bd1xxx_start(data);
			if (ret)
				data->enabled = false;
		}
	}
	mutex_unlock(&data->ctl);

	return ret;
}
#endif

static SIMPLE_DEV_PM_OPS(bd1xxx_pm_ops, bd1xxx_suspend, bd1xxx_resume);

static const struct i2c_device_id bd1xxx_id[] = {
	{ BD1XXX_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bd1xxx_id);

static const struct of_device_id bd1xxx_of_match[] = {
	{ .compatible = "heartrate,bd1xxx" },
	{ }
};
MODULE_DEVICE_TABLE(of, bd1xxx_of_match);

static struct i2c_driver bd1xxx_driver = {
	.driver = {
		.name = BD1XXX_DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = bd1xxx_of_match,
		.pm = &bd1xxx_pm_ops,
	},
	.probe = bd1xxx_probe,
	.remove = bd1xxx_remove,
	.id_table = bd1xxx_id,
};
module_i2c_driver(bd1xxx_driver);

MODULE_DESCRIPTION("Rohm BD1xxx (BD1820) heart-rate sensor driver");
MODULE_LICENSE("GPL");
