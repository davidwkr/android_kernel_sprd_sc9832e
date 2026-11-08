/*
 * Hynitron CST9217 touchscreen driver
 *
 * Driver for the Hynitron CST9217 touch controller (16 bit register
 * addresses) as used on the DW100U smartwatch. The input device is named
 * "hyn_ts" like in the stock kernel.
 *
 * The controller raises a rising edge on its interrupt line when a new
 * report is available. A report is read from register 0xd000 and
 * acknowledged by writing 0xab back to it. The controller stays powered
 * during suspend; a report that arrives while the I2C bus is down is read
 * on resume.
 *
 * Screen state comes from the ADF blank notifier. With the screen on the
 * controller reports touches. With the screen off it is put to sleep with
 * the stock sleep command (0xd105) and a hardware reset plus the stock
 * normal command (0xd109) wakes it. The stock driver has no separate
 * gesture command: it keeps the interrupt armed after the sleep command and
 * turns a report with 0xf1 in byte 4 into a KEY_WAKEUP press. The same is
 * done here, controlled by the "wake_gesture" attribute (0 or 1, default 1)
 * of the input device:
 *   1: the interrupt stays enabled and is a wake source while the screen
 *      is off, and a gesture report wakes the device;
 *   0: the interrupt is disabled and not a wake source while the screen
 *      is off.
 *
 * Device tree (compatible "hynitron,hyn_ts"):
 *   hynitron,reset-gpio    : reset gpio (optional)
 *   hynitron,irq-gpio      : interrupt gpio (else the I2C client irq)
 *   hynitron,display-coords: <max-x max-y>
 *   vdd_name               : supply regulator, default "vddsdcore"
 *
 * Based on the previous in-tree hynitron_cst9217 driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <uapi/drm/drm_mode.h>
#include <video/adf_notifier.h>

#define HYN_NAME		"hyn_ts"

#define HYN_REG_BASE		0xd000
#define HYN_CMD_NORMAL		0xd109
#define HYN_CMD_SLEEP		0xd105
#define HYN_GESTURE_ID		0xf1	/* report byte 4 */
#define HYN_SYNC_BYTE		0xab
#define HYN_STAT_DOWN		0x06

#define HYN_MAX_POINTS		5
#define HYN_FIRST_LEN		7
#define HYN_POINT_LEN		5
#define HYN_READ_BUF_LEN	(HYN_FIRST_LEN + \
				 (HYN_MAX_POINTS - 1) * HYN_POINT_LEN + 1)

#define HYN_SUPPLY_MIN_UV	2800000
#define HYN_SUPPLY_MAX_UV	3000000
#define HYN_WAKEUP_MS		200
#define HYN_GESTURE_WAKEUP_MS	1000
#define HYN_RESET_LOW_MS	21
#define HYN_RESET_HIGH_MS	40

struct hyn_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *vdd;
	int reset_gpio;
	int irq;
	u32 max_x;
	u32 max_y;
	struct notifier_block adf_nb;
	struct mutex state_lock; /* serializes screen and switch changes */
	struct mutex lock;	/* protects the fields below and the report */
	bool suspended;
	bool pending;		/* a report arrived while suspended */
	bool screen_off;	/* last ADF blank state */
	bool asleep;		/* the controller got the sleep command */
	bool wake_gesture;	/* the attribute */
	bool suspend_wake;	/* irq wake armed by system suspend */
	bool irq_off;		/* state_lock: irq disabled for the screen */
	bool irq_wake;		/* state_lock: irq wake armed for the screen */
	u8 buf[HYN_READ_BUF_LEN] ____cacheline_aligned;
};

static int hyn_read(struct hyn_ts *ts, u16 reg, u8 *data, u16 len)
{
	u8 reg_buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{
			.addr = ts->client->addr,
			.len = sizeof(reg_buf),
			.buf = reg_buf,
		}, {
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = data,
		},
	};
	int ret;

	ret = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/* Write a register address followed by one data byte, or none. */
static int hyn_write(struct hyn_ts *ts, u16 reg, const u8 *data)
{
	u8 buf[3] = { reg >> 8, reg & 0xff };
	int len = 2;
	int ret;

	if (data)
		buf[len++] = *data;

	ret = i2c_master_send(ts->client, buf, len);
	if (ret < 0)
		return ret;

	return ret == len ? 0 : -EIO;
}

