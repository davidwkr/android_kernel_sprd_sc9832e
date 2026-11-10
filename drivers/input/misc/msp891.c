// SPDX-License-Identifier: GPL-2.0
/*
 * MSP891 I2C pressure + temperature sensor.
 *
 * Behaviour follows the stock vendor driver (see devices/MSP891_INTERFACE.md
 * for the evidence). The chip is pure software polling, no IRQ/GPIO of any
 * kind. Two input devices are registered, "pressure" and
 * "ambient_temperature", both EV_REL/REL_MISC only, matching stock exactly.
 *
 * sysfs (identical pair of attributes on each of the two input devices,
 * backing one shared state, as stock): enable, delay.
 *
 * Deviations from the documented stock driver (see devices/MSP891_INTERFACE.md,
 * "In-tree driver", for the full list): a failed chip id check returns
 * -ENODEV instead of leaking the probe as a false success; an I2C error while
 * reading a sample skips the report instead of emitting the error code as a
 * reading; the temperature raw value is sign-extended before the shift; the
 * delayed work re-arms with schedule_delayed_work() instead of a literal,
 * implausible CPU pin; the work is cancelled before teardown in remove().
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define MSP891_DRV_NAME			"msp891"
#define MSP891_INPUT_PRES_NAME		"pressure"
#define MSP891_INPUT_TEMP_NAME		"ambient_temperature"

#define MSP891_REG_CHIP_ID0		0x00
#define MSP891_REG_CHIP_ID1		0x01
#define MSP891_REG_PRESSURE		0x17	/* 3 bytes, burst read, LE */
#define MSP891_REG_TEMPERATURE		0x1a	/* 2 bytes, burst read, LE */

#define MSP891_CHIP_ID0_BIT		0x20
#define MSP891_CHIP_ID1_VAL		0x03

#define MSP891_DEFAULT_DELAY_MS		15
#define MSP891_MIN_DELAY_MS		1
#define MSP891_MAX_DELAY_MS		199	/* stock: (value - 1) <= 0xc6 */

struct msp891_data {
	struct i2c_client *client;
	struct input_dev *pres_input;
	struct input_dev *temp_input;
	struct delayed_work work;
	struct mutex lock;		/* enable/delay transitions */
	bool enabled;
	bool resume_enabled;
	unsigned int delay_ms;
};

struct msp891_reg_value {
	u8 reg;
	u8 value;
};

static int msp891_write(struct i2c_client *client, u8 reg, u8 value)
{
	u8 buf[2] = { reg, value };
	struct i2c_msg msg = {
		.addr = client->addr, .flags = 0, .len = sizeof(buf), .buf = buf,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

static int msp891_read(struct i2c_client *client, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		return ret;
	return ret == 2 ? 0 : -EIO;
}

static int msp891_write_sequence(struct i2c_client *client,
				 const struct msp891_reg_value *sequence,
				 size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = msp891_write(client, sequence[i].reg, sequence[i].value);
		if (ret)
			return ret;
	}

	return 0;
}

/* read-only identity check, safe on a chip that may not be ours */
static int msp891_check_id(struct i2c_client *client)
{
	u8 chip_id0, chip_id1;
	int ret;

	ret = msp891_read(client, MSP891_REG_CHIP_ID0, &chip_id0, 1);
	if (ret)
		return ret;
	ret = msp891_read(client, MSP891_REG_CHIP_ID1, &chip_id1, 1);
	if (ret)
		return ret;

	dev_info(&client->dev, "chip_id0: 0x%x chip_id1: 0x%x\n",
		 chip_id0, chip_id1);

	/*
	 * Stock: on a mismatch here, the probe frees its state and returns 0
	 * (success) instead of an error, leaving a dangling global pointer.
	 * Reproduced only as a documented hazard, not here: fail the probe.
	 */
	if (!(chip_id0 & MSP891_CHIP_ID0_BIT) || chip_id1 != MSP891_CHIP_ID1_VAL) {
		dev_err(&client->dev, "check chip id failed\n");
		return -ENODEV;
	}

	return 0;
}

/* stock msp891_init register sequence, reproduced as a literal list */
static int msp891_hw_init(struct i2c_client *client)
{
	static const struct msp891_reg_value sequence[] = {
		{ 0x7f, 0x67 },
		{ 0x56, 0x00 },
		{ 0x07, 0x08 },
		{ 0x08, 0x00 },
		{ 0x09, 0x00 },
	};
	int ret;

	ret = msp891_write(client, 0x06, 0x80);
	if (ret)
		return ret;
	msleep(10);

	ret = msp891_write_sequence(client, sequence, ARRAY_SIZE(sequence));
	if (ret)
		return ret;

	return msp891_write(client, 0x06, 0x13);
}

static void msp891_work(struct work_struct *work)
{
	struct msp891_data *data = container_of(work, struct msp891_data,
						 work.work);
	u8 buf[3];
	s32 raw;
	s16 traw;
	int ret;

	msleep(15);
	ret = msp891_read(data->client, MSP891_REG_PRESSURE, buf, 3);
	if (!ret) {
		raw = buf[0] | (buf[1] << 8) | (buf[2] << 16);
		if (raw >= 0x800000)
			raw -= 0x1000000;
		input_report_rel(data->pres_input, REL_MISC, raw >> 6);
		input_sync(data->pres_input);
	} else {
		dev_err_ratelimited(&data->client->dev,
				    "pressure read failed: %d\n", ret);
	}

	msleep(3);
	ret = msp891_read(data->client, MSP891_REG_TEMPERATURE, buf, 2);
	if (!ret) {
		/*
		 * Stock shifts the 16 bit raw value without sign-extending
		 * it first: a negative reading comes out wrong. Fixed here.
		 */
		traw = (s16)(buf[0] | (buf[1] << 8));
		input_report_rel(data->temp_input, REL_MISC, (s32)traw >> 7);
		input_sync(data->temp_input);
	} else {
		dev_err_ratelimited(&data->client->dev,
				    "temperature read failed: %d\n", ret);
	}

	schedule_delayed_work(&data->work, msecs_to_jiffies(READ_ONCE(data->delay_ms)));
}

static void msp891_set_enabled(struct msp891_data *data, bool enable)
{
	mutex_lock(&data->lock);
	if (enable != data->enabled) {
		data->enabled = enable;
		if (enable)
			schedule_delayed_work(&data->work,
					      msecs_to_jiffies(data->delay_ms));
		else
			cancel_delayed_work_sync(&data->work);
	}
	mutex_unlock(&data->lock);
}

static void msp891_set_delay(struct msp891_data *data, unsigned int delay_ms)
{
	delay_ms = clamp(delay_ms, (unsigned int)MSP891_MIN_DELAY_MS,
			 (unsigned int)MSP891_MAX_DELAY_MS);

	mutex_lock(&data->lock);
	if (data->enabled)
		cancel_delayed_work_sync(&data->work);
	data->delay_ms = delay_ms;
	if (data->enabled)
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
	mutex_unlock(&data->lock);
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct msp891_data *data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", data->enabled);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct msp891_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	msp891_set_enabled(data, !!val);

	return count;
}

static ssize_t delay_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct msp891_data *data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", data->delay_ms);
}

