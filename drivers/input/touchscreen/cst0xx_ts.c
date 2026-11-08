/*
 * Hynitron CST0xx touchscreen driver
 *
 * Driver for the Hynitron CST0xx family of capacitive touch controllers
 * (CST816 style register map) as used on UNISOC SL8541E smartwatches.
 *
 * The controller raises a falling edge on its interrupt line when a new
 * report is available. A report is read with a single I2C transfer from
 * register 0 and holds up to five points.
 *
 * The screen state comes from the ADF blank notifier, as in the stock
 * driver. When the screen blanks (DRM_MODE_DPMS_OFF) the controller gets
 * the stock sleep command (register 0xe5 = 0x03) and its interrupt is
 * disabled: the controller is not a wake source then. When the screen
 * unblanks (DRM_MODE_DPMS_ON) the controller is reset, as in the stock
 * driver, the interrupt is enabled again and every finger is released.
 *
 * The stock driver has no double-tap-to-wake support that can be
 * reproduced: it never writes a gesture enable command and never arms the
 * interrupt as a wake source. The "wake_gesture" attribute of the input
 * device exists for ABI compatibility with the other touch drivers: it reads
 * 0 and writing 1 fails with -EOPNOTSUPP. See devices/TOUCH_WAKE.md.
 *
 * While the system is suspended the I2C bus is not usable, so a report
 * that arrives then is only noted and read on resume, and a blank state
 * change is applied on resume.
 *
 * The register layout, the sleep command and the reset timing were derived
 * from the stock kernel binary for interoperability.
 *
 * Device tree (compatible "cst0xx,cst0xx_ts"):
 *   gpios    : <reset-gpio>, <irq-gpio>, in this order
 *   vdd_name : name of the supply regulator
 *   TP_MAX_X, TP_MAX_Y : size of the reported coordinate range
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
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <video/adf_notifier.h>

#define CST0XX_NAME		"cst0xx_ts"

/* Report layout, read in one go from register 0 */
#define CST0XX_REG_REPORT	0x00
#define CST0XX_REG_GESTURE	1
#define CST0XX_REG_COUNT	2
#define CST0XX_REG_POINTS	3

#define CST0XX_GESTURE_MARK	0xaa
#define CST0XX_MAX_POINTS	5
#define CST0XX_POINT_LEN	6
#define CST0XX_REPORT_LEN	(CST0XX_REG_POINTS + \
				 CST0XX_MAX_POINTS * CST0XX_POINT_LEN)

/* Point layout, relative to the start of the point */
#define CST0XX_PT_XH		0	/* event flag [7:6], X [11:8] */
#define CST0XX_PT_XL		1
#define CST0XX_PT_YH		2	/* finger id [7:4], Y [11:8] */
#define CST0XX_PT_YL		3
#define CST0XX_PT_PRESSURE	4
#define CST0XX_PT_AREA		5	/* area [7:4] */

#define CST0XX_EVENT_DOWN	0
#define CST0XX_EVENT_CONTACT	2

/* The finger id is a nibble, use it directly as the MT slot */
#define CST0XX_NUM_SLOTS	16
#define CST0XX_MAX_COORD	0xfff
#define CST0XX_MAX_PRESSURE	127
#define CST0XX_MAX_AREA		15
#define CST0XX_DEFAULT_AREA	9

#define CST0XX_SUPPLY_MIN_UV	2800000
#define CST0XX_SUPPLY_MAX_UV	3000000

/* Sleep command: two byte write, register 0xe5 = 0x03 (deep sleep) */
#define CST0XX_REG_SLEEP	0xe5
#define CST0XX_SLEEP_DEEP	0x03

/* the stock driver registers its ADF client with this priority */
#define CST0XX_ADF_PRIO		1000

struct cst0xx_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *vdd;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	u32 max_x;
	u32 max_y;
	int irq;
	struct notifier_block adf_nb;
	struct mutex lock;	/* protects the state below and the report */
	bool suspended;		/* system sleep: the I2C bus is down */
	bool pending;		/* a report arrived while suspended */
	bool blanked;		/* the screen is off, as last notified */
	bool asleep;		/* the controller got the sleep command */
	bool irq_disabled;	/* the line is masked: disable_irq() pending */
	u8 report[CST0XX_REPORT_LEN] ____cacheline_aligned;
};

