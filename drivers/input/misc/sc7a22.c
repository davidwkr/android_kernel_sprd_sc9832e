/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SC7A22/SC7A20 accelerometer driver for the legacy Spreadtrum sensor HAL.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sprd_panel_notifier.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define SC7A22_DRV_NAME                  "sc7a22"
#define SC7A22_MISC_NAME                 "qmax981"
#define SC7A22_INPUT_NAME                "accelerometer"
#define SC7A22_INPUT_STEP_NAME           "sc7a22_input_step"
#define SC7A22_INPUT_KEY_NAME            "sc7a22_input_key"

#define SC7A22_REG_WHO_AM_I              0x0f
#define SC7A22_REG_H_WHO_AM_I            0x01
#define SC7A22_REG_BANK                  0x7f
#define SC7A22_REG_H_STATUS              0x0b
#define SC7A22_REG_OUT_X_L_CMD           0xa8
#define SC7A22_REG_H_OUT_X_H_CMD         0x8c

#define SC7A22_ID                        0x13
#define SC7A22_H_ID                      0x18
#define SC7A22_BANK_0                    0x00
#define SC7A22_BANK_90                   0x90
#define SC7A22_H_DATA_READY              0x03
#define SC7A22_H_READY_RETRIES           50

#define SC7A22_REG_FLAGS_13              0x06
#define SC7A22_REG_MOTION_13             0x14
#define SC7A22_REG_STEP_13               0x95	/* 0x15 | 0x80, 3 bytes */
#define SC7A22_REG_STEP_CTRL_13          0x10
#define SC7A22_REG_MOTION_18             0x3b
#define SC7A22_REG_STEP_18               0xbc	/* 0x3c | 0x80, 2 bytes */
#define SC7A22_REG_STEP_CTRL_18          0x3a
#define SC7A22_REG_FLAGS_18              0x3e	/* bank 0x90 */
#define SC7A22_DEFAULT_ANGLE             10
#define SC7A22_DEFAULT_HANDUP_CTRL       0x11
#define SC7A22_FLAG_HAND_UP              0x02
#define SC7A22_FLAG_HAND_DOWN            0x04
#define SC7A22_IRQ_RECONCILE             2
#define SC7A22_FLAG_RETRIES              3
#define SC7A22_FLAG_RETRY_MS             50
#define SC7A22_STEP_MAX                  0xffff

#define SC7A22_DEFAULT_DELAY_MS          50
#define SC7A22_MIN_DELAY_MS              1
#define SC7A22_MAX_DELAY_MS              1000
#define SC7A22_ABS_MAX                   (8 * 1024)

#define QMAX981_IOCTL_MAGIC              77
#define QMAX981_IOCTL_SET_DELAY          _IOW(QMAX981_IOCTL_MAGIC, 0, int)
#define QMAX981_IOCTL_GET_DELAY          _IOR(QMAX981_IOCTL_MAGIC, 1, int)
#define QMAX981_IOCTL_SET_ENABLE         _IOW(QMAX981_IOCTL_MAGIC, 2, int)
#define QMAX981_IOCTL_GET_ENABLE         _IOR(QMAX981_IOCTL_MAGIC, 3, int)
#define QMAX981_IOCTL_CALIBRATION        _IOW(QMAX981_IOCTL_MAGIC, 4, int)

struct sc7a22_axis_map {
	s8 sign[3];
	u8 axis[3];
};

static const struct sc7a22_axis_map sc7a22_axis_maps[] = {
	{ {  1,  1,  1 }, { 0, 1, 2 } },
	{ { -1,  1,  1 }, { 1, 0, 2 } },
	{ { -1, -1,  1 }, { 0, 1, 2 } },
	{ {  1, -1,  1 }, { 1, 0, 2 } },
	{ { -1,  1, -1 }, { 0, 1, 2 } },
	{ {  1,  1, -1 }, { 1, 0, 2 } },
	{ {  1, -1, -1 }, { 0, 1, 2 } },
	{ { -1, -1, -1 }, { 1, 0, 2 } },
};

struct sc7a22_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct miscdevice miscdev;
	struct delayed_work work;
	struct delayed_work flag_work; /* retries a failed flag read */
	struct mutex irq_lock; /* Serializes flag servicing and re-arming. */
	unsigned int flag_retries; /* under irq_lock */
	bool suspended; /* under irq_lock: no retry is queued */
	struct mutex lock; /* Serializes enable, delay, and work transitions. */
	struct kref ref; /* probe + one per open misc descriptor */
	bool removed; /* under lock: no enabling after teardown started */
	atomic_t enabled;
	unsigned int delay_ms;
	unsigned int layout;
	u8 chip_id;
	bool resume_enabled;
	struct class *control_class;
	struct device *control_dev;

	struct input_dev *key_input;
	struct input_dev *step_input;
	struct mutex op_lock; /* Bank switching and step counter accesses. */
	struct notifier_block panel_nb;
	int irq_gpio;
	int irq;
	u32 angle;
	u32 handup_ctrl;
	u32 step_value;
	u32 step_offset;
	bool hand_start;
	bool screen_on;
	bool panel_registered;
	bool sysfs_group;
	bool irq_requested;
	bool gpio_owned;
	bool misc_registered;
	unsigned int map_layout;
};

static int sc7a22_read_reg(struct sc7a22_data *data, u8 reg)
{
	return i2c_smbus_read_byte_data(data->client, reg);
}

static int sc7a22_write_reg(struct sc7a22_data *data, u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(data->client, reg, value);
}

struct sc7a22_reg_value {
	u8 reg;
	u8 value;
};

