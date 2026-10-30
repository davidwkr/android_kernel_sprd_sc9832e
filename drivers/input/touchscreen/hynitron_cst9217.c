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
 * Device tree (compatible "hynitron,hyn_ts"):
 *   hynitron,reset-gpio    : reset gpio (optional)
 *   hynitron,irq-gpio      : interrupt gpio (else the I2C client irq)
 *   hynitron,display-coords: <max-x max-y>
 *   vdd_name               : supply regulator, default "vddsdcore"
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
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>

#define HYN_NAME		"hyn_ts"

#define HYN_REG_BASE		0xd000
#define HYN_CMD_NORMAL		0xd109
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

struct hyn_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *vdd;
	int reset_gpio;
	int irq;
	u32 max_x;
	u32 max_y;
	struct mutex lock;	/* protects suspended, pending and the report */
	bool suspended;
	bool pending;		/* a report arrived while suspended */
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
		return;
	}

	/* key frames (bit 7) are not used */
	if (buf[6] != HYN_SYNC_BYTE || buf[0] == HYN_SYNC_BYTE ||
	    (buf[5] & 0x80))
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
		unsigned int x = (p[1] << 4) | (p[3] >> 4);
		unsigned int y = (p[2] << 4) | (p[3] & 0x0f);

		if (id >= HYN_MAX_POINTS || test_bit(id, &seen) ||
		    (event && event != (HYN_STAT_DOWN >> 1)) ||
		    x > ts->max_x || y > ts->max_y)
			goto ack;

		__set_bit(id, &seen);
	}

	if (hyn_write(ts, HYN_REG_BASE, &sync))
		return;

	for (i = 0; i < count; i++) {
		const u8 *p = hyn_point(buf, i);

		if (((p[0] & 0x0f) >> 1) != (HYN_STAT_DOWN >> 1))
			continue;

		input_mt_slot(input, p[0] >> 4);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X,
				 (p[1] << 4) | (p[3] >> 4));
		input_report_abs(input, ABS_MT_POSITION_Y,
				 (p[2] << 4) | (p[3] & 0x0f));
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
	} else {
		hyn_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

static int hyn_suspend(struct device *dev)
{
	struct hyn_ts *ts = dev_get_drvdata(dev);

	/* waits for a handler that is reading from the controller */
	mutex_lock(&ts->lock);
	ts->suspended = true;
	mutex_unlock(&ts->lock);

	if (device_may_wakeup(dev))
		enable_irq_wake(ts->irq);

	return 0;
}

static int hyn_resume(struct device *dev)
{
	struct hyn_ts *ts = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(ts->irq);

	/* the I2C adapter, our parent, is already resumed */
	mutex_lock(&ts->lock);
	ts->suspended = false;
	if (ts->pending) {
		ts->pending = false;
		hyn_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	return 0;
}

static SIMPLE_DEV_PM_OPS(hyn_pm_ops, hyn_suspend, hyn_resume);

static void hyn_disable_regulator(void *data)
{
	struct hyn_ts *ts = data;

	regulator_disable(ts->vdd);
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

	ret = hyn_write(ts, HYN_CMD_NORMAL, NULL);
	if (ret) {
		dev_err(dev, "failed to start the controller: %d\n", ret);
		return -ENODEV;
	}
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
	if (ret)
		dev_err(dev, "failed to request irq %d: %d\n", ts->irq, ret);

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
