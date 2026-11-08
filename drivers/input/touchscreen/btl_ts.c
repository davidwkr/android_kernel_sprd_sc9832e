/*
 * Betterlife (BTL) BL6133/BL6138 touchscreen driver
 *
 * Driver for the Betterlife capacitive touch controllers as used on UNISOC
 * SC9832E smartwatches (DT node "btl,btl_ts", I2C address 0x2d). The input
 * device is named "Betterlife_ts" like in the stock kernel.
 *
 * The controller shares its reset and interrupt GPIOs with other touch
 * candidates (cst0xx_ts, hyn_ts). The chip identity is read in probe; when
 * it is not a Betterlife chip the probe fails and the devm managed GPIOs and
 * supply are released for the next candidate.
 *
 * The controller raises a falling edge on its interrupt line when a report
 * is available. A report is 8 bytes read from register 0x01. The stock
 * driver reports a single contact (protocol A); here it is reported through
 * slot 0 of protocol B.
 *
 * Screen state comes from the ADF blank notifier, like in the stock driver.
 * Screen on: normal touch. Screen off: the controller is put in its sleep
 * mode (register 0xa5 = 3), the interrupt is disabled and it is not a wake
 * source. Screen on again: dummy read, reset pulse, interrupt enabled. The
 * stock driver has no gesture mode that could wake the device (no enable
 * register or command, no switch), so the "wake_gesture" attribute of the
 * input device only accepts 0.
 *
 * While the screen is on a report that arrives while the I2C bus is down is
 * read on resume, and touch wakes the system from suspend. A screen state
 * change that arrives while the bus is down is applied on resume.
 *
 * Firmware checking and flashing of the controller is deliberately not
 * implemented.
 *
 * The register layout and the timing were derived from the stock kernel
 * binary for interoperability, see devices/BTL_TS_INTERFACE.md.
 *
 * Device tree (compatible "btl,btl_ts"):
 *   gpios    : <reset-gpio>, <irq-gpio>, in this order
 *   vdd_name : name of the supply regulator, default "vddsdcore"
 *   TP_MAX_X, TP_MAX_Y : size of the reported coordinate range
 *   (virtualkeys is ignored)
 *
 * Copyright (C) Urbetrack
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <video/adf_notifier.h>

#define BTL_NAME		"Betterlife_ts"
#define BTL_I2C_NAME		"btl_ts"

/* Report: register pointer 0x01, 8 bytes read back */
#define BTL_REG_REPORT		0x01
#define BTL_REPORT_LEN		8
#define BTL_R_GESTURE		0
#define BTL_R_COUNT		1
#define BTL_R_XH		2	/* event flag [7:6], X [11:8] */
#define BTL_R_XL		3
#define BTL_R_YH		4	/* Y [11:8] */
#define BTL_R_YL		5

#define BTL_GESTURE_KEY		0x17	/* reported as KEY_F4 */

/* Mode register: written with a single value, 3 = sleep (stock blank path) */
#define BTL_REG_MODE		0xa5
#define BTL_MODE_SLEEP		0x03

#define BTL_EVENT_DOWN		0
#define BTL_EVENT_UP		1
#define BTL_EVENT_CONTACT	2

#define BTL_MAX_COORD		0xfff

/* Chip identity: flash-mode I2C addresses and the ID command */
#define BTL_FLASH_ADDR		0x2c
#define BTL_CHIP_BL6133		0x22
#define BTL_CHIP_BL6138		0x23

#define BTL_SUPPLY_NAME		"vddsdcore"
#define BTL_SUPPLY_UV		2800000
#define BTL_WAKEUP_MS		200

struct btl_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *vdd;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	u32 max_x;
	u32 max_y;
	int irq;
	struct notifier_block adf_nb;
	/* protects the flags below, the I2C traffic and the report */
	struct mutex lock;
	bool suspended;		/* the I2C bus is down */
	bool pending;		/* a report arrived while suspended */
	bool screen_off;	/* last state from the ADF notifier */
	bool asleep;		/* controller in sleep mode, irq disabled */
	bool wake_armed;	/* enable_irq_wake() is in effect */
	u8 report[BTL_REPORT_LEN] ____cacheline_aligned;
};