static int sc7a22_write_sequence(struct sc7a22_data *data,
				 const struct sc7a22_reg_value *sequence,
				 size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		u8 reg = sequence[i].reg;
		u8 value = sequence[i].value;

		ret = sc7a22_write_reg(data, reg, value);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int sc7a22_detect(struct sc7a22_data *data)
{
	int primary_id;
	int chip_id;
	int ret;

	primary_id = sc7a22_read_reg(data, SC7A22_REG_WHO_AM_I);
	if (primary_id < 0)
		return primary_id;
	if (primary_id == SC7A22_ID) {
		data->chip_id = primary_id;
		return 0;
	}

	/* The 0x18 silicon exposes its ID in bank 0 at register 0x01. */
	ret = sc7a22_write_reg(data, SC7A22_REG_BANK, SC7A22_BANK_0);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	chip_id = sc7a22_read_reg(data, SC7A22_REG_H_WHO_AM_I);
	if (chip_id < 0)
		return chip_id;
	if (chip_id != SC7A22_H_ID) {
		dev_err(&data->client->dev,
			"unexpected chip ids 0x%02x/0x%02x\n",
			primary_id, chip_id);
		return -ENODEV;
	}

	data->chip_id = chip_id;
	return 0;
}

static int sc7a22_set_bank(struct sc7a22_data *data, u8 bank)
{
	int ret;

	ret = sc7a22_write_reg(data, SC7A22_REG_BANK, bank);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);

	return 0;
}

/* sc7a22_i2c_probe, 0x13 branch: base configuration. */
static int sc7a22_hw_init_13(struct sc7a22_data *data)
{
	const struct sc7a22_reg_value sequence[] = {
		{ 0x20, 0x4f },
		{ 0x23, 0x88 },
		{ 0x1f, 0x08 },
		{ 0x57, 0x08 },
	};

	return sc7a22_write_sequence(data, sequence, ARRAY_SIZE(sequence));
}

/* sc7a22h_initialize: soft reset, then the 0x18 core configuration. */
static int sc7a22_hw_init_18(struct sc7a22_data *data)
{
	const struct sc7a22_reg_value core_sequence[] = {
		{ 0x40, 0x26 },
		{ 0x41, 0x00 },
		{ 0x05, 0x50 },
		{ 0x08, 0x05 },
	};
	int ret;

	ret = sc7a22_set_bank(data, SC7A22_BANK_0);
	if (ret < 0)
		return ret;
	ret = sc7a22_write_reg(data, 0x4a, 0xa5);
	if (ret < 0)
		return ret;
	msleep(50);

	ret = sc7a22_set_bank(data, SC7A22_BANK_0);
	if (ret < 0)
		return ret;
	ret = sc7a22_write_reg(data, 0x7d, 0x04);
	if (ret < 0)
		return ret;
	msleep(30);

	return sc7a22_write_sequence(data, core_sequence,
				     ARRAY_SIZE(core_sequence));
}

static int sc7a22_hw_init(struct sc7a22_data *data)
{
	if (data->chip_id == SC7A22_H_ID)
		return sc7a22_hw_init_18(data);

	return sc7a22_hw_init_13(data);
}

/*
 * sc7a22_set_hand_up_mode. The angle property goes to the hand-up angle
 * register, the layout (plus 0x70) to the orientation register.
 */
static int sc7a22_set_hand_up_mode(struct sc7a22_data *data)
{
	int ret;

	if (data->chip_id == SC7A22_ID) {
		const struct sc7a22_reg_value sequence[] = {
			{ 0x08, data->handup_ctrl },
			{ 0x09, data->layout | 0x70 },
			{ 0x0a, data->angle },
			{ 0x0b, 0xc8 },
		};

		return sc7a22_write_sequence(data, sequence,
					     ARRAY_SIZE(sequence));
	} else {
		const struct sc7a22_reg_value sequence[] = {
			{ 0x3e, 0x10 },
			{ 0x51, data->layout | 0x70 },
			{ 0x52, data->angle },
			{ 0x53, 0xc8 },
		};

		ret = sc7a22_set_bank(data, SC7A22_BANK_90);
		if (ret < 0)
			return ret;
		ret = sc7a22_write_sequence(data, sequence,
					    ARRAY_SIZE(sequence));
		if (ret < 0)
			return ret;
		ret = sc7a22_set_bank(data, SC7A22_BANK_0);
		if (ret < 0)
			return ret;

		return sc7a22_write_reg(data, 0x06, 0x15);
	}
}

/* sc7a22_set_step_mode: configure the in-chip pedometer engine. */
static int sc7a22_set_step_mode(struct sc7a22_data *data)
{
	int ret;

	if (data->chip_id == SC7A22_ID) {
		const struct sc7a22_reg_value sequence[] = {
			{ 0x10, 0x05 },
			{ 0x11, 0x20 },
			{ 0x18, 0xbc },
			{ 0x19, 0x1b },
			{ 0x1a, 0x52 },
		};

		return sc7a22_write_sequence(data, sequence,
					     ARRAY_SIZE(sequence));
	} else {
		const struct sc7a22_reg_value sequence[] = {
			{ 0x3a, 0x05 },
			{ 0x46, 0x4f },
			{ 0x47, 0x33 },
			{ 0x48, 0x9d },
			{ 0x49, 0x24 },
			{ 0x4a, 0x04 },
			{ 0x4b, 0x8c },
		};

		ret = sc7a22_set_bank(data, SC7A22_BANK_90);
		if (ret < 0)
			return ret;
		ret = sc7a22_write_sequence(data, sequence,
					    ARRAY_SIZE(sequence));
		if (ret < 0)
			return ret;

		return sc7a22_set_bank(data, SC7A22_BANK_0);
	}
}

