// SPDX-License-Identifier: GPL-2.0
/*
 * AF6837 3-axis magnetometer (compass) used on the stock MT107/DM63 boards.
 *
 * Behaviour follows the stock vendor driver as reconstructed in
 * devices/AF6837_INTERFACE.md (the stock source itself was not available:
 * the register map and probe sequence come from disassembly of the stock
 * kernel image). Pure I2C, no GPIO, no interrupt line.
 *
 * Output: input device "compass", EV_ABS ABS_RX/RY/RZ in raw register
 * units, plus an EV_ABS ABS_MISC capability kept only for userspace ABI
 * compatibility (stock configures it too but was never observed writing a
 * sample to it).
 *
 * The stock driver's periodic sampling mechanism (timer vs. workqueue, and
 * its period) could not be recovered from the disassembly, and there is no
 * DT property to configure it either way. This port polls on a
 * delayed_work instead, gated by the "enable" (0/1) sysfs attribute on the
 * input device -- polling is off until enabled, unlike stock which appears
 * to self-arm at probe. "delay" (sysfs, milliseconds) selects the polling
 * period.
 *
 * Per the register map, the stock read function re-arms the chip (register
 * 0x0a = 0x01) only when at least one axis sampled non-zero, silently
 * skipping the re-arm -- and reporting nothing -- when all three read
 * exactly zero. That looks like a stock bug: a chip stuck reporting all
 * zeroes is never re-armed and so never recovers. This port always
 * re-arms after a successful transfer, zero sample or not.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define AF6837_DRV_NAME		"af6837"
#define AF6837_INPUT_NAME	"compass"

/* Registers, see devices/AF6837_INTERFACE.md */
#define AF6837_REG_CHIP_ID	0x00	/* read 1 byte, must be AF6837_CHIP_ID */
#define AF6837_REG_STATUS	0x01	/* read once at init, side effect only */
#define AF6837_REG_DATA		0x03	/* 6 bytes: X,Y,Z, little-endian s16 */
#define AF6837_REG_CTRL1	0x0a	/* 0x09 at init, 0x01 after each sample */
#define AF6837_REG_CTRL2	0x0c
#define AF6837_REG_RESET	0x11

#define AF6837_CHIP_ID		0x6c

#define AF6837_DEFAULT_DELAY_MS	200
#define AF6837_MIN_DELAY_MS	20
#define AF6837_MAX_DELAY_MS	2000

struct af6837_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	struct mutex ctl;	/* serializes enable/delay transitions */
	atomic_t enabled;
	unsigned int delay_ms;
	bool resume_enabled;
};

static int af6837_read(struct i2c_client *client, u8 reg, u8 *buf, int len)
{
	int ret = i2c_smbus_read_i2c_block_data(client, reg, len, buf);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int af6837_write(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

static int af6837_check_id(struct i2c_client *client)
{
	u8 id;
	int ret;

	ret = af6837_read(client, AF6837_REG_CHIP_ID, &id, 1);
	if (ret)
		return ret;
	if (id != AF6837_CHIP_ID) {
		dev_err(&client->dev, "unexpected chip id 0x%02x\n", id);
		return -ENODEV;
	}

	return 0;
}

/* stock af6837_i2c_probe init sequence */
static int af6837_hw_init(struct i2c_client *client)
{
	u8 status;
	int ret;

	ret = af6837_write(client, AF6837_REG_RESET, 0x81);
	if (ret)
		return ret;
	msleep(11);

	ret = af6837_write(client, AF6837_REG_CTRL2, 0x04);
	if (ret)
		return ret;

	/* result discarded by stock too: looks like a clear-on-read flag */
	af6837_read(client, AF6837_REG_STATUS, &status, 1);

	ret = af6837_write(client, AF6837_REG_CTRL1, 0x09);
	if (ret)
		return ret;
	msleep(36);

	return 0;
}

/*
 * stock af6837_read_xyz: a single 6 byte block read, unpacked as three
 * little-endian s16 in register order (X, Y, Z). Returns -EAGAIN when all
 * three samples are exactly zero, same as the stock silent "return -1" --
 * the caller decides whether that is worth logging.
 */
static int af6837_read_xyz(struct af6837_data *data, s16 *x, s16 *y, s16 *z)
{
	u8 buf[6];
	int ret;

	ret = af6837_read(data->client, AF6837_REG_DATA, buf, sizeof(buf));
	if (ret)
		return ret;

	*x = (s16)(buf[0] | (buf[1] << 8));
	*y = (s16)(buf[2] | (buf[3] << 8));
	*z = (s16)(buf[4] | (buf[5] << 8));

	/* re-arm for the next sample; always, see the file header comment */
	ret = af6837_write(data->client, AF6837_REG_CTRL1, 0x01);
	if (ret)
		return ret;

	if (*x == 0 && *y == 0 && *z == 0)
		return -EAGAIN;

	return 0;
}

static void af6837_work(struct work_struct *work)
{
	struct af6837_data *data = container_of(work, struct af6837_data,
						 work.work);
	s16 x, y, z;
	int ret;

	if (!atomic_read(&data->enabled))
		return;

	ret = af6837_read_xyz(data, &x, &y, &z);
	if (!ret) {
		input_report_abs(data->input, ABS_RX, x);
		input_report_abs(data->input, ABS_RY, y);
		input_report_abs(data->input, ABS_RZ, z);
		input_sync(data->input);
	} else if (ret != -EAGAIN) {
		dev_err_ratelimited(&data->client->dev,
				    "failed to read sample: %d\n", ret);
	}

	if (atomic_read(&data->enabled))
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
}

static int af6837_set_enabled(struct af6837_data *data, bool enable)
{
	mutex_lock(&data->ctl);
	if (enable == !!atomic_read(&data->enabled)) {
		mutex_unlock(&data->ctl);
		return 0;
	}

	if (enable) {
		atomic_set(&data->enabled, 1);
		schedule_delayed_work(&data->work,
				      msecs_to_jiffies(data->delay_ms));
	} else {
		atomic_set(&data->enabled, 0);
		cancel_delayed_work_sync(&data->work);
	}
	mutex_unlock(&data->ctl);

	return 0;
}

static void af6837_set_delay(struct af6837_data *data, unsigned int delay_ms)
{
	bool enabled;

	delay_ms = clamp(delay_ms, (unsigned int)AF6837_MIN_DELAY_MS,
			 (unsigned int)AF6837_MAX_DELAY_MS);

	mutex_lock(&data->ctl);
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
	mutex_unlock(&data->ctl);
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct af6837_data *data = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->enabled));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct af6837_data *data = dev_get_drvdata(dev->parent);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;
	if (val != 0 && val != 1)
		return -EINVAL;

	af6837_set_enabled(data, val);

	return count;
}