static void hyn_hw_reset(struct hyn_ts *ts)
{
	if (!gpio_is_valid(ts->reset_gpio))
		return;

	gpio_set_value_cansleep(ts->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpio_set_value_cansleep(ts->reset_gpio, 1);
	msleep(50);
}

/* The reset of the stock resume: low for 21 ms, high, wait 40 ms. */
static void hyn_wake_reset(struct hyn_ts *ts)
{
	if (!gpio_is_valid(ts->reset_gpio))
		return;

	gpio_set_value_cansleep(ts->reset_gpio, 0);
	msleep(HYN_RESET_LOW_MS);
	gpio_set_value_cansleep(ts->reset_gpio, 1);
	msleep(HYN_RESET_HIGH_MS);
}

static const u8 *hyn_point(const u8 *buf, unsigned int i)
{
	return i ? buf + HYN_FIRST_LEN + (i - 1) * HYN_POINT_LEN : buf;
}

/* Called with ts->lock held and the I2C bus usable. */
static void hyn_process_report(struct hyn_ts *ts)
{
	static const u8 sync = HYN_SYNC_BYTE;
	struct input_dev *input = ts->input;
	const u8 *buf = ts->buf;
	unsigned long seen = 0;
	unsigned int count, i;
	int ret;

	/*
	 * The first read has one point plus the count and a trailer; more
	 * points follow at 0xd007 and end with a second trailer.
	 */
	ret = hyn_read(ts, HYN_REG_BASE, ts->buf, HYN_FIRST_LEN);
	if (ret) {
		dev_err_ratelimited(&ts->client->dev,
				    "failed to read report: %d\n", ret);
		goto ack;
	}

	if (buf[6] != HYN_SYNC_BYTE || buf[0] == HYN_SYNC_BYTE)
		goto ack;

	/* asleep: only the gesture report matters */
	if (ts->asleep) {
		if (buf[4] == HYN_GESTURE_ID && ts->wake_gesture) {
			pm_wakeup_event(&ts->client->dev,
					HYN_GESTURE_WAKEUP_MS);
			input_report_key(input, KEY_WAKEUP, 1);
			input_sync(input);
			input_report_key(input, KEY_WAKEUP, 0);
			input_sync(input);
		}
		goto ack;
	}

	/* key frames (bit 7) are not used */
	if (buf[5] & 0x80)
		goto ack;

	count = buf[5] & 0x7f;
	if (count > HYN_MAX_POINTS)
		goto ack;

	if (count > 1) {
		ret = hyn_read(ts, HYN_REG_BASE + HYN_FIRST_LEN,
			       &ts->buf[HYN_FIRST_LEN],
			       (count - 1) * HYN_POINT_LEN + 1);
		if (ret || *hyn_point(buf, count) != HYN_SYNC_BYTE)
			goto ack;
	}

	/* reject the whole frame before reporting any slot */
	for (i = 0; i < count; i++) {
		const u8 *p = hyn_point(buf, i);
		unsigned int id = p[0] >> 4;
		unsigned int event = (p[0] & 0x0f) >> 1;

		if (id >= HYN_MAX_POINTS || test_bit(id, &seen) ||
		    (event && event != (HYN_STAT_DOWN >> 1)))
			goto ack;

		__set_bit(id, &seen);
	}

	if (hyn_write(ts, HYN_REG_BASE, &sync))
		return;

	for (i = 0; i < count; i++) {
		const u8 *p = hyn_point(buf, i);
		unsigned int x = (p[1] << 4) | (p[3] >> 4);
		unsigned int y = (p[2] << 4) | (p[3] & 0x0f);

		if (((p[0] & 0x0f) >> 1) != (HYN_STAT_DOWN >> 1))
			continue;

		input_mt_slot(input, p[0] >> 4);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X,
				 min_t(unsigned int, x, ts->max_x));
		input_report_abs(input, ABS_MT_POSITION_Y,
				 min_t(unsigned int, y, ts->max_y));
	}

	/* slots that were not reported are released here */
	input_mt_sync_frame(input);
	input_sync(input);
	return;

ack:
	hyn_write(ts, HYN_REG_BASE, &sync);
}