static int cst0xx_read(struct cst0xx_ts *ts, u8 reg, u8 *buf, u16 len)
{
	struct i2c_msg msgs[2] = {
		{
			.addr = ts->client->addr,
			.len = 1,
			.buf = &reg,
		}, {
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret;

	ret = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int cst0xx_write(struct cst0xx_ts *ts, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = ts->client->addr,
		.len = sizeof(buf),
		.buf = buf,
	};
	int ret;

	ret = i2c_transfer(ts->client->adapter, &msg, 1);
	if (ret < 0)
		return ret;

	return ret == 1 ? 0 : -EIO;
}

/* The reset line is held high (1) while the controller runs. */
static void cst0xx_hw_reset(struct cst0xx_ts *ts)
{
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(50);
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	/* the firmware needs about 200 ms to start reporting */
	msleep(200);
}

/* Called with ts->lock held. */
static void cst0xx_process_report(struct cst0xx_ts *ts)
{
	struct input_dev *input = ts->input;
	const u8 *report = ts->report;
	unsigned long seen = 0;
	unsigned int count, i;
	u32 x[CST0XX_MAX_POINTS], y[CST0XX_MAX_POINTS];
	u8 id[CST0XX_MAX_POINTS];
	int ret;

	/* the irq was masked without waiting for this handler */
	if (ts->asleep)
		return;

	ret = cst0xx_read(ts, CST0XX_REG_REPORT, ts->report,
			  sizeof(ts->report));
	if (ret) {
		dev_err_ratelimited(&ts->client->dev,
				    "failed to read report: %d\n", ret);
		return;
	}

	/* a gesture report has no point data */
	if (report[CST0XX_REG_GESTURE] == CST0XX_GESTURE_MARK)
		return;

	count = report[CST0XX_REG_COUNT] & 0x07;
	if (count > CST0XX_MAX_POINTS)
		return;

	/* validate the whole frame before reporting any slot */
	for (i = 0; i < count; i++) {
		const u8 *p = report + CST0XX_REG_POINTS +
			      i * CST0XX_POINT_LEN;

		id[i] = p[CST0XX_PT_YH] >> 4;
		x[i] = ((p[CST0XX_PT_XH] & 0x0f) << 8) | p[CST0XX_PT_XL];
		y[i] = ((p[CST0XX_PT_YH] & 0x0f) << 8) | p[CST0XX_PT_YL];

		if (id[i] >= CST0XX_NUM_SLOTS || test_bit(id[i], &seen))
			return;

		__set_bit(id[i], &seen);
	}

	for (i = 0; i < count; i++) {
		const u8 *p = report + CST0XX_REG_POINTS +
			      i * CST0XX_POINT_LEN;
		unsigned int event = p[CST0XX_PT_XH] >> 6;
		unsigned int pressure = p[CST0XX_PT_PRESSURE];
		unsigned int area = p[CST0XX_PT_AREA] >> 4;

		/* lifted fingers are dropped by input_mt_sync_frame() */
		if (event != CST0XX_EVENT_DOWN && event != CST0XX_EVENT_CONTACT)
			continue;

		if (!pressure || pressure > CST0XX_MAX_PRESSURE)
			pressure = CST0XX_MAX_PRESSURE;
		if (!area)
			area = CST0XX_DEFAULT_AREA;

		input_mt_slot(input, id[i]);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X,
				 min_t(u32, x[i], ts->max_x));
		input_report_abs(input, ABS_MT_POSITION_Y,
				 min_t(u32, y[i], ts->max_y));
		input_report_abs(input, ABS_MT_PRESSURE, pressure);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, area);
	}

	input_mt_sync_frame(input);
	input_sync(input);
}