/* Raw transfer to an arbitrary address (the flash-mode addresses). */
static int btl_raw_xfer(struct btl_ts *ts, u16 addr, u16 flags, u8 *buf,
			u16 len)
{
	struct i2c_msg msg = {
		.addr = addr,
		.flags = flags,
		.len = len,
		.buf = buf,
	};
	int ret;

	ret = i2c_transfer(ts->client->adapter, &msg, 1);
	if (ret < 0)
		return ret;

	return ret == 1 ? 0 : -EIO;
}

static int btl_write_reg(struct btl_ts *ts, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };

	return btl_raw_xfer(ts, ts->client->addr, 0, buf, sizeof(buf));
}

static int btl_read_report(struct btl_ts *ts)
{
	u8 reg = BTL_REG_REPORT;
	struct i2c_msg msgs[2] = {
		{
			.addr = ts->client->addr,
			.len = 1,
			.buf = &reg,
		}, {
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = BTL_REPORT_LEN,
			.buf = ts->report,
		},
	};
	int ret;

	ret = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/* Reset pulse: high, low, high, 50 ms apart. The line is high while running. */
static void btl_reset_pulse(struct btl_ts *ts)
{
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	msleep(50);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(50);
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
}

/*
 * Reset with the interrupt line driven to @irq_level (low enters the
 * flash mode used for the ID read, high returns to the application).
 */
static void btl_reset_irq_level(struct btl_ts *ts, int irq_level)
{
	gpiod_direction_output_raw(ts->irq_gpio, irq_level);
	msleep(50);
	btl_reset_pulse(ts);
}

static int btl_read_chip_id(struct btl_ts *ts, u8 *chip)
{
	u8 sw[4] = { 0x0a, 0xf5, 0xe7, 0xe8 };
	u8 cmd[3] = { 0x0a, 0xf5, 0xe7 };
	u8 id = 0;
	int ret;

	btl_reset_irq_level(ts, 0);
	msleep(50);
	/* soft reset, switch the interrupt line to wake mode */
	btl_raw_xfer(ts, ts->client->addr, 0, sw, sizeof(sw));
	msleep(50);

	ret = btl_raw_xfer(ts, BTL_FLASH_ADDR, 0, cmd, sizeof(cmd));
	if (!ret)
		ret = btl_raw_xfer(ts, BTL_FLASH_ADDR, I2C_M_RD, &id, 1);

	/* back to the application, interrupt line high */
	btl_reset_irq_level(ts, 1);
	msleep(20);

	if (ret)
		return ret;

	*chip = id;
	return 0;
}

/* Called with ts->lock held and the controller awake. */
static void btl_process_report(struct btl_ts *ts)
{
	struct input_dev *input = ts->input;
	const u8 *r = ts->report;
	unsigned int event;
	int ret;

	ret = btl_read_report(ts);
	if (ret) {
		dev_err_ratelimited(&ts->client->dev,
				    "failed to read report: %d\n", ret);
		return;
	}

	if (r[BTL_R_GESTURE] == BTL_GESTURE_KEY) {
		input_report_key(input, KEY_F4, 1);
		input_sync(input);
		input_report_key(input, KEY_F4, 0);
		input_sync(input);
	}

	event = r[BTL_R_XH] >> 6;

	if (event == BTL_EVENT_UP) {
		if (r[BTL_R_COUNT] <= 1) {
			input_mt_slot(input, 0);
			input_mt_report_slot_state(input, MT_TOOL_FINGER,
						   false);
		}
	} else if (!(event & BTL_EVENT_UP) && r[BTL_R_COUNT] == 1) {
		u32 x = ((r[BTL_R_XH] & 0x0f) << 8) | r[BTL_R_XL];
		u32 y = ((r[BTL_R_YH] & 0x0f) << 8) | r[BTL_R_YL];

		input_mt_slot(input, 0);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X,
				 min_t(u32, x, ts->max_x));
		input_report_abs(input, ABS_MT_POSITION_Y,
				 min_t(u32, y, ts->max_y));
	}

	input_mt_sync_frame(input);
	input_sync(input);
}

