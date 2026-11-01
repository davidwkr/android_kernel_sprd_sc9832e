// SPDX-License-Identifier: GPL-2.0
/*
 * Orient-Chip OCH1970 3-axis Hall magnetic sensor driver (input device).
 *
 * Written from the public OCH1970VAD-H datasheet (V1.1). The sensor is
 * left in power-down mode and is only run while a userspace sensor HAL
 * has it enabled through sysfs. Each sample is a single measurement
 * (the chip returns to power-down by itself afterwards), polled from a
 * delayed work. The DRDY/irq pin and the hall "open/close" thresholds of
 * the stock firmware are not used.
 *
 * Output: input device "och1970", ABS_X/Y/Z as raw signed 16 bit counts,
 * 1.1 uT/LSB (high sensitivity, SMR=0; range about +-36 mT).
 *
 * sysfs ABI (on the i2c device):
 *   enable  0/1   start/stop periodic measurement
 *   delay   ms    sample period; values below 10 are clamped to 10 (and
 *                 above 10000 to 10000), applied from the next sample
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define OCH1970_DRV_NAME	"och1970"
#define OCH1970_INPUT_NAME	"och1970"

/* Registers (datasheet Table 12.1) */
#define OCH1970_REG_WIA		0x00	/* 2 bytes: WIA[15:8], WIA[7:0] */
#define OCH1970_REG_DATA_ZYX	0x17	/* ST(2), HZ, HY, HX; 16 bit big endian */
#define OCH1970_REG_CNTL2	0x21
#define OCH1970_REG_SRST	0x30

#define OCH1970_WIA_COMPANY	0x48
#define OCH1970_WIA_DEVICE	0xc0

#define OCH1970_ST_DRDY		BIT(0)	/* ST[7:0] */

#define OCH1970_CNTL2_SDR	BIT(4)	/* 0: low noise drive */
#define OCH1970_CNTL2_SMR	BIT(5)	/* 0: high sensitivity (1.1 uT/LSB) */
#define OCH1970_MODE_POWERDOWN	0x0
#define OCH1970_MODE_SINGLE	0x1

#define OCH1970_SRST		BIT(0)

#define OCH1970_DEFAULT_DELAY_MS	200
#define OCH1970_MIN_DELAY_MS		10
#define OCH1970_MAX_DELAY_MS		10000

#define OCH1970_ABS_MAX		32768

struct och1970_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	struct mutex enable_lock;	/* serialises set_enabled(); not taken by the work */
	struct mutex lock;		/* enabled, delay_ms, chip mode */
	unsigned int delay_ms;
	bool enabled;
	bool resume_enabled;
};

static int och1970_read(struct i2c_client *client, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = len,
		  .buf = buf },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		return ret;
	return ret == 2 ? 0 : -EIO;
}

static int och1970_write(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	int ret;

	ret = i2c_master_send(client, buf, 2);
	if (ret < 0)
		return ret;
	return ret == 2 ? 0 : -EIO;
}

static int och1970_power_down(struct och1970_data *data)
{
	return och1970_write(data->client, OCH1970_REG_CNTL2,
			     OCH1970_MODE_POWERDOWN);
}

/* One single measurement, then report. Called from the work only. */
static int och1970_sample(struct och1970_data *data)
{
	u8 buf[8];
	int ret, retry;

	ret = och1970_write(data->client, OCH1970_REG_CNTL2,
			    OCH1970_MODE_SINGLE);
	if (ret)
		return ret;

	for (retry = 0; retry < 3; retry++) {
		/* measurement time is 0.872 ms max (low noise drive) */
		usleep_range(1000, 1500);
		ret = och1970_read(data->client, OCH1970_REG_DATA_ZYX,
				   buf, sizeof(buf));
		if (ret)
			return ret;
		if (buf[1] & OCH1970_ST_DRDY)
			break;
	}
	if (!(buf[1] & OCH1970_ST_DRDY))
		return -EAGAIN;

	input_report_abs(data->input, ABS_X, (s16)((buf[6] << 8) | buf[7]));
	input_report_abs(data->input, ABS_Y, (s16)((buf[4] << 8) | buf[5]));
	input_report_abs(data->input, ABS_Z, (s16)((buf[2] << 8) | buf[3]));
	input_sync(data->input);

	return 0;
}

