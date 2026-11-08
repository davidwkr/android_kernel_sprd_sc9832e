/*
 * Rohm BD1xxx (BD1820) PPG heart-rate sensor: userspace ABI of the stock
 * driver, without the measurement back end.
 *
 * The vendor Android sensors HAL expects:
 *
 *  - an input device named "heartrate" (EV_KEY KEY_63, EV_REL X/Y/Z) with the
 *    sysfs attributes "enable" and "chipinfo" (read as "bd1820", no newline),
 *  - a misc device /dev/bd1662rate: write(2) of up to 20 bytes with byte 0 =
 *    test mode and byte 1 = enable, read(2) of raw 16-bit PPG samples.
 *
 * The HAL computes heart rate and SpO2 from the raw samples.  Programming
 * the chip (BD_Sensor_Init/Start/Data_Read of the stock driver) is not done
 * here: the register order and the sample layout were not fully recovered
 * (see devices/BD1XXX_INTERFACE.md), so the driver only powers the chip,
 * checks its ID and returns an all-zero raw buffer, and writes no unknown
 * register.  Enabling therefore never produces measurement data or events.
 *
 * TODO: measurement back end (init register blocks, polling every 50 ms
 * (200 ms in finger mode 15), the 3 x 35 sample layout of the read buffer,
 * input events after each sample set).
 *
 * Interface details and evidence: devices/BD1XXX_INTERFACE.md
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
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

#define BD1XXX_DRV_NAME		"bd1xxx"
#define BD1XXX_INPUT_NAME	"heartrate"
#define BD1XXX_MISC_NAME	"bd1662rate"	/* stock name for all chips */

#define BD1XXX_REG_ID_LO	0xbc
#define BD1XXX_REG_ID_HI	0xbd
#define BD1XXX_ID_HI_BD1820	0x70
#define BD1XXX_ID_LO_BD1820	0x11

#define BD1XXX_VDD_UV		2800000
#define BD1XXX_POWERUP_DELAY_MS	10

#define BD1XXX_KEY_CODE		63
#define BD1XXX_RAW_SIZE		400	/* stock raw buffer */
#define BD1XXX_WRITE_MAX	20	/* HAL write size: mode, enable, ... */

struct bd1xxx_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct miscdevice misc;
	struct regulator *vdd;	/* NULL when the DT has no vdd_name */
	struct mutex lock;	/* protects mode, enabled, powered */
	int power_gpio;		/* < 0 when the board has no power switch */
	unsigned int mode;	/* test mode written by the HAL */
	bool enabled;
	bool powered;
	bool suspended;
	u8 raw[BD1XXX_RAW_SIZE];	/* always zero: no measurement yet */
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

	/* TODO: stock close writes reg 0x00 = 0x00, 0x80, 0x00 first. */
	if (gpio_is_valid(data->power_gpio))
		gpio_set_value_cansleep(data->power_gpio, 0);
	if (data->vdd)
		regulator_disable(data->vdd);

	data->powered = false;
}

static int bd1xxx_set_enable(struct bd1xxx_data *data, bool enable)
{
	int ret = 0;

	mutex_lock(&data->lock);
	if (enable) {
		if (!data->suspended)
			ret = bd1xxx_power_on(data);
		if (!ret)
			data->enabled = true;
		/* TODO: program the chip for data->mode and start sampling. */
	} else {
		data->enabled = false;
		bd1xxx_power_off(data);
	}
	mutex_unlock(&data->lock);

	return ret;
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

static ssize_t bd1xxx_misc_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct miscdevice *misc = file->private_data;
	struct bd1xxx_data *data = container_of(misc, struct bd1xxx_data, misc);
	size_t len = min_t(size_t, count, sizeof(data->raw));

	/* Stock returns the saved sample buffer; ours is always zero. */
	mutex_lock(&data->lock);
	if (copy_to_user(buf, data->raw, len))
		len = -EFAULT;
	mutex_unlock(&data->lock);

	return len;
}

static ssize_t bd1xxx_misc_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct miscdevice *misc = file->private_data;
	struct bd1xxx_data *data = container_of(misc, struct bd1xxx_data, misc);
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
	.open = nonseekable_open,
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

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);

	ret = bd1xxx_parse_dt(data);
	if (ret)
		return ret;

	if (gpio_is_valid(data->power_gpio)) {
		ret = devm_gpio_request_one(dev, data->power_gpio,
					    GPIOF_OUT_INIT_LOW,
					    "bd1xxx-power-gpio");
		if (ret) {
			dev_err(dev, "cannot request power gpio %d: %d\n",
				data->power_gpio, ret);
			return ret;
		}
	}

	ret = bd1xxx_power_on(data);
	if (ret)
		return ret;

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
	return ret;
}

static int bd1xxx_remove(struct i2c_client *client)
{
	struct bd1xxx_data *data = i2c_get_clientdata(client);

	misc_deregister(&data->misc);
	input_unregister_device(data->input);

	mutex_lock(&data->lock);
	data->enabled = false;
	bd1xxx_power_off(data);
	mutex_unlock(&data->lock);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int bd1xxx_suspend(struct device *dev)
{
	struct bd1xxx_data *data = dev_get_drvdata(dev);

	/* Stock does nothing here; with no sampling there is nothing to keep. */
	mutex_lock(&data->lock);
	data->suspended = true;
	bd1xxx_power_off(data);
	mutex_unlock(&data->lock);

	return 0;
}

static int bd1xxx_resume(struct device *dev)
{
	struct bd1xxx_data *data = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&data->lock);
	data->suspended = false;
	if (data->enabled)
		ret = bd1xxx_power_on(data);
	mutex_unlock(&data->lock);

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

MODULE_DESCRIPTION("Rohm BD1xxx (BD1820) heart-rate sensor interface driver");
MODULE_LICENSE("GPL");