static irqreturn_t btl_irq_thread(int irq, void *dev_id)
{
	struct btl_ts *ts = dev_id;

	mutex_lock(&ts->lock);
	if (ts->asleep) {
		/* a report that was queued before the sleep command */
	} else if (ts->suspended) {
		/* the I2C bus is down: read the report on resume */
		ts->pending = true;
		pm_wakeup_event(&ts->client->dev, BTL_WAKEUP_MS);
	} else {
		btl_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

/* Release every finger, like the stock unblank path does. */
static void btl_release_fingers(struct btl_ts *ts)
{
	input_mt_slot(ts->input, 0);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

/* Called with ts->lock held and the I2C bus up. */
static void btl_enter_sleep(struct btl_ts *ts)
{
	int ret;

	/*
	 * Not disable_irq(): the handler takes ts->lock. A handler that is
	 * already running sees ts->asleep and returns.
	 */
	disable_irq_nosync(ts->irq);
	if (ts->wake_armed) {
		disable_irq_wake(ts->irq);
		ts->wake_armed = false;
	}
	ts->pending = false;
	ts->asleep = true;

	ret = btl_write_reg(ts, BTL_REG_MODE, BTL_MODE_SLEEP);
	if (ret)
		dev_warn(&ts->client->dev, "failed to sleep: %d\n", ret);
}

/* Called with ts->lock held and the I2C bus up. */
static void btl_leave_sleep(struct btl_ts *ts)
{
	u8 reg = BTL_REG_MODE;
	u8 val;
	struct i2c_msg msgs[2] = {
		{ .addr = ts->client->addr, .len = 1, .buf = &reg },
		{ .addr = ts->client->addr, .flags = I2C_M_RD, .len = 1,
		  .buf = &val },
	};

	/* dummy read of the mode register, the result is not used */
	i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));
	btl_reset_pulse(ts);

	ts->asleep = false;
	enable_irq(ts->irq);
	btl_release_fingers(ts);
}

/* Called with ts->lock held and the I2C bus up. */
static void btl_apply_screen(struct btl_ts *ts)
{
	if (ts->screen_off && !ts->asleep)
		btl_enter_sleep(ts);
	else if (!ts->screen_off && ts->asleep)
		btl_leave_sleep(ts);
}

static int btl_adf_notify(struct notifier_block *nb, unsigned long event,
			  void *event_data)
{
	struct btl_ts *ts = container_of(nb, struct btl_ts, adf_nb);
	struct adf_notifier_event *ev = event_data;
	int state;

	if (!ev || !ev->data || event != ADF_EVENT_BLANK)
		return 0;

	state = *(int *)ev->data;
	if (state != DRM_MODE_DPMS_ON && state != DRM_MODE_DPMS_OFF)
		return 0;

	mutex_lock(&ts->lock);
	ts->screen_off = state == DRM_MODE_DPMS_OFF;
	/* with the bus down the state is applied on resume */
	if (!ts->suspended)
		btl_apply_screen(ts);
	mutex_unlock(&ts->lock);

	return 0;
}

static ssize_t wake_gesture_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}

/* The stock driver has no double-tap gesture mode: only 0 is accepted. */
static ssize_t wake_gesture_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;
	if (val)
		return -EOPNOTSUPP;

	return count;
}
static DEVICE_ATTR_RW(wake_gesture);

static struct attribute *btl_attrs[] = {
	&dev_attr_wake_gesture.attr,
	NULL
};

static const struct attribute_group btl_attr_group = {
	.attrs = btl_attrs,
};