static void och1970_work(struct work_struct *work)
{
	struct och1970_data *data =
		container_of(work, struct och1970_data, work.work);
	int ret;

	mutex_lock(&data->lock);
	if (data->enabled) {
		ret = och1970_sample(data);
		if (ret)
			dev_dbg(&data->client->dev, "sample failed: %d\n", ret);
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
	}
	mutex_unlock(&data->lock);
}

static int och1970_set_enabled(struct och1970_data *data, bool enable)
{
	int ret = 0;

	mutex_lock(&data->enable_lock);
	mutex_lock(&data->lock);
	if (data->enabled == enable) {
		mutex_unlock(&data->lock);
		/* a failed power-down is retried by the next disable */
		if (!enable)
			ret = och1970_power_down(data);
		goto out;
	}
	data->enabled = enable;
	if (enable)
		schedule_delayed_work(&data->work, 0);
	mutex_unlock(&data->lock);

	if (!enable) {
		/* the work cannot rearm itself once enabled is false */
		cancel_delayed_work_sync(&data->work);
		ret = och1970_power_down(data);
	}
out:
	mutex_unlock(&data->enable_lock);
	return ret;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct och1970_data *data = dev_get_drvdata(dev);
	bool enabled;

	mutex_lock(&data->lock);
	enabled = data->enabled;
	mutex_unlock(&data->lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", enabled);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct och1970_data *data = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	ret = och1970_set_enabled(data, enable);

	return ret ? ret : count;
}

static ssize_t delay_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct och1970_data *data = dev_get_drvdata(dev);
	unsigned int delay_ms;

	mutex_lock(&data->lock);
	delay_ms = data->delay_ms;
	mutex_unlock(&data->lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", delay_ms);
}

static ssize_t delay_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct och1970_data *data = dev_get_drvdata(dev);
	unsigned int delay_ms;
	int ret;

	ret = kstrtouint(buf, 10, &delay_ms);
	if (ret)
		return ret;

	delay_ms = clamp_t(unsigned int, delay_ms, OCH1970_MIN_DELAY_MS,
			   OCH1970_MAX_DELAY_MS);
	mutex_lock(&data->lock);
	data->delay_ms = delay_ms;	/* applies from the next sample */
	mutex_unlock(&data->lock);

	return count;
}

static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(delay);

static struct attribute *och1970_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_delay.attr,
	NULL
};

static const struct attribute_group och1970_attr_group = {
	.attrs = och1970_attrs,
};

static int och1970_hw_reset(struct i2c_client *client)
{
	int gpio = of_get_named_gpio(client->dev.of_node, "rst-gpio", 0);
	int ret;

	if (gpio == -ENOENT) {
		dev_dbg(&client->dev, "no rst-gpio, soft reset only\n");
		return 0;
	}
	if (!gpio_is_valid(gpio))
		return gpio < 0 ? gpio : -EINVAL;

	ret = devm_gpio_request_one(&client->dev, gpio, GPIOF_OUT_INIT_HIGH,
				    "och1970-rstn");
	if (ret)
		return ret;

	/* RSTN is active low: >= 5 us low pulse, then >= 5 us wait */
	gpio_set_value_cansleep(gpio, 0);
	usleep_range(20, 50);
	gpio_set_value_cansleep(gpio, 1);
	usleep_range(100, 200);

	return 0;
}

/* read-only identity check, safe on a chip that may not be ours */
static int och1970_check_id(struct i2c_client *client)
{
	u8 id[2];
	int ret;

	ret = och1970_read(client, OCH1970_REG_WIA, id, sizeof(id));
	if (ret)
		return ret;
	if (id[0] != OCH1970_WIA_COMPANY || id[1] != OCH1970_WIA_DEVICE) {
		dev_err(&client->dev, "unexpected chip id %02x%02x\n",
			id[0], id[1]);
		return -ENODEV;
	}

	return 0;
}

static int och1970_detect(struct och1970_data *data)
{
	u8 id[2];
	int ret, pass;

	/* check the id before any write, so a foreign chip is never touched */
	for (pass = 0; pass < 2; pass++) {
		ret = och1970_read(data->client, OCH1970_REG_WIA, id,
				   sizeof(id));
		if (ret)
			return ret;
		if (id[0] != OCH1970_WIA_COMPANY ||
		    id[1] != OCH1970_WIA_DEVICE) {
			dev_err(&data->client->dev,
				"unexpected chip id %02x%02x\n", id[0], id[1]);
			return -ENODEV;
		}
		if (pass)
			break;
		ret = och1970_write(data->client, OCH1970_REG_SRST,
				    OCH1970_SRST);
		if (ret)
			return ret;
		usleep_range(1000, 2000);
	}

	return och1970_power_down(data);
}