static int sc7a22_read_block(struct sc7a22_data *data, u8 reg,
			     u8 *buffer, int length)
{
	int i;
	int ret;
	u8 base_reg = reg & 0x7f;

	ret = i2c_smbus_read_i2c_block_data(data->client, reg, length, buffer);
	if (ret == length)
		return 0;

	for (i = 0; i < length; i++) {
		ret = sc7a22_read_reg(data, base_reg + i);
		if (ret < 0)
			return ret;
		buffer[i] = ret;
	}

	return 0;
}

static int sc7a22_read_sample(struct sc7a22_data *data, int sample[3])
{
	const struct sc7a22_axis_map *map;
	u8 buffer[6];
	int raw[3];
	int status;
	int ret;
	int i;

	if (data->chip_id == SC7A22_H_ID) {
		ret = sc7a22_write_reg(data, SC7A22_REG_BANK,
				       SC7A22_BANK_0);
		if (ret < 0)
			return ret;
		usleep_range(1000, 2000);
		for (i = 0; i < SC7A22_H_READY_RETRIES; i++) {
			status = sc7a22_read_reg(data, SC7A22_REG_H_STATUS);
			if (status < 0)
				return status;
			if ((status & SC7A22_H_DATA_READY) ==
			    SC7A22_H_DATA_READY)
				break;
			usleep_range(1000, 2000);
		}
		if (i == SC7A22_H_READY_RETRIES)
			return -ETIMEDOUT;
		ret = sc7a22_read_block(data, SC7A22_REG_H_OUT_X_H_CMD,
					buffer, 6);
	} else {
		ret = sc7a22_read_block(data, SC7A22_REG_OUT_X_L_CMD,
					buffer, 6);
	}
	if (ret < 0)
		return ret;

	for (i = 0; i < 3; i++) {
		if (data->chip_id == SC7A22_H_ID)
			raw[i] = (s16)(((u16)buffer[i * 2] << 8) |
					 buffer[i * 2 + 1]);
		else
			raw[i] = (s16)(((u16)buffer[i * 2 + 1] << 8) |
					 buffer[i * 2]);
		/*
		 * 12-bit value, reported undivided as stock does
		 * (read_accel_xyz+0x274): +-2 g high resolution is ~1024
		 * counts per g, and the HAL scales by 9.80665 / 1024.
		 */
		raw[i] >>= 4;
	}

	map = &sc7a22_axis_maps[data->map_layout];
	for (i = 0; i < 3; i++)
		sample[i] = map->sign[i] * raw[map->axis[i]];

	return 0;
}

/* sc7a22_step_value_show: read the chip's cumulative step count. */
static int sc7a22_read_step_hw(struct sc7a22_data *data, u32 *count)
{
	u8 buffer[3] = { };
	int id;
	int ret;

	if (data->chip_id == SC7A22_ID) {
		ret = i2c_smbus_read_i2c_block_data(data->client,
						    SC7A22_REG_STEP_13, 3,
						    buffer);
		if (ret != 3)
			return ret < 0 ? ret : -EIO;
		*count = buffer[0] | (buffer[1] << 8) | (buffer[2] << 16);
		return 0;
	}

	ret = sc7a22_set_bank(data, SC7A22_BANK_90);
	if (ret < 0)
		return ret;
	ret = i2c_smbus_read_i2c_block_data(data->client,
					    SC7A22_REG_STEP_18, 2, buffer);
	if (ret != 2)
		ret = ret < 0 ? ret : -EIO;
	else
		ret = 0;
	sc7a22_set_bank(data, SC7A22_BANK_0);
	id = sc7a22_read_reg(data, SC7A22_REG_H_WHO_AM_I);
	if (ret < 0)
		return ret;

	if (id != SC7A22_H_ID) {
		/*
		 * The chip lost its configuration: keep the running total as
		 * an offset and program it again, as the stock driver does.
		 */
		dev_warn(&data->client->dev, "reset chip\n");
		data->step_offset = data->step_value;
		sc7a22_hw_init_18(data);
		sc7a22_set_hand_up_mode(data);
		sc7a22_set_step_mode(data);
		return -EAGAIN;
	}

	*count = buffer[0] | (buffer[1] << 8);
	return 0;
}

static u32 sc7a22_step_value(struct sc7a22_data *data)
{
	u32 count;
	u32 total;

	mutex_lock(&data->op_lock);
	if (!sc7a22_read_step_hw(data, &count)) {
		total = count + data->step_offset;
		/* The stock driver only keeps values that fit in 16 bits. */
		if (total <= SC7A22_STEP_MAX)
			data->step_value = total;
	}
	total = data->step_value;
	mutex_unlock(&data->op_lock);

	return total;
}

static void sc7a22_step_clear(struct sc7a22_data *data)
{
	int ctrl;

	mutex_lock(&data->op_lock);
	data->step_value = 0;
	data->step_offset = 0;
	if (data->chip_id == SC7A22_ID) {
		ctrl = sc7a22_read_reg(data, SC7A22_REG_STEP_CTRL_13);
		if (ctrl < 0) {
			dev_err(&data->client->dev, "i2c error\n");
		} else {
			sc7a22_write_reg(data, SC7A22_REG_STEP_CTRL_13, 0x04);
			sc7a22_write_reg(data, SC7A22_REG_STEP_CTRL_13, ctrl);
		}
	} else if (data->chip_id == SC7A22_H_ID) {
		if (!sc7a22_set_bank(data, SC7A22_BANK_90)) {
			sc7a22_write_reg(data, SC7A22_REG_STEP_CTRL_18, 0x04);
			usleep_range(1000, 2000);
			sc7a22_write_reg(data, SC7A22_REG_STEP_CTRL_18, 0x05);
			sc7a22_set_bank(data, SC7A22_BANK_0);
		}
	}
	mutex_unlock(&data->op_lock);
}