static irqreturn_t cst0xx_irq_thread(int irq, void *dev_id)
{
	struct cst0xx_ts *ts = dev_id;

	mutex_lock(&ts->lock);
	if (ts->suspended) {
		/* the I2C bus is down: read the report on resume */
		ts->pending = true;
	} else {
		cst0xx_process_report(ts);
	}
	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

/*
 * Bring the controller in line with ts->blanked. Called with ts->lock held
 * and the I2C bus usable. The irq is masked without waiting: its handler
 * takes ts->lock and checks ts->asleep.
 */
static void cst0xx_sync_state(struct cst0xx_ts *ts)
{
	int ret;

	if (ts->blanked && !ts->asleep) {
		ret = cst0xx_write(ts, CST0XX_REG_SLEEP, CST0XX_SLEEP_DEEP);
		if (ret)
			dev_err(&ts->client->dev,
				"failed to send the sleep command: %d\n", ret);

		ts->asleep = true;
		ts->pending = false;
		if (!ts->irq_disabled) {
			disable_irq_nosync(ts->irq);
			ts->irq_disabled = true;
		}
	} else if (!ts->blanked && ts->asleep) {
		cst0xx_hw_reset(ts);
		ts->asleep = false;
		ts->pending = false;
		if (ts->irq_disabled) {
			enable_irq(ts->irq);
			ts->irq_disabled = false;
		}

		/* drop every finger: nothing is marked as used */
		input_mt_sync_frame(ts->input);
		input_sync(ts->input);
	}
}

static int cst0xx_adf_notify(struct notifier_block *nb, unsigned long event,
			     void *event_data)
{
	struct cst0xx_ts *ts = container_of(nb, struct cst0xx_ts, adf_nb);
	struct adf_notifier_event *ev = event_data;
	int state;

	if (!ev || !ev->data || event != ADF_EVENT_BLANK)
		return NOTIFY_DONE;

	/* other states (standby, suspend) are ignored, like the stock driver */
	state = *(int *)ev->data;
	if (state != DRM_MODE_DPMS_ON && state != DRM_MODE_DPMS_OFF)
		return NOTIFY_DONE;

	mutex_lock(&ts->lock);
	ts->blanked = state == DRM_MODE_DPMS_OFF;
	/* with the bus down the state is applied on resume */
	if (!ts->suspended)
		cst0xx_sync_state(ts);
	mutex_unlock(&ts->lock);

	return NOTIFY_OK;
}

static int cst0xx_suspend(struct device *dev)
{
	struct cst0xx_ts *ts = dev_get_drvdata(dev);

	/* waits for a handler or a notifier that is using the bus */
	mutex_lock(&ts->lock);
	ts->suspended = true;
	mutex_unlock(&ts->lock);

	return 0;
}

static int cst0xx_resume(struct device *dev)
{
	struct cst0xx_ts *ts = dev_get_drvdata(dev);

	/* all resume_noirq callbacks, including the adapter's, have run */
	mutex_lock(&ts->lock);
	ts->suspended = false;
	if (ts->pending) {
		ts->pending = false;
		cst0xx_process_report(ts);
	}
	cst0xx_sync_state(ts);
	mutex_unlock(&ts->lock);

	return 0;
}

static const struct dev_pm_ops cst0xx_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(cst0xx_suspend, cst0xx_resume)
};

static void cst0xx_disable_regulator(void *data)
{
	struct cst0xx_ts *ts = data;

	regulator_disable(ts->vdd);
}

static void cst0xx_unregister_adf(void *data)
{
	struct cst0xx_ts *ts = data;

	adf_unregister_client(&ts->adf_nb);
}

/* No gesture mode is known for this controller: always 0. */
static ssize_t wake_gesture_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t wake_gesture_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	u8 val;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	/* writing 0 keeps the only supported state */
	if (val)
		return -EOPNOTSUPP;

	return count;
}
static DEVICE_ATTR_RW(wake_gesture);

static struct attribute *cst0xx_attrs[] = {
	&dev_attr_wake_gesture.attr,
	NULL
};

static const struct attribute_group cst0xx_attr_group = {
	.attrs = cst0xx_attrs,
};

static void cst0xx_remove_group(void *data)
{
	struct cst0xx_ts *ts = data;

	sysfs_remove_group(&ts->input->dev.kobj, &cst0xx_attr_group);
}