static int och1970_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct och1970_data *data;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->delay_ms = OCH1970_DEFAULT_DELAY_MS;
	mutex_init(&data->lock);
	mutex_init(&data->enable_lock);
	INIT_DELAYED_WORK(&data->work, och1970_work);
	i2c_set_clientdata(client, data);

	/* validate the chip before touching rst-gpio or writing anything */
	ret = och1970_check_id(client);
	if (ret)
		return ret;

	ret = och1970_hw_reset(client);
	if (ret)
		return ret;

	ret = och1970_detect(data);
	if (ret)
		return ret;

	data->input = input_allocate_device();
	if (!data->input)
		return -ENOMEM;

	data->input->name = OCH1970_INPUT_NAME;
	data->input->id.bustype = BUS_I2C;
	data->input->dev.parent = &client->dev;
	input_set_drvdata(data->input, data);
	input_set_abs_params(data->input, ABS_X, -OCH1970_ABS_MAX,
			     OCH1970_ABS_MAX - 1, 0, 0);
	input_set_abs_params(data->input, ABS_Y, -OCH1970_ABS_MAX,
			     OCH1970_ABS_MAX - 1, 0, 0);
	input_set_abs_params(data->input, ABS_Z, -OCH1970_ABS_MAX,
			     OCH1970_ABS_MAX - 1, 0, 0);

	ret = input_register_device(data->input);
	if (ret) {
		input_free_device(data->input);
		return ret;
	}

	ret = sysfs_create_group(&client->dev.kobj, &och1970_attr_group);
	if (ret) {
		input_unregister_device(data->input);
		return ret;
	}

	dev_info(&client->dev, "OCH1970 magnetic sensor detected\n");
	return 0;
}

static int och1970_remove(struct i2c_client *client)
{
	struct och1970_data *data = i2c_get_clientdata(client);

	/* userspace interface first, then stop the work, then the input */
	sysfs_remove_group(&client->dev.kobj, &och1970_attr_group);
	och1970_set_enabled(data, false);
	input_unregister_device(data->input);

	return 0;
}

static void och1970_shutdown(struct i2c_client *client)
{
	struct och1970_data *data = i2c_get_clientdata(client);

	if (data)
		och1970_set_enabled(data, false);
}

#ifdef CONFIG_PM_SLEEP
static int och1970_suspend(struct device *dev)
{
	struct och1970_data *data = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&data->lock);
	data->resume_enabled = data->enabled;
	mutex_unlock(&data->lock);

	/* always run the power-down path, it retries an earlier failure */
	ret = och1970_set_enabled(data, false);
	if (ret) {
		dev_warn(dev, "power-down failed on suspend\n");
		/* PM will not call resume for us: restore the prior state */
		if (data->resume_enabled)
			och1970_set_enabled(data, true);
		data->resume_enabled = false;
	}

	return ret;
}

static int och1970_resume(struct device *dev)
{
	struct och1970_data *data = dev_get_drvdata(dev);

	return data->resume_enabled ? och1970_set_enabled(data, true) : 0;
}

static SIMPLE_DEV_PM_OPS(och1970_pm_ops, och1970_suspend, och1970_resume);
#define OCH1970_PM_OPS (&och1970_pm_ops)
#else
#define OCH1970_PM_OPS NULL
#endif

static const struct i2c_device_id och1970_ids[] = {
	{ OCH1970_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, och1970_ids);

static const struct of_device_id och1970_of_match[] = {
	{ .compatible = "magnetic,och1970" },
	{ }
};
MODULE_DEVICE_TABLE(of, och1970_of_match);

static struct i2c_driver och1970_driver = {
	.driver = {
		.name = OCH1970_DRV_NAME,
		.of_match_table = och1970_of_match,
		.pm = OCH1970_PM_OPS,
		.suppress_bind_attrs = true,
	},
	.probe = och1970_probe,
	.remove = och1970_remove,
	.shutdown = och1970_shutdown,
	.id_table = och1970_ids,
};

module_i2c_driver(och1970_driver);

MODULE_DESCRIPTION("Orient-Chip OCH1970 3-axis Hall magnetic sensor");
MODULE_LICENSE("GPL v2");