static int sc7a22_read_flags(struct sc7a22_data *data)
{
	int flags;
	int ret;

	mutex_lock(&data->op_lock);
	if (data->chip_id == SC7A22_ID) {
		flags = sc7a22_read_reg(data, SC7A22_REG_FLAGS_13);
	} else {
		flags = sc7a22_set_bank(data, SC7A22_BANK_90);
		if (flags >= 0)
			flags = sc7a22_read_reg(data, SC7A22_REG_FLAGS_18);
		/* Always try to leave bank 0, whatever the read did. */
		ret = sc7a22_set_bank(data, SC7A22_BANK_0);
		if (flags >= 0 && ret < 0)
			flags = ret;
	}
	mutex_unlock(&data->op_lock);

	return flags;
}

/*
 * The chip keeps INT asserted for as long as its condition holds, so a fixed
 * level trigger re-enters the handler forever. The stock driver waits for the
 * opposite level instead (DM63 stock work function, request in probe):
 * pin high -> trigger low, pin low -> trigger high.
 */
static unsigned long sc7a22_irq_type(int level)
{
	return level ? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH;
}

static void sc7a22_report_flags(struct sc7a22_data *data, int flags)
{
	if ((flags & SC7A22_FLAG_HAND_UP) && data->hand_start &&
	    !data->screen_on) {
		input_event(data->key_input, EV_KEY, KEY_F10, 1);
		input_event(data->key_input, EV_KEY, KEY_F10, 0);
		input_event(data->key_input, EV_SYN, SYN_REPORT, 0);
	}
	/* Flag bit 2 (hand down) is only logged by the stock driver. */
}

/*
 * Sample the pin before the flags, and again after servicing: a pin that moved
 * meanwhile may be a new assertion whose flags were not yet latched when read,
 * so run up to SC7A22_IRQ_RECONCILE extra passes before arming for release.
 * The trigger is always the opposite of the last sampled level, never fixed.
 * Returns the source flags seen, or a negative errno (trigger still armed).
 * Called with irq_lock held.
 */
static int sc7a22_irq_service(struct sc7a22_data *data)
{
	int seen = 0;
	int level;
	int after;
	int flags;
	int pass;

	for (pass = 0;; pass++) {
		level = gpio_get_value_cansleep(data->irq_gpio);
		flags = sc7a22_read_flags(data);
		if (flags < 0) {
			level = gpio_get_value_cansleep(data->irq_gpio);
			seen = flags;
			break;
		}
		dev_dbg(&data->client->dev, "wrist flags 0x%x\n", flags);
		if (flags & (SC7A22_FLAG_HAND_UP | SC7A22_FLAG_HAND_DOWN)) {
			seen |= flags;
			sc7a22_report_flags(data, flags);
		}

		after = gpio_get_value_cansleep(data->irq_gpio);
		if (after == level || pass >= SC7A22_IRQ_RECONCILE) {
			level = after;
			break;
		}
	}
	irq_set_irq_type(data->irq, sc7a22_irq_type(level));

	/* A low pin is the release we waited for, not a fault. */
	if (seen == 0 && level)
		dev_warn_ratelimited(&data->client->dev,
				     "interrupt without a source\n");
	return seen;
}

static irqreturn_t sc7a22_irq_handler(int irq, void *dev_id)
{
	struct sc7a22_data *data = dev_id;

	if (device_may_wakeup(&data->client->dev))
		pm_stay_awake(&data->client->dev);

	return IRQ_WAKE_THREAD;
}

/*
 * The line stays masked (IRQF_ONESHOT) until this returns. With no source
 * bit set the interrupt is not ours: IRQ_NONE lets the spurious interrupt
 * detector disable a line that is stuck. A failed flag read is not spurious:
 * the opposite level is armed (no storm) and the read is retried from
 * flag_work, independent of further pin transitions.
 */
static irqreturn_t sc7a22_irq_thread(int irq, void *dev_id)
{
	struct sc7a22_data *data = dev_id;
	int seen;

	if (device_may_wakeup(&data->client->dev))
		pm_relax(&data->client->dev);

	mutex_lock(&data->irq_lock);
	seen = sc7a22_irq_service(data);
	if (seen < 0) {
		dev_warn_ratelimited(&data->client->dev,
				     "flag read failed: %d\n", seen);
		data->flag_retries = SC7A22_FLAG_RETRIES;
		if (!data->suspended)
			schedule_delayed_work(&data->flag_work,
					      msecs_to_jiffies(SC7A22_FLAG_RETRY_MS));
	} else {
		data->flag_retries = 0;
		cancel_delayed_work(&data->flag_work);
	}
	mutex_unlock(&data->irq_lock);

	return seen == 0 ? IRQ_NONE : IRQ_HANDLED;
}

static void sc7a22_flag_work(struct work_struct *work)
{
	struct sc7a22_data *data = container_of(work, struct sc7a22_data,
						 flag_work.work);
	int seen;

	mutex_lock(&data->irq_lock);
	if (!data->flag_retries)
		goto out;

	seen = sc7a22_irq_service(data);
	if (seen >= 0) {
		data->flag_retries = 0;
	} else if (--data->flag_retries) {
		if (!data->suspended)
			schedule_delayed_work(&data->flag_work,
					      msecs_to_jiffies(SC7A22_FLAG_RETRY_MS));
	} else {
		dev_err(&data->client->dev,
			"flag read still failing: %d\n", seen);
	}
out:
	mutex_unlock(&data->irq_lock);
}

