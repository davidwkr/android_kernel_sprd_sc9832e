/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SC7A22/SC7A20 accelerometer driver for the legacy Spreadtrum sensor HAL.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define SC7A22_DRV_NAME                  "sc7a22"
#define SC7A22_MISC_NAME                 "qmax981"
#define SC7A22_INPUT_NAME                "accelerometer"

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
	struct mutex lock; /* Serializes enable, delay, and work transitions. */
	atomic_t enabled;
	unsigned int delay_ms;
	unsigned int layout;
	u8 chip_id;
	bool resume_enabled;
	struct class *control_class;
	struct device *control_dev;
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

static int sc7a22_hw_init_13(struct sc7a22_data *data)
{
	const struct sc7a22_reg_value sequence[] = {
		{ 0x20, 0x4f },
		{ 0x23, 0x88 },
		{ 0x1f, 0x08 },
		{ 0x57, 0x08 },
		{ 0x08, 0x11 },
		{ 0x09, data->layout | 0x70 },
		{ 0x0a, 0x00 },
		{ 0x0b, 0xc8 },
		{ 0x06, 0x15 },
	};

	return sc7a22_write_sequence(data, sequence, ARRAY_SIZE(sequence));
}

static int sc7a22_hw_init_18(struct sc7a22_data *data)
{
	const struct sc7a22_reg_value core_sequence[] = {
		{ 0x40, 0x26 },
		{ 0x41, 0x00 },
		{ 0x05, 0x50 },
		{ 0x06, 0x01 },
		{ 0x08, 0x05 },
	};
	const struct sc7a22_reg_value bank_90_sequence[] = {
		{ 0x3e, 0x10 },
		{ 0x51, data->layout | 0x70 },
		{ 0x52, 0x00 },
		{ 0x53, 0xc8 },
	};
	int ret;

	ret = sc7a22_write_reg(data, SC7A22_REG_BANK, SC7A22_BANK_0);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = sc7a22_write_reg(data, 0x4a, 0xa5);
	if (ret < 0)
		return ret;
	msleep(50);

	ret = sc7a22_write_reg(data, SC7A22_REG_BANK, SC7A22_BANK_0);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = sc7a22_write_reg(data, 0x7d, 0x04);
	if (ret < 0)
		return ret;
	usleep_range(10000, 12000);
	ret = sc7a22_write_sequence(data, core_sequence,
				    ARRAY_SIZE(core_sequence));
	if (ret < 0)
		return ret;

	ret = sc7a22_write_reg(data, SC7A22_REG_BANK, SC7A22_BANK_90);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = sc7a22_write_sequence(data, bank_90_sequence,
				    ARRAY_SIZE(bank_90_sequence));
	if (ret < 0)
		return ret;
	ret = sc7a22_write_reg(data, SC7A22_REG_BANK, SC7A22_BANK_0);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);

	return sc7a22_write_reg(data, 0x06, 0x15);
}

static int sc7a22_hw_init(struct sc7a22_data *data)
{
	if (data->chip_id == SC7A22_H_ID)
		return sc7a22_hw_init_18(data);

	return sc7a22_hw_init_13(data);
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
		raw[i] >>= 4;
		/* The HAL's qmax981 path expects 256 counts per g. */
		if (data->chip_id == SC7A22_H_ID)
			raw[i] /= 4;
		else
			raw[i] /= 2;
	}

	map = &sc7a22_axis_maps[data->layout];
	for (i = 0; i < 3; i++)
		sample[i] = map->sign[i] * raw[map->axis[i]];

	return 0;
}

static void sc7a22_work(struct work_struct *work)
{
	struct sc7a22_data *data = container_of(work, struct sc7a22_data,
						 work.work);
	int sample[3];
	int ret;

	if (!atomic_read(&data->enabled))
		return;

	ret = sc7a22_read_sample(data, sample);
	if (!ret) {
		input_report_abs(data->input, ABS_X, sample[0]);
		input_report_abs(data->input, ABS_Y, sample[1]);
		input_report_abs(data->input, ABS_Z, sample[2]);
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

static int sc7a22_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct sc7a22_data *data;

	data = container_of(miscdev, struct sc7a22_data, miscdev);
	file->private_data = data;
	return nonseekable_open(inode, file);
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
	bool enabled;
	int ret;

	ret = kstrtobool(buffer, &enabled);
	if (ret)
		return ret;
	ret = sc7a22_set_enabled(data, enabled);

	return ret < 0 ? ret : count;
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

static DEVICE_ATTR(gsensor, 0644, gsensor_show, gsensor_store);
static DEVICE_ATTR(delay_acc, 0644, delay_acc_show, delay_acc_store);

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

static int sc7a22_input_init(struct sc7a22_data *data)
{
	int ret;

	data->input = input_allocate_device();
	if (!data->input)
		return -ENOMEM;

	data->input->name = SC7A22_INPUT_NAME;
	data->input->id.bustype = BUS_I2C;
	data->input->dev.parent = &data->client->dev;
	input_set_drvdata(data->input, data);
	input_set_abs_params(data->input, ABS_X, -SC7A22_ABS_MAX,
			     SC7A22_ABS_MAX, 0, 0);
	input_set_abs_params(data->input, ABS_Y, -SC7A22_ABS_MAX,
			     SC7A22_ABS_MAX, 0, 0);
	input_set_abs_params(data->input, ABS_Z, -SC7A22_ABS_MAX,
			     SC7A22_ABS_MAX, 0, 0);

	ret = input_register_device(data->input);
	if (ret) {
		input_free_device(data->input);
		data->input = NULL;
	}

	return ret;
}

static int sc7a22_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct sc7a22_data *data;
	u32 layout = 0;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->delay_ms = SC7A22_DEFAULT_DELAY_MS;
	mutex_init(&data->lock);
	atomic_set(&data->enabled, 0);
	INIT_DELAYED_WORK(&data->work, sc7a22_work);
	i2c_set_clientdata(client, data);

	if (!of_property_read_u32(client->dev.of_node, "layout", &layout) &&
	    layout < ARRAY_SIZE(sc7a22_axis_maps))
		data->layout = layout;

	ret = sc7a22_detect(data);
	if (ret < 0)
		goto err_free;

	ret = sc7a22_hw_init(data);
	if (ret < 0)
		goto err_free;
	ret = sc7a22_input_init(data);
	if (ret < 0)
		goto err_free;

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = SC7A22_MISC_NAME;
	data->miscdev.fops = &sc7a22_misc_fops;
	data->miscdev.parent = &client->dev;
	ret = misc_register(&data->miscdev);
	if (ret < 0)
		goto err_input;

	sc7a22_create_control_files(data);
	dev_info(&client->dev, "SC7A2x detected, id=0x%02x layout=%u\n",
		 data->chip_id, data->layout);
	return 0;

err_input:
	input_unregister_device(data->input);
	data->input = NULL;
err_free:
	i2c_set_clientdata(client, NULL);
	kfree(data);
	return ret;
}

static int sc7a22_remove(struct i2c_client *client)
{
	struct sc7a22_data *data = i2c_get_clientdata(client);

	sc7a22_set_enabled(data, false);
	sc7a22_remove_control_files(data);
	misc_deregister(&data->miscdev);
	input_unregister_device(data->input);
	i2c_set_clientdata(client, NULL);
	kfree(data);
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

	data->resume_enabled = !!atomic_read(&data->enabled);
	if (data->resume_enabled)
		return sc7a22_set_enabled(data, false);
	return 0;
}

static int sc7a22_resume(struct device *dev)
{
	struct sc7a22_data *data = dev_get_drvdata(dev);

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