static ssize_t delay_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct af6837_data *data = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%u\n", data->delay_ms);
}

static ssize_t delay_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct af6837_data *data = dev_get_drvdata(dev->parent);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	af6837_set_delay(data, val);

	return count;
}

static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(delay);

static struct attribute *af6837_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_delay.attr,
	NULL
};

static const struct attribute_group af6837_attr_group = {
	.attrs = af6837_attrs,
};

static int af6837_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct af6837_data *data;
	struct input_dev *input;
	s16 x, y, z;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->delay_ms = AF6837_DEFAULT_DELAY_MS;
	mutex_init(&data->ctl);
	INIT_DELAYED_WORK(&data->work, af6837_work);
	i2c_set_clientdata(client, data);

	ret = af6837_check_id(client);
	if (ret)
		return ret;

	ret = af6837_hw_init(client);
	if (ret) {
		dev_err(dev, "chip init failed: %d\n", ret);
		return ret;
	}

	/* self-test, as stock af6837_i2c_probe: the result is not reported */
	ret = af6837_read_xyz(data, &x, &y, &z);
	if (ret && ret != -EAGAIN) {
		dev_err(dev, "self-test read failed: %d\n", ret);
		return ret;
	}

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	data->input = input;
	input->name = AF6837_INPUT_NAME;
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;
	input_set_drvdata(input, data);
	input_set_capability(input, EV_ABS, ABS_RX);
	input_set_capability(input, EV_ABS, ABS_RY);
	input_set_capability(input, EV_ABS, ABS_RZ);
	input_set_capability(input, EV_ABS, ABS_MISC);
	input_set_abs_params(input, ABS_RX, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_RY, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_RZ, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_MISC, INT_MIN, INT_MAX, 0, 0);

	ret = input_register_device(input);
	if (ret)
		return ret;

	ret = sysfs_create_group(&input->dev.kobj, &af6837_attr_group);
	if (ret) {
		/* a partly created group may have enabled polling meanwhile */
		af6837_set_enabled(data, false);
		return ret;
	}

	dev_info(dev, "AF6837 magnetometer probed\n");

	return 0;
}

static int af6837_remove(struct i2c_client *client)
{
	struct af6837_data *data = i2c_get_clientdata(client);

	sysfs_remove_group(&data->input->dev.kobj, &af6837_attr_group);
	af6837_set_enabled(data, false);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int af6837_suspend(struct device *dev)
{
	struct af6837_data *data = dev_get_drvdata(dev);

	data->resume_enabled = !!atomic_read(&data->enabled);
	if (data->resume_enabled)
		af6837_set_enabled(data, false);

	return 0;
}

static int af6837_resume(struct device *dev)
{
	struct af6837_data *data = dev_get_drvdata(dev);

	if (data->resume_enabled)
		af6837_set_enabled(data, true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(af6837_pm_ops, af6837_suspend, af6837_resume);
#define AF6837_PM_OPS (&af6837_pm_ops)
#else
#define AF6837_PM_OPS NULL
#endif

static const struct i2c_device_id af6837_ids[] = {
	{ AF6837_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, af6837_ids);

static const struct of_device_id af6837_of_match[] = {
	{ .compatible = "compass,af6837" },
	{ }
};
MODULE_DEVICE_TABLE(of, af6837_of_match);

static struct i2c_driver af6837_driver = {
	.driver = {
		.name = AF6837_DRV_NAME,
		.of_match_table = af6837_of_match,
		.pm = AF6837_PM_OPS,
	},
	.probe = af6837_probe,
	.remove = af6837_remove,
	.id_table = af6837_ids,
};

module_i2c_driver(af6837_driver);

MODULE_DESCRIPTION("AF6837 3-axis magnetometer");
MODULE_LICENSE("GPL v2");