#if IS_ENABLED(CONFIG_DRM_SPRD)
/*
 * Screen state. The stock kernel used the ADF blank notifier (DPMS on / off);
 * the 4.14 equivalent is the sprd DRM panel notifier, sent when the DSI
 * encoder is enabled / disabled. Without the sprd DRM driver the screen is
 * assumed on and the raise-to-wake key is never sent. See
 * devices/SC7A22_INTERFACE.md.
 */
static int sc7a22_panel_notify(struct notifier_block *nb,
			       unsigned long event, void *event_data)
{
	struct sc7a22_data *data = container_of(nb, struct sc7a22_data,
						 panel_nb);

	if (event == SPRD_PANEL_EVENT_ON)
		WRITE_ONCE(data->screen_on, true);
	else if (event == SPRD_PANEL_EVENT_OFF)
		WRITE_ONCE(data->screen_on, false);

	return NOTIFY_DONE;
}
#endif

static void sc7a22_work(struct work_struct *work)
{
	struct sc7a22_data *data = container_of(work, struct sc7a22_data,
						 work.work);
	int sample[3];
	int ret;

	if (!atomic_read(&data->enabled))
		return;

	mutex_lock(&data->op_lock);
	ret = sc7a22_read_sample(data, sample);
	mutex_unlock(&data->op_lock);
	if (!ret) {
		input_report_abs(data->input, ABS_X, sample[0]);
		input_report_abs(data->input, ABS_Y, sample[1]);
		input_report_abs(data->input, ABS_Z, sample[2]);
		/* The stock driver reports accuracy 3 with every sample. */
		input_report_abs(data->input, ABS_THROTTLE, 3);
		input_sync(data->input);
	} else {
		dev_err_ratelimited(&data->client->dev,
				    "failed to read sample: %d\n", ret);
	}

	if (atomic_read(&data->enabled))
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
}

static int sc7a22_set_enabled(struct sc7a22_data *data, bool enabled)
{
	int ret = 0;

	mutex_lock(&data->lock);
	if (enabled && data->removed) {
		ret = -ENODEV;
		goto out;
	}
	if (enabled == !!atomic_read(&data->enabled))
		goto out;

	if (enabled) {
		atomic_set(&data->enabled, 1);
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
	} else {
		atomic_set(&data->enabled, 0);
		cancel_delayed_work_sync(&data->work);
	}

out:
	mutex_unlock(&data->lock);
	return ret;
}

static void sc7a22_set_delay(struct sc7a22_data *data, unsigned int delay_ms)
{
	bool enabled;

	delay_ms = clamp(delay_ms, (unsigned int)SC7A22_MIN_DELAY_MS,
			 (unsigned int)SC7A22_MAX_DELAY_MS);

	mutex_lock(&data->lock);
	enabled = !!atomic_read(&data->enabled);
	if (enabled) {
		atomic_set(&data->enabled, 0);
		cancel_delayed_work_sync(&data->work);
	}
	data->delay_ms = delay_ms;
	if (enabled) {
		atomic_set(&data->enabled, 1);
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
	}
	mutex_unlock(&data->lock);
}

static void sc7a22_free(struct kref *ref)
{
	kfree(container_of(ref, struct sc7a22_data, ref));
}

/*
 * Descriptors can outlive the client (adapter unbind): each one holds a
 * reference, and enabling after remove fails with -ENODEV.
 */
static int sc7a22_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct sc7a22_data *data;

	data = container_of(miscdev, struct sc7a22_data, miscdev);
	/* misc_open() holds misc_mtx: remove has not deregistered us yet. */
	kref_get(&data->ref);
	file->private_data = data;
	return nonseekable_open(inode, file);
}

static int sc7a22_misc_release(struct inode *inode, struct file *file)
{
	struct sc7a22_data *data = file->private_data;

	kref_put(&data->ref, sc7a22_free);
	return 0;
}

static long sc7a22_misc_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct sc7a22_data *data = file->private_data;
	void __user *argp = (void __user *)arg;
	int value;
	int ret = 0;

	switch (cmd) {
	case QMAX981_IOCTL_SET_DELAY:
		if (copy_from_user(&value, argp, sizeof(value)))
			return -EFAULT;
		if (value < 0 || value > SC7A22_MAX_DELAY_MS)
			return -EINVAL;
		sc7a22_set_delay(data, value);
		break;
	case QMAX981_IOCTL_GET_DELAY:
		value = data->delay_ms;
		if (copy_to_user(argp, &value, sizeof(value)))
			return -EFAULT;
		break;
	case QMAX981_IOCTL_SET_ENABLE:
		if (copy_from_user(&value, argp, sizeof(value)))
			return -EFAULT;
		if (value != 0 && value != 1)
			return -EINVAL;
		ret = sc7a22_set_enabled(data, value);
		break;
	case QMAX981_IOCTL_GET_ENABLE:
		value = atomic_read(&data->enabled);
		if (copy_to_user(argp, &value, sizeof(value)))
			return -EFAULT;
		break;
	case QMAX981_IOCTL_CALIBRATION:
		return -EOPNOTSUPP;
	default:
		return -ENOTTY;
	}

	return ret;
}

static const struct file_operations sc7a22_misc_fops = {
	.owner = THIS_MODULE,
	.open = sc7a22_misc_open,
	.release = sc7a22_misc_release,
	.unlocked_ioctl = sc7a22_misc_ioctl,
	.llseek = no_llseek,
};

static ssize_t gsensor_show(struct device *dev,
			    struct device_attribute *attr, char *buffer)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%d\n",
			 atomic_read(&data->enabled));
}

static ssize_t gsensor_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buffer, size_t count)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);
	int ret;

	/* The stock driver enables on "1" and disables on anything else. */
	ret = sc7a22_set_enabled(data, simple_strtoul(buffer, NULL, 10) == 1);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t delay_acc_show(struct device *dev,
			      struct device_attribute *attr, char *buffer)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%u\n", data->delay_ms);
}