static const struct attribute_group *btl_attr_groups[] = {
	&btl_attr_group,
	NULL
};

static int btl_suspend(struct device *dev)
{
	struct btl_ts *ts = dev_get_drvdata(dev);

	/* waits for a handler that is reading from the controller */
	mutex_lock(&ts->lock);
	ts->suspended = true;
	/* with the controller asleep touch is not a wake source */
	if (!ts->asleep && device_may_wakeup(dev) &&
	    !enable_irq_wake(ts->irq))
		ts->wake_armed = true;
	mutex_unlock(&ts->lock);

	return 0;
}

static int btl_resume(struct device *dev)
{
	struct btl_ts *ts = dev_get_drvdata(dev);

	mutex_lock(&ts->lock);
	if (ts->wake_armed) {
		disable_irq_wake(ts->irq);
		ts->wake_armed = false;
	}
	ts->suspended = false;
	/* a screen state change that arrived with the bus down */
	btl_apply_screen(ts);
	if (ts->pending) {
		ts->pending = false;
		if (!ts->asleep)
			btl_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	return 0;
}

static const struct dev_pm_ops btl_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(btl_suspend, btl_resume)
};

static void btl_disable_regulator(void *data)
{
	struct btl_ts *ts = data;

	regulator_disable(ts->vdd);
}

static void btl_unregister_adf(void *data)
{
	struct btl_ts *ts = data;

	adf_unregister_client(&ts->adf_nb);
}

static void btl_disable_wakeup(void *data)
{
	struct btl_ts *ts = data;

	device_init_wakeup(&ts->client->dev, false);
}

static int btl_power_on(struct btl_ts *ts)
{
	struct device *dev = &ts->client->dev;
	const char *vdd_name = BTL_SUPPLY_NAME;
	int ret;

	of_property_read_string(dev->of_node, "vdd_name", &vdd_name);

	ts->vdd = devm_regulator_get(dev, vdd_name);
	if (IS_ERR(ts->vdd)) {
		ret = PTR_ERR(ts->vdd);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get regulator %s: %d\n",
				vdd_name, ret);
		return ret;
	}

	/* the supply may be fixed, so a failure here is not fatal */
	ret = regulator_set_voltage(ts->vdd, BTL_SUPPLY_UV, BTL_SUPPLY_UV);
	if (ret)
		dev_warn(dev, "failed to set supply voltage: %d\n", ret);

	ret = regulator_enable(ts->vdd);
	if (ret) {
		dev_err(dev, "failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = devm_add_action(dev, btl_disable_regulator, ts);
	if (ret)
		regulator_disable(ts->vdd);

	return ret;
}

static void btl_irq_gpio_input(void *data)
{
	struct btl_ts *ts = data;

	/* the ID read drives the line; leave it as an input for the next user */
	gpiod_direction_input(ts->irq_gpio);
}

static int btl_parse_dt(struct btl_ts *ts)
{
	struct device *dev = &ts->client->dev;
	struct device_node *np = dev->of_node;

	if (of_property_read_u32(np, "TP_MAX_X", &ts->max_x) ||
	    of_property_read_u32(np, "TP_MAX_Y", &ts->max_y)) {
		dev_err(dev, "missing TP_MAX_X or TP_MAX_Y\n");
		return -EINVAL;
	}

	if (!ts->max_x || ts->max_x > BTL_MAX_COORD ||
	    !ts->max_y || ts->max_y > BTL_MAX_COORD) {
		dev_err(dev, "invalid panel size %ux%u\n",
			ts->max_x, ts->max_y);
		return -EINVAL;
	}

	ts->reset_gpio = devm_gpiod_get_index(dev, NULL, 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio)) {
		dev_err(dev, "failed to get reset gpio: %ld\n",
			PTR_ERR(ts->reset_gpio));
		return PTR_ERR(ts->reset_gpio);
	}

	ts->irq_gpio = devm_gpiod_get_index(dev, NULL, 1, GPIOD_IN);
	if (IS_ERR(ts->irq_gpio)) {
		dev_err(dev, "failed to get irq gpio: %ld\n",
			PTR_ERR(ts->irq_gpio));
		return PTR_ERR(ts->irq_gpio);
	}

	ts->irq = gpiod_to_irq(ts->irq_gpio);
	if (ts->irq < 0) {
		dev_err(dev, "failed to map irq gpio: %d\n", ts->irq);
		return ts->irq;
	}

	return 0;
}