static ssize_t delay_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct msp891_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	msp891_set_delay(data, val);

	return count;
}

static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(delay);

static struct attribute *msp891_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_delay.attr,
	NULL
};

static const struct attribute_group msp891_attr_group = {
	.attrs = msp891_attrs,
};

static int msp891_input_init(struct device *dev, struct msp891_data *data,
			     struct input_dev **slot, const char *name)
{
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = name;
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, data);
	input_set_capability(input, EV_REL, REL_MISC);

	ret = input_register_device(input);
	if (ret)
		return ret;

	*slot = input;
	return 0;
}

static int msp891_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct msp891_data *data;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->delay_ms = MSP891_DEFAULT_DELAY_MS;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->work, msp891_work);
	i2c_set_clientdata(client, data);

	ret = msp891_check_id(client);
	if (ret)
		return ret;

	ret = msp891_hw_init(client);
	if (ret)
		return ret;

	ret = msp891_input_init(dev, data, &data->pres_input,
				MSP891_INPUT_PRES_NAME);
	if (ret)
		return ret;
	ret = msp891_input_init(dev, data, &data->temp_input,
				MSP891_INPUT_TEMP_NAME);
	if (ret)
		return ret;

	ret = sysfs_create_group(&data->pres_input->dev.kobj, &msp891_attr_group);
	if (ret)
		return ret;
	ret = sysfs_create_group(&data->temp_input->dev.kobj, &msp891_attr_group);
	if (ret)
		goto err_pres_group;

	dev_info(dev, "MSP891 pressure/temperature sensor ready\n");
	return 0;

err_pres_group:
	sysfs_remove_group(&data->pres_input->dev.kobj, &msp891_attr_group);
	return ret;
}

static int msp891_remove(struct i2c_client *client)
{
	struct msp891_data *data = i2c_get_clientdata(client);

	/* Stock frees state without cancelling the work first; fixed here. */
	cancel_delayed_work_sync(&data->work);
	sysfs_remove_group(&data->temp_input->dev.kobj, &msp891_attr_group);
	sysfs_remove_group(&data->pres_input->dev.kobj, &msp891_attr_group);

	return 0;
}

static void msp891_shutdown(struct i2c_client *client)
{
	struct msp891_data *data = i2c_get_clientdata(client);

	if (data)
		cancel_delayed_work_sync(&data->work);
}

#ifdef CONFIG_PM_SLEEP
static int msp891_suspend(struct device *dev)
{
	struct msp891_data *data = dev_get_drvdata(dev);

	data->resume_enabled = data->enabled;
	if (data->resume_enabled)
		cancel_delayed_work_sync(&data->work);

	return 0;
}

static int msp891_resume(struct device *dev)
{
	struct msp891_data *data = dev_get_drvdata(dev);

	if (data->resume_enabled)
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));

	return 0;
}

static SIMPLE_DEV_PM_OPS(msp891_pm_ops, msp891_suspend, msp891_resume);
#define MSP891_PM_OPS (&msp891_pm_ops)
#else
#define MSP891_PM_OPS NULL
#endif

static const struct i2c_device_id msp891_ids[] = {
	{ "MSP891", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, msp891_ids);

static const struct of_device_id msp891_of_match[] = {
	{ .compatible = "pressure,msp891" },
	{ }
};
MODULE_DEVICE_TABLE(of, msp891_of_match);

static struct i2c_driver msp891_driver = {
	.driver = {
		.name = MSP891_DRV_NAME,
		.of_match_table = msp891_of_match,
		.pm = MSP891_PM_OPS,
	},
	.probe = msp891_probe,
	.remove = msp891_remove,
	.shutdown = msp891_shutdown,
	.id_table = msp891_ids,
};

module_i2c_driver(msp891_driver);

MODULE_DESCRIPTION("MSP891 pressure and temperature sensor");
MODULE_LICENSE("GPL v2");