static ssize_t delay_acc_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);
	unsigned int delay_ms;
	int ret;

	ret = kstrtouint(buffer, 10, &delay_ms);
	if (ret)
		return ret;
	sc7a22_set_delay(data, delay_ms);

	return count;
}

static ssize_t layout_show(struct device *dev,
			   struct device_attribute *attr, char *buffer)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	/* Like the stock driver: the layout from the DT, not the live map. */
	return sprintf(buffer, "%d\n", data->layout);
}

static ssize_t layout_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buffer, size_t count)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);
	unsigned long layout = simple_strtoul(buffer, NULL, 10);

	if (layout < ARRAY_SIZE(sc7a22_axis_maps))
		data->map_layout = layout;

	return count;
}

static ssize_t chipinfo_show(struct device *dev,
			     struct device_attribute *attr, char *buffer)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);
	const char *info = "unknow!\n";

	if (data->chip_id == SC7A22_ID)
		info = "sc7a22c\n";
	else if (data->chip_id == SC7A22_H_ID)
		info = "sc7a22h\n";

	return sprintf(buffer, "%s", info);
}

static ssize_t hand_start_store(struct device *dev,
				struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);
	unsigned long value = simple_strtoul(buffer, NULL, 10);

	if (value == 1) {
		data->hand_start = true;
		msleep(10);
		device_init_wakeup(&data->client->dev, true);
	} else if (value == 0) {
		device_init_wakeup(&data->client->dev, false);
		data->hand_start = false;
		msleep(10);
	}

	return count;
}

static ssize_t step_value_show(struct device *dev,
			       struct device_attribute *attr, char *buffer)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	return sprintf(buffer, "%u\n", sc7a22_step_value(data));
}

static ssize_t step_clean_store(struct device *dev,
				struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	if ((u8)simple_strtoul(buffer, NULL, 10) == 1)
		sc7a22_step_clear(data);

	return count;
}

static ssize_t motion_state_show(struct device *dev,
				 struct device_attribute *attr, char *buffer)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);
	int state = 0;

	mutex_lock(&data->op_lock);
	if (data->chip_id == SC7A22_ID)
		state = sc7a22_read_reg(data, SC7A22_REG_MOTION_13);
	else if (data->chip_id == SC7A22_H_ID)
		state = sc7a22_read_reg(data, SC7A22_REG_MOTION_18);
	mutex_unlock(&data->op_lock);
	if (state < 0)
		state = 0;

	return sprintf(buffer, "%x\n", state & 0x30);
}

/* The stock nodes are 0644 even when they lack a show or a store method. */
static DEVICE_ATTR(gsensor, 0644, gsensor_show, gsensor_store);
static DEVICE_ATTR(delay_acc, 0644, delay_acc_show, delay_acc_store);
static DEVICE_ATTR(layout, 0644, layout_show, layout_store);
static DEVICE_ATTR(chipinfo, 0644, chipinfo_show, NULL);
static DEVICE_ATTR(hand_start, 0644, NULL, hand_start_store);
static DEVICE_ATTR(step_value, 0644, step_value_show, NULL);
static DEVICE_ATTR(motion_state, 0644, motion_state_show, NULL);
static DEVICE_ATTR(step_clean, 0644, NULL, step_clean_store);

static struct attribute *sc7a22_attributes[] = {
	&dev_attr_gsensor.attr,
	&dev_attr_delay_acc.attr,
	&dev_attr_layout.attr,
	&dev_attr_chipinfo.attr,
	&dev_attr_hand_start.attr,
	&dev_attr_step_value.attr,
	&dev_attr_motion_state.attr,
	&dev_attr_step_clean.attr,
	NULL
};

static const struct attribute_group sc7a22_attribute_group = {
	.attrs = sc7a22_attributes,
};

static void sc7a22_remove_control_files(struct sc7a22_data *data)
{
	if (!data->control_class)
		return;
	if (data->control_dev) {
		device_remove_file(data->control_dev, &dev_attr_delay_acc);
		device_remove_file(data->control_dev, &dev_attr_gsensor);
		device_unregister(data->control_dev);
	}
	class_destroy(data->control_class);
	data->control_dev = NULL;
	data->control_class = NULL;
}

static void sc7a22_create_control_files(struct sc7a22_data *data)
{
	int ret;

	data->control_class = class_create(THIS_MODULE, "xr-gsensor");
	if (IS_ERR(data->control_class)) {
		dev_warn(&data->client->dev, "failed to create control class\n");
		data->control_class = NULL;
		return;
	}

	data->control_dev = device_create(data->control_class, NULL, 0, data,
					  "device");
	if (IS_ERR(data->control_dev)) {
		dev_warn(&data->client->dev, "failed to create control device\n");
		data->control_dev = NULL;
		sc7a22_remove_control_files(data);
		return;
	}

	ret = device_create_file(data->control_dev, &dev_attr_gsensor);
	if (ret)
		dev_warn(&data->client->dev, "failed to create gsensor control\n");
	ret = device_create_file(data->control_dev, &dev_attr_delay_acc);
	if (ret)
		dev_warn(&data->client->dev, "failed to create delay control\n");
}

/*
 * The stock driver registers its input devices without a parent, so they
 * show up under /sys/devices/virtual/input.
 */
static struct input_dev *sc7a22_alloc_input(struct sc7a22_data *data,
					    const char *name, u16 bustype)
{
	struct input_dev *input = input_allocate_device();

	if (!input)
		return NULL;

	input->name = name;
	input->id.bustype = bustype;
	input_set_drvdata(input, data);

	return input;
}

static int sc7a22_register_input(struct input_dev **slot,
				 struct input_dev *input)
{
	int ret = input_register_device(input);