static int btl_register_input(struct btl_ts *ts)
{
	struct device *dev = &ts->client->dev;
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = BTL_NAME;
	input->id.bustype = BUS_I2C;
	input->id.version = 1;
	input->dev.groups = btl_attr_groups;
	input_set_drvdata(input, ts);

	input_set_capability(input, EV_KEY, KEY_F4);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);

	ret = input_mt_init_slots(input, 1,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	ts->input = input;
	return 0;
}

static int btl_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct btl_ts *ts;
	u8 chip = 0;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "plain I2C transfers are not supported\n");
		return -ENODEV;
	}

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	mutex_init(&ts->lock);
	i2c_set_clientdata(client, ts);

	ret = btl_parse_dt(ts);
	if (ret)
		return ret;

	ret = btl_power_on(ts);
	if (ret)
		return ret;

	/* supply settle time of the stock driver: 100 ms + 200 ms */
	msleep(300);

	/* runs on failure too, before the GPIOs are released */
	ret = devm_add_action(dev, btl_irq_gpio_input, ts);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	msleep(100);
	btl_reset_pulse(ts);
	msleep(50);

	ret = btl_read_chip_id(ts, &chip);
	if (ret || (chip != BTL_CHIP_BL6133 && chip != BTL_CHIP_BL6138)) {
		dev_dbg(dev, "no Betterlife chip (id 0x%02x, err %d)\n",
			chip, ret);
		return -ENODEV;
	}
	dev_info(dev, "chip is %s\n",
		 chip == BTL_CHIP_BL6133 ? "bl6133" : "bl6138");

	/* the interrupt line is an input again for the application */
	gpiod_direction_input(ts->irq_gpio);

	ret = btl_register_input(ts);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		return ret;
	}

	device_init_wakeup(dev, true);
	ret = devm_add_action(dev, btl_disable_wakeup, ts);
	if (ret) {
		btl_disable_wakeup(ts);
		return ret;
	}

	ret = devm_request_threaded_irq(dev, ts->irq, NULL, btl_irq_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT |
					IRQF_NO_SUSPEND, BTL_NAME, ts);
	if (ret) {
		dev_err(dev, "failed to request irq %d: %d\n", ts->irq, ret);
		return ret;
	}

	/* the screen is on at probe, like the stock driver assumes */
	ts->adf_nb.notifier_call = btl_adf_notify;
	ts->adf_nb.priority = 1000;
	if (adf_register_client(&ts->adf_nb)) {
		dev_warn(dev, "failed to register the display notifier\n");
		return 0;
	}
	/* runs before the irq is freed: no callback with a dead handler */
	ret = devm_add_action(dev, btl_unregister_adf, ts);
	if (ret) {
		btl_unregister_adf(ts);
		return ret;
	}

	return 0;
}

static const struct of_device_id btl_of_match[] = {
	{ .compatible = "btl,btl_ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, btl_of_match);

static const struct i2c_device_id btl_id[] = {
	{ BTL_I2C_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, btl_id);

static struct i2c_driver btl_driver = {
	.driver = {
		.name = BTL_I2C_NAME,
		.of_match_table = btl_of_match,
		.pm = &btl_pm_ops,
	},
	.probe = btl_probe,
	.id_table = btl_id,
};
module_i2c_driver(btl_driver);

MODULE_DESCRIPTION("Betterlife BL6133/BL6138 touchscreen driver");
MODULE_LICENSE("GPL v2");