static int cst0xx_power_on(struct cst0xx_ts *ts)
{
	struct device *dev = &ts->client->dev;
	const char *vdd_name;
	int ret;

	ret = of_property_read_string(dev->of_node, "vdd_name", &vdd_name);
	if (ret) {
		dev_err(dev, "missing vdd_name\n");
		return ret;
	}

	ts->vdd = devm_regulator_get(dev, vdd_name);
	if (IS_ERR(ts->vdd)) {
		ret = PTR_ERR(ts->vdd);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get regulator %s: %d\n",
				vdd_name, ret);
		return ret;
	}

	/* the supply may be fixed, so a failure here is not fatal */
	ret = regulator_set_voltage(ts->vdd, CST0XX_SUPPLY_MIN_UV,
				    CST0XX_SUPPLY_MAX_UV);
	if (ret)
		dev_warn(dev, "failed to set supply voltage: %d\n", ret);

	ret = regulator_enable(ts->vdd);
	if (ret) {
		dev_err(dev, "failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = devm_add_action(dev, cst0xx_disable_regulator, ts);
	if (ret)
		regulator_disable(ts->vdd);

	return ret;
}

static int cst0xx_parse_dt(struct cst0xx_ts *ts)
{
	struct device *dev = &ts->client->dev;
	struct device_node *np = dev->of_node;

	if (of_property_read_u32(np, "TP_MAX_X", &ts->max_x) ||
	    of_property_read_u32(np, "TP_MAX_Y", &ts->max_y)) {
		dev_err(dev, "missing TP_MAX_X or TP_MAX_Y\n");
		return -EINVAL;
	}

	if (!ts->max_x || ts->max_x > CST0XX_MAX_COORD ||
	    !ts->max_y || ts->max_y > CST0XX_MAX_COORD) {
		dev_err(dev, "invalid panel size %ux%u\n",
			ts->max_x, ts->max_y);
		return -EINVAL;
	}

	ts->reset_gpio = devm_gpiod_get_index(dev, NULL, 0, GPIOD_OUT_LOW);
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

static int cst0xx_register_input(struct cst0xx_ts *ts)
{
	struct device *dev = &ts->client->dev;
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = CST0XX_NAME;
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, ts);

	/* the wake key of the common ABI, see wake_gesture */
	input_set_capability(input, EV_KEY, KEY_WAKEUP);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0,
			     CST0XX_MAX_PRESSURE, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0,
			     CST0XX_MAX_AREA, 0, 0);

	ret = input_mt_init_slots(input, CST0XX_NUM_SLOTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	ts->input = input;
	return 0;
}

static int cst0xx_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct cst0xx_ts *ts;
	u8 buf[CST0XX_REG_POINTS];
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

	ret = cst0xx_parse_dt(ts);
	if (ret)
		return ret;

	ret = cst0xx_power_on(ts);
	if (ret)
		return ret;

	cst0xx_hw_reset(ts);

	/* only check that the controller answers */
	ret = cst0xx_read(ts, CST0XX_REG_REPORT, buf, sizeof(buf));
	if (ret) {
		dev_err(dev, "controller does not respond: %d\n", ret);
		return ret;
	}

	ret = cst0xx_register_input(ts);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		return ret;
	}

	ret = devm_request_threaded_irq(dev, ts->irq, NULL, cst0xx_irq_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					CST0XX_NAME, ts);
	if (ret) {
		dev_err(dev, "failed to request irq %d: %d\n", ts->irq, ret);
		return ret;
	}

	ret = sysfs_create_group(&ts->input->dev.kobj, &cst0xx_attr_group);
	if (ret) {
		dev_err(dev, "failed to create sysfs group: %d\n", ret);
		return ret;
	}

	ret = devm_add_action(dev, cst0xx_remove_group, ts);
	if (ret) {
		cst0xx_remove_group(ts);
		return ret;
	}

	ts->adf_nb.notifier_call = cst0xx_adf_notify;
	ts->adf_nb.priority = CST0XX_ADF_PRIO;
	ret = adf_register_client(&ts->adf_nb);
	if (ret) {
		dev_err(dev, "failed to register ADF client: %d\n", ret);
		return ret;
	}

	ret = devm_add_action(dev, cst0xx_unregister_adf, ts);
	if (ret)
		cst0xx_unregister_adf(ts);

	return ret;
}

static const struct of_device_id cst0xx_of_match[] = {
	{ .compatible = "cst0xx,cst0xx_ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, cst0xx_of_match);

static const struct i2c_device_id cst0xx_id[] = {
	{ CST0XX_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cst0xx_id);

static struct i2c_driver cst0xx_driver = {
	.driver = {
		.name = CST0XX_NAME,
		.of_match_table = cst0xx_of_match,
		.pm = &cst0xx_pm_ops,
	},
	.probe = cst0xx_probe,
	.id_table = cst0xx_id,
};
module_i2c_driver(cst0xx_driver);

MODULE_DESCRIPTION("Hynitron CST0xx touchscreen driver");
MODULE_LICENSE("GPL v2");