	if (ret) {
		input_free_device(input);
		return ret;
	}
	*slot = input;

	return 0;
}

static int sc7a22_input_init(struct sc7a22_data *data)
{
	struct input_dev *input;

	input = sc7a22_alloc_input(data, SC7A22_INPUT_NAME, BUS_I2C);
	if (!input)
		return -ENOMEM;

	input_set_capability(input, EV_ABS, ABS_X);
	input_set_capability(input, EV_ABS, ABS_Y);
	input_set_capability(input, EV_ABS, ABS_Z);
	input_set_capability(input, EV_ABS, ABS_THROTTLE);
	input_set_abs_params(input, ABS_X, -SC7A22_ABS_MAX,
			     SC7A22_ABS_MAX, 0, 0);
	input_set_abs_params(input, ABS_Y, -SC7A22_ABS_MAX,
			     SC7A22_ABS_MAX, 0, 0);
	input_set_abs_params(input, ABS_Z, -SC7A22_ABS_MAX,
			     SC7A22_ABS_MAX, 0, 0);
	input_set_abs_params(input, ABS_THROTTLE, 0, 3, 0, 0);

	return sc7a22_register_input(&data->input, input);
}

static int sc7a22_input_key_init(struct sc7a22_data *data)
{
	struct input_dev *input;

	input = sc7a22_alloc_input(data, SC7A22_INPUT_KEY_NAME, BUS_HOST);
	if (!input)
		return -ENOMEM;

	__set_bit(EV_ABS, input->evbit);
	__set_bit(EV_KEY, input->evbit);
	__set_bit(KEY_F6, input->keybit);
	__set_bit(KEY_F10, input->keybit);
	__set_bit(KEY_POWER, input->keybit);

	return sc7a22_register_input(&data->key_input, input);
}

/* The stock driver registers this device but never reports on it. */
static int sc7a22_input_step_init(struct sc7a22_data *data)
{
	struct input_dev *input;

	input = sc7a22_alloc_input(data, SC7A22_INPUT_STEP_NAME, BUS_HOST);
	if (!input)
		return -ENOMEM;

	input_set_capability(input, EV_ABS, ABS_X);
	input_set_capability(input, EV_REL, REL_X);
	input_set_abs_params(input, ABS_X, 0, 0xffff, 0, 0);

	return sc7a22_register_input(&data->step_input, input);
}

static void sc7a22_cleanup(struct sc7a22_data *data)
{
	mutex_lock(&data->lock);
	data->removed = true;
	mutex_unlock(&data->lock);

	/* Remove every way to enable polling first: sysfs, ioctl, notifier. */
	sc7a22_remove_control_files(data);
	if (data->sysfs_group)
		sysfs_remove_group(&data->input->dev.kobj,
				   &sc7a22_attribute_group);
	if (data->misc_registered)
		misc_deregister(&data->miscdev);
#if IS_ENABLED(CONFIG_DRM_SPRD)
	if (data->panel_registered)
		sprd_panel_notifier_unregister(&data->panel_nb);
#endif

	/* free_irq() waits for the handler and its thread. */
	if (data->irq_requested)
		free_irq(data->irq, data);
	/* Nothing queues flag_work once the thread is gone. */
	cancel_delayed_work_sync(&data->flag_work);
	if (data->gpio_owned)
		gpio_free(data->irq_gpio);

	sc7a22_set_enabled(data, false);
	device_init_wakeup(&data->client->dev, false);
	if (data->step_input)
		input_unregister_device(data->step_input);
	if (data->key_input)
		input_unregister_device(data->key_input);
	if (data->input)
		input_unregister_device(data->input);
}