static irqreturn_t hyn_irq_thread(int irq, void *dev_id)
{
	struct hyn_ts *ts = dev_id;

	mutex_lock(&ts->lock);
	if (ts->suspended) {
		/* the I2C bus is down: read the report on resume */
		ts->pending = true;
		pm_wakeup_event(&ts->client->dev, HYN_WAKEUP_MS);
	} else if (!ts->asleep || ts->wake_gesture) {
		hyn_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

/* Called with ts->lock held and the I2C bus usable. */
static void hyn_release_fingers(struct hyn_ts *ts)
{
	/* nothing is reported in this frame: every slot is released */
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

/* The stock driver retries the command once. */
static int hyn_send_cmd(struct hyn_ts *ts, u16 cmd)
{
	int ret;

	ret = hyn_write(ts, cmd, NULL);
	if (ret)
		ret = hyn_write(ts, cmd, NULL);

	return ret;
}

/*
 * Bring the controller and the interrupt in line with ts->screen_off and
 * ts->wake_gesture. Called with ts->state_lock held and ts->lock not held.
 * Nothing is done while the I2C bus is suspended: hyn_resume() calls this
 * again.
 */
static void hyn_apply_state(struct hyn_ts *ts)
{
	struct device *dev = &ts->client->dev;
	bool off, gesture;
	int ret;

	mutex_lock(&ts->lock);
	if (ts->suspended) {
		mutex_unlock(&ts->lock);
		return;
	}

	off = ts->screen_off;
	gesture = off && ts->wake_gesture;

	if (off && !ts->asleep) {
		hyn_release_fingers(ts);
		ret = hyn_send_cmd(ts, HYN_CMD_SLEEP);
		if (ret)
			dev_warn(dev, "sleep command failed: %d\n", ret);
		ts->asleep = true;
	} else if (!off && ts->asleep) {
		/* the irq is disabled or harmless here, see hyn_irq_thread */
		hyn_wake_reset(ts);
		ret = hyn_send_cmd(ts, HYN_CMD_NORMAL);
		if (ret)
			dev_warn(dev, "wake command failed: %d\n", ret);
		hyn_release_fingers(ts);
		ts->asleep = false;
	}
	mutex_unlock(&ts->lock);

	/*
	 * disable_irq() waits for the handler, which takes ts->lock: it must
	 * not be called with the lock held.
	 */
	if (gesture) {
		if (!ts->irq_wake && !enable_irq_wake(ts->irq))
			ts->irq_wake = true;
		if (ts->irq_off) {
			enable_irq(ts->irq);
			ts->irq_off = false;
		}
	} else if (off) {
		if (ts->irq_wake) {
			disable_irq_wake(ts->irq);
			ts->irq_wake = false;
		}
		if (!ts->irq_off) {
			disable_irq(ts->irq);
			ts->irq_off = true;
		}
	} else {
		if (ts->irq_wake) {
			disable_irq_wake(ts->irq);
			ts->irq_wake = false;
		}
		if (ts->irq_off) {
			enable_irq(ts->irq);
			ts->irq_off = false;
		}
	}
}

static int hyn_adf_notify(struct notifier_block *nb, unsigned long event,
			  void *event_data)
{
	struct hyn_ts *ts = container_of(nb, struct hyn_ts, adf_nb);
	struct adf_notifier_event *ev = event_data;
	bool off;

	if (!ev || !ev->data || event != ADF_EVENT_BLANK)
		return NOTIFY_DONE;

	switch (*(int *)ev->data) {
	case DRM_MODE_DPMS_ON:
		off = false;
		break;
	case DRM_MODE_DPMS_OFF:
		off = true;
		break;
	default:
		return NOTIFY_DONE;
	}

	mutex_lock(&ts->state_lock);
	mutex_lock(&ts->lock);
	ts->screen_off = off;
	mutex_unlock(&ts->lock);
	hyn_apply_state(ts);
	mutex_unlock(&ts->state_lock);

	return NOTIFY_OK;
}

static ssize_t wake_gesture_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hyn_ts *ts = dev_get_drvdata(dev);
	bool val;

	mutex_lock(&ts->lock);
	val = ts->wake_gesture;
	mutex_unlock(&ts->lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t wake_gesture_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct hyn_ts *ts = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	mutex_lock(&ts->state_lock);
	mutex_lock(&ts->lock);
	ts->wake_gesture = val;
	mutex_unlock(&ts->lock);
	hyn_apply_state(ts);
	mutex_unlock(&ts->state_lock);

	return count;
}
static DEVICE_ATTR_RW(wake_gesture);

static struct attribute *hyn_attrs[] = {
	&dev_attr_wake_gesture.attr,
	NULL
};

static const struct attribute_group hyn_attr_group = {
	.attrs = hyn_attrs,
};

static int hyn_suspend(struct device *dev)
{
	struct hyn_ts *ts = dev_get_drvdata(dev);

	/* waits for a handler that is reading from the controller */
	mutex_lock(&ts->lock);
	ts->suspended = true;
	/*
	 * Screen off: hyn_apply_state() has set up the wake source. Screen
	 * on: a touch wakes the system, as before.
	 */
	if (!ts->asleep && device_may_wakeup(dev) &&
	    !enable_irq_wake(ts->irq))
		ts->suspend_wake = true;
	mutex_unlock(&ts->lock);

	return 0;
}

static int hyn_resume(struct device *dev)
{
	struct hyn_ts *ts = dev_get_drvdata(dev);

	/* all resume_noirq callbacks, including the adapter's, have run */
	mutex_lock(&ts->lock);
	if (ts->suspend_wake) {
		disable_irq_wake(ts->irq);
		ts->suspend_wake = false;
	}
	ts->suspended = false;
	if (ts->pending) {
		ts->pending = false;
		hyn_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	/* a screen change that came in while suspended */
	mutex_lock(&ts->state_lock);
	hyn_apply_state(ts);
	mutex_unlock(&ts->state_lock);

	return 0;
}

static const struct dev_pm_ops hyn_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(hyn_suspend, hyn_resume)
};

static void hyn_disable_regulator(void *data)
{
	struct hyn_ts *ts = data;

	regulator_disable(ts->vdd);
}

static void hyn_remove_attrs(void *data)
{
	struct hyn_ts *ts = data;

	sysfs_remove_group(&ts->input->dev.kobj, &hyn_attr_group);
}

static void hyn_unregister_adf(void *data)
{
	struct hyn_ts *ts = data;

	adf_unregister_client(&ts->adf_nb);
}

/*
 * Runs after the notifier and the attributes are gone (devm order) and
 * before the irq is freed: drop any wake reference still held on the line.
 */
static void hyn_release_irq_wake(void *data)
{
	struct hyn_ts *ts = data;

	/* lock order: state_lock, then lock (suspend_wake is under lock) */
	mutex_lock(&ts->state_lock);
	if (ts->irq_wake) {
		disable_irq_wake(ts->irq);
		ts->irq_wake = false;
	}
	mutex_lock(&ts->lock);
	if (ts->suspend_wake) {
		disable_irq_wake(ts->irq);
		ts->suspend_wake = false;
	}
	mutex_unlock(&ts->lock);
	mutex_unlock(&ts->state_lock);
}

static void hyn_disable_wakeup(void *data)
{
	struct hyn_ts *ts = data;

	device_init_wakeup(&ts->client->dev, false);
}

static int hyn_power_on(struct hyn_ts *ts)
{
	struct device *dev = &ts->client->dev;
	const char *vdd_name = "vddsdcore";
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
	ret = regulator_set_voltage(ts->vdd, HYN_SUPPLY_MIN_UV,
				    HYN_SUPPLY_MAX_UV);
	if (ret)
		dev_warn(dev, "failed to set supply voltage: %d\n", ret);

	ret = regulator_enable(ts->vdd);
	if (ret) {
		dev_err(dev, "failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = devm_add_action(dev, hyn_disable_regulator, ts);
	if (ret)
		regulator_disable(ts->vdd);

	return ret;
}

static int hyn_parse_dt(struct hyn_ts *ts)
{
	struct device *dev = &ts->client->dev;
	struct device_node *np = dev->of_node;
	u32 coords[2];
	int irq_gpio;
	int ret;

	ret = of_property_read_u32_array(np, "hynitron,display-coords",
					 coords, ARRAY_SIZE(coords));
	if (ret || !coords[0] || !coords[1]) {
		dev_err(dev, "missing or invalid hynitron,display-coords\n");
		return -EINVAL;
	}
	ts->max_x = coords[0];
	ts->max_y = coords[1];

	ts->reset_gpio = of_get_named_gpio(np, "hynitron,reset-gpio", 0);
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = devm_gpio_request_one(dev, ts->reset_gpio,
					    GPIOF_OUT_INIT_LOW, "hyn_rst");
		if (ret) {
			dev_err(dev, "failed to get reset gpio: %d\n", ret);
			return ret;
		}
	} else if (ts->reset_gpio == -EPROBE_DEFER) {
		return ts->reset_gpio;
	}

	irq_gpio = of_get_named_gpio(np, "hynitron,irq-gpio", 0);
	if (irq_gpio == -EPROBE_DEFER)
		return irq_gpio;

	if (gpio_is_valid(irq_gpio)) {
		ret = devm_gpio_request_one(dev, irq_gpio, GPIOF_IN,
					    "hyn_irq");
		if (ret) {
			dev_err(dev, "failed to get irq gpio: %d\n", ret);
			return ret;
		}
		ts->irq = gpio_to_irq(irq_gpio);
	} else {
		ts->irq = ts->client->irq;
	}

	if (ts->irq <= 0) {
		dev_err(dev, "no usable interrupt: %d\n", ts->irq);
		return ts->irq ? ts->irq : -EINVAL;
	}

	return 0;
}

static int hyn_register_input(struct hyn_ts *ts)
{
	struct device *dev = &ts->client->dev;
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = HYN_NAME;
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, ts);
	input_set_capability(input, EV_KEY, KEY_WAKEUP);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);

	ret = input_mt_init_slots(input, HYN_MAX_POINTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	ts->input = input;
	return 0;
}

static int hyn_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct hyn_ts *ts;
	u8 dummy[4];
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
	mutex_init(&ts->state_lock);
	ts->wake_gesture = true;
	ts->adf_nb.notifier_call = hyn_adf_notify;
	ts->adf_nb.priority = 1000;
	i2c_set_clientdata(client, ts);

	ret = hyn_parse_dt(ts);
	if (ret)
		return ret;

	ret = hyn_power_on(ts);
	if (ret)
		return ret;

	msleep(20);
	hyn_hw_reset(ts);

	/* give up on this node if no controller answers */
	if (hyn_read(ts, HYN_REG_BASE, dummy, sizeof(dummy))) {
		hyn_hw_reset(ts);
		msleep(100);
		if (hyn_read(ts, HYN_REG_BASE, dummy, sizeof(dummy))) {
			dev_err(dev, "controller does not respond\n");
			return -ENODEV;
		}
	}

	/*
	 * The controller answered the read above, so a NACK of the start
	 * command is not fatal: it is not known whether a working unit
	 * acknowledges it.
	 */
	ret = hyn_write(ts, HYN_CMD_NORMAL, NULL);
	if (ret)
		dev_warn(dev, "start command failed: %d\n", ret);
	msleep(20);

	ret = hyn_register_input(ts);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		return ret;
	}

	device_init_wakeup(dev, true);
	ret = devm_add_action(dev, hyn_disable_wakeup, ts);
	if (ret) {
		hyn_disable_wakeup(ts);
		return ret;
	}

	ret = devm_request_threaded_irq(dev, ts->irq, NULL, hyn_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT |
					IRQF_NO_SUSPEND, HYN_NAME, ts);
	if (ret) {
		dev_err(dev, "failed to request irq %d: %d\n", ts->irq, ret);
		return ret;
	}
	ret = devm_add_action(dev, hyn_release_irq_wake, ts);
	if (ret)
		return ret;

	/* the attribute and the notifier use the interrupt: after the request */
	ret = sysfs_create_group(&ts->input->dev.kobj, &hyn_attr_group);
	if (ret) {
		dev_err(dev, "failed to create sysfs group: %d\n", ret);
		return ret;
	}
	ret = devm_add_action(dev, hyn_remove_attrs, ts);
	if (ret) {
		hyn_remove_attrs(ts);
		return ret;
	}

	ret = adf_register_client(&ts->adf_nb);
	if (ret) {
		/* touch keeps working, the screen state is not followed */
		dev_warn(dev, "failed to register ADF client: %d\n", ret);
		return 0;
	}
	ret = devm_add_action(dev, hyn_unregister_adf, ts);
	if (ret)
		hyn_unregister_adf(ts);

	return ret;
}

static const struct of_device_id hyn_of_match[] = {
	{ .compatible = "hynitron,hyn_ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, hyn_of_match);

static const struct i2c_device_id hyn_id[] = {
	{ HYN_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, hyn_id);

static struct i2c_driver hyn_driver = {
	.driver = {
		.name = HYN_NAME,
		.of_match_table = hyn_of_match,
		.pm = &hyn_pm_ops,
	},
	.probe = hyn_probe,
	.id_table = hyn_id,
};
module_i2c_driver(hyn_driver);

MODULE_DESCRIPTION("Hynitron CST9217 touchscreen driver");
MODULE_LICENSE("GPL v2");