static int sc7a22_parse_dt(struct sc7a22_data *data)
{
	struct device_node *np = data->client->dev.of_node;
	u32 value;

	data->angle = SC7A22_DEFAULT_ANGLE;
	data->handup_ctrl = SC7A22_DEFAULT_HANDUP_CTRL;
	data->irq_gpio = -1;
	if (!np)
		return 0;

	if (!of_property_read_u32(np, "layout", &value) &&
	    value < ARRAY_SIZE(sc7a22_axis_maps))
		data->layout = value;
	if (!of_property_read_u32(np, "angle", &value))
		data->angle = value;
	if (!of_property_read_u32(np, "hand-up-ctrl", &value))
		data->handup_ctrl = value;
	data->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	/* optional, but a controller that is not there yet must defer */
	if (data->irq_gpio == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	return 0;
}

static int sc7a22_setup_irq(struct sc7a22_data *data)
{
	unsigned long flags;
	int ret;

	if (!gpio_is_valid(data->irq_gpio))
		return 0;

	ret = gpio_request_one(data->irq_gpio, GPIOF_IN, "sc7a22-hand-up");
	if (ret < 0)
		return ret;
	data->gpio_owned = true;

	data->irq = gpio_to_irq(data->irq_gpio);
	if (data->irq < 0) {
		ret = data->irq;
		goto err_gpio;
	}

	/* Opposite of the pin level, like the stock driver; the thread re-arms. */
	flags = sc7a22_irq_type(gpio_get_value_cansleep(data->irq_gpio));
	ret = request_threaded_irq(data->irq, sc7a22_irq_handler,
				   sc7a22_irq_thread, flags | IRQF_ONESHOT,
				   "sc7a22-hand-up-irq", data);
	if (ret < 0)
		goto err_gpio;
	data->irq_requested = true;

	return 0;

err_gpio:
	gpio_free(data->irq_gpio);
	data->gpio_owned = false;
	return ret;
}

static int sc7a22_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct sc7a22_data *data;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->delay_ms = SC7A22_DEFAULT_DELAY_MS;
	mutex_init(&data->lock);
	kref_init(&data->ref);
	mutex_init(&data->op_lock);
	mutex_init(&data->irq_lock);
	atomic_set(&data->enabled, 0);
	INIT_DELAYED_WORK(&data->work, sc7a22_work);
#if IS_ENABLED(CONFIG_DRM_SPRD)
	data->panel_nb.notifier_call = sc7a22_panel_notify;
	data->panel_nb.priority = 1000;
#endif
	INIT_DELAYED_WORK(&data->flag_work, sc7a22_flag_work);
	/* Same initial state as the stock driver: "screen on", no hand_start. */
	data->screen_on = true;
	i2c_set_clientdata(client, data);

	ret = sc7a22_parse_dt(data);
	if (ret < 0)
		goto err_free;
	data->map_layout = data->layout;

	ret = sc7a22_detect(data);
	if (ret < 0)
		goto err_free;

	ret = sc7a22_hw_init(data);
	if (ret < 0)
		goto err_free;
	ret = sc7a22_input_init(data);
	if (ret < 0)
		goto err_free;

	ret = sc7a22_set_hand_up_mode(data);
	if (ret < 0)
		goto err_cleanup;
	ret = sc7a22_input_key_init(data);
	if (ret < 0)
		goto err_cleanup;
	/* The stock driver arms the interrupt as a wake source at probe. */
	device_init_wakeup(&client->dev, true);
	ret = sc7a22_set_step_mode(data);
	if (ret < 0)
		goto err_cleanup;
	ret = sc7a22_input_step_init(data);
	if (ret < 0)
		goto err_cleanup;

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = SC7A22_MISC_NAME;
	data->miscdev.fops = &sc7a22_misc_fops;
	data->miscdev.parent = &client->dev;
	ret = misc_register(&data->miscdev);
	if (ret < 0)
		goto err_cleanup;
	data->misc_registered = true;

#if IS_ENABLED(CONFIG_DRM_SPRD)
	/* the callback gets the current screen state during registration */
	if (!sprd_panel_notifier_register(&data->panel_nb))
		data->panel_registered = true;
#endif

	ret = sysfs_create_group(&data->input->dev.kobj,
				 &sc7a22_attribute_group);
	if (ret < 0)
		goto err_cleanup;
	data->sysfs_group = true;

	sc7a22_create_control_files(data);

	ret = sc7a22_setup_irq(data);
	if (ret == -EPROBE_DEFER)
		goto err_cleanup;
	if (ret < 0)
		dev_warn(&client->dev, "no hand-up interrupt: %d\n", ret);

	dev_info(&client->dev, "SC7A2x detected, id=0x%02x layout=%u\n",
		 data->chip_id, data->layout);
	return 0;

err_cleanup:
	sc7a22_cleanup(data);
err_free:
	i2c_set_clientdata(client, NULL);
	kref_put(&data->ref, sc7a22_free);
	return ret;
}

static int sc7a22_remove(struct i2c_client *client)
{
	struct sc7a22_data *data = i2c_get_clientdata(client);

	sc7a22_cleanup(data);
	i2c_set_clientdata(client, NULL);
	kref_put(&data->ref, sc7a22_free);
	return 0;
}

static void sc7a22_shutdown(struct i2c_client *client)
{
	struct sc7a22_data *data = i2c_get_clientdata(client);

	if (data)
		sc7a22_set_enabled(data, false);
}

#ifdef CONFIG_PM_SLEEP
static int sc7a22_suspend(struct device *dev)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	/* The IRQ thread stays live: keep it from queuing a retry. */
	mutex_lock(&data->irq_lock);
	data->suspended = true;
	mutex_unlock(&data->irq_lock);
	cancel_delayed_work_sync(&data->flag_work);
	if (data->irq_requested && device_may_wakeup(dev))
		irq_set_irq_wake(data->irq, 1);
	data->resume_enabled = !!atomic_read(&data->enabled);
	if (data->resume_enabled)
		return sc7a22_set_enabled(data, false);
	return 0;
}

static int sc7a22_resume(struct device *dev)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

	if (data->irq_requested && device_may_wakeup(dev))
		irq_set_irq_wake(data->irq, 0);
	/* Resume a retry that suspend cancelled. */
	mutex_lock(&data->irq_lock);
	data->suspended = false;
	if (data->flag_retries)
		schedule_delayed_work(&data->flag_work,
				      msecs_to_jiffies(SC7A22_FLAG_RETRY_MS));
	mutex_unlock(&data->irq_lock);
	if (data->resume_enabled)
		return sc7a22_set_enabled(data, true);
	return 0;
}

static SIMPLE_DEV_PM_OPS(sc7a22_pm_ops, sc7a22_suspend, sc7a22_resume);
#define SC7A22_PM_OPS (&sc7a22_pm_ops)
#else
#define SC7A22_PM_OPS NULL
#endif

static const struct i2c_device_id sc7a22_ids[] = {
	{ SC7A22_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sc7a22_ids);

static const struct of_device_id sc7a22_of_match[] = {
	{ .compatible = "sprd,sc7a22" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7a22_of_match);

static struct i2c_driver sc7a22_driver = {
	.driver = {
		.name = SC7A22_DRV_NAME,
		.of_match_table = sc7a22_of_match,
		.pm = SC7A22_PM_OPS,
	},
	.probe = sc7a22_probe,
	.remove = sc7a22_remove,
	.shutdown = sc7a22_shutdown,
	.id_table = sc7a22_ids,
};

module_i2c_driver(sc7a22_driver);

MODULE_AUTHOR("LineageOS device maintainers");
MODULE_DESCRIPTION("SC7A22 accelerometer with legacy qmax981 HAL ABI");
MODULE_LICENSE("GPL v2");
