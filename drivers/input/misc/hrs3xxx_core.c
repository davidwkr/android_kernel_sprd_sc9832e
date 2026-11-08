/*
 * TYHX HRS3xxx family (HX3918 "hrs3918", HRS3300) heart-rate sensor driver.
 *
 * Provides the kernel interface the vendor Android sensors HAL expects:
 *
 *  - an input device named "heartrate" reporting EV_REL events
 *    (REL_X = bpm, REL_RX = SpO2, REL_RZ = wear state, ...),
 *  - sysfs attributes "enable" (rw) and "chipinfo" (ro) on that input
 *    device, i.e. /sys/class/input/inputN/{enable,chipinfo}.
 *
 * The heart-rate / SpO2 algorithms of the stock driver are closed vendor
 * code and are not part of this tree.  This driver handles probing,
 * power sequencing and the userspace ABI; measurements come from the stock
 * code only with CONFIG_HRS3XXX_TYHX_BLOB (see hrs3xxx_blob.c).  Without
 * it the driver reports "no valid measurement" (all values 0,
 * REL_HWHEEL/REL_MISC = -1) once per second while enabled.
 *
 * Interface details and evidence: devices/HRS3XXX_INTERFACE.md
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "hrs3xxx_blob.h"

#define HRS3XXX_DRV_NAME	"hrs3xxx"
#define HRS3XXX_INPUT_NAME	"heartrate"

#define HRS3XXX_REG_ID		0x00
#define HRS3XXX_REG_ENABLE	0x01

#define HRS3XXX_ID_HRS3300	0x21
#define HRS3XXX_ID_HRS3918	0x27

#define HRS3XXX_MODE_MAX	3	/* accepted values of "enable": 0..3 */

#define HRS3XXX_POWERUP_DELAY_MS	100
#define HRS3XXX_ID_DELAY_MS		10
#define HRS3XXX_STANDBY_DELAY_MS	5
#define HRS3XXX_REPORT_PERIOD_MS	1000

struct hrs3xxx_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	struct mutex lock;	/* protects mode, power state */
	const char *chipinfo;
	int power_gpio;		/* < 0 when the board has no power switch */
	unsigned int mode;	/* 0 = off, 1..3 = measurement mode */
	bool powered;
	bool ready;		/* input device registered; work may run */
	struct hrsb *blob;	/* stock measurement code, NULL when unused */
};

static int hrs3xxx_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	int ret;

	/* Same two-transfer sequence (write reg, then read) as stock. */
	ret = i2c_master_send(client, &reg, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	ret = i2c_master_recv(client, val, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	return 0;
}

static int hrs3xxx_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret != sizeof(buf))
		return ret < 0 ? ret : -EIO;

	return 0;
}

/* Must be called with data->lock held (or before the device is visible). */
static void hrs3xxx_power_off(struct hrs3xxx_data *data)
{
	int ret;

	if (!data->powered)
		return;

	/* Software standby first, then cut the supply if there is a switch. */
	ret = hrs3xxx_write_reg(data->client, HRS3XXX_REG_ENABLE, 0);
	if (ret)
		dev_warn_ratelimited(&data->client->dev,
				     "standby write failed: %d\n", ret);
	msleep(HRS3XXX_STANDBY_DELAY_MS);

	if (gpio_is_valid(data->power_gpio))
		gpio_set_value_cansleep(data->power_gpio, 0);

	data->powered = false;
}

/*
 * Note: on boards without a power switch (DW100U) power_off() leaves the chip
 * in software standby (reg 0x01 = 0) and power_on() does not wake it. A future
 * measurement back end must write the enable register itself.
 */
static void hrs3xxx_power_on(struct hrs3xxx_data *data)
{
	if (data->powered)
		return;

	if (gpio_is_valid(data->power_gpio))
		gpio_set_value_cansleep(data->power_gpio, 1);

	msleep(HRS3XXX_POWERUP_DELAY_MS);
	data->powered = true;
}

static void hrs3xxx_report(struct hrs3xxx_data *data)
{
	struct input_dev *input = data->input;

	/*
	 * No measurement back end yet: report "no valid sample". The input
	 * core drops EV_REL events with value 0, so userspace sees only
	 * HWHEEL = -1, MISC = -1 and SYN (same as stock).
	 */
	input_report_rel(input, REL_X, 0);	/* bpm */
	input_report_rel(input, REL_Y, 0);
	input_report_rel(input, REL_Z, 0);
	input_report_rel(input, REL_RX, 0);	/* SpO2 */
	input_report_rel(input, REL_RY, 0);
	input_report_rel(input, REL_RZ, 0);	/* wear state */
	input_report_rel(input, REL_HWHEEL, -1);
	input_report_rel(input, REL_MISC, -1);
	input_sync(input);
}

static void hrs3xxx_work(struct work_struct *work)
{
	struct hrs3xxx_data *data =
		container_of(to_delayed_work(work), struct hrs3xxx_data, work);

	/* hrs3xxx_set_mode() cancels this work without holding the lock. */
	mutex_lock(&data->lock);
	if (data->ready && data->mode) {
		hrs3xxx_power_on(data);	/* no-op unless resuming */
		if (data->blob) {
			/* Resume: the stock code reports from its own works. */
			hrsb_start(data->blob, data->mode);
		} else {
			hrs3xxx_report(data);
			schedule_delayed_work(&data->work,
				msecs_to_jiffies(HRS3XXX_REPORT_PERIOD_MS));
		}
	}
	mutex_unlock(&data->lock);
}

/* data->lock held. Starts measuring (or the 1 s dummy report without blob). */
static void hrs3xxx_meas_start(struct hrs3xxx_data *data)
{
	if (data->blob) {
		hrsb_start(data->blob, data->mode);
		return;
	}
	schedule_delayed_work(&data->work,
			      msecs_to_jiffies(HRS3XXX_REPORT_PERIOD_MS));
}

/*
 * data->lock held. Stops the stock measurement (timers, works, exit); the
 * delayed work of this driver is cancelled by the caller without the lock.
 */
static void hrs3xxx_meas_stop(struct hrs3xxx_data *data)
{
	if (data->blob)
		hrsb_stop(data->blob);
}

static void hrs3xxx_set_mode(struct hrs3xxx_data *data, unsigned int mode)
{
	/* Not under data->lock: the work item takes it. */
	cancel_delayed_work_sync(&data->work);

	mutex_lock(&data->lock);
	if (mode) {
		data->mode = mode;
		/* Before registration completes only remember the request. */
		if (data->ready) {
			hrs3xxx_power_on(data);
			hrs3xxx_meas_start(data);
		}
	} else {
		data->mode = 0;
		hrs3xxx_meas_stop(data);
		hrs3xxx_power_off(data);
	}
	mutex_unlock(&data->lock);
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct hrs3xxx_data *data = dev_get_drvdata(dev);
	unsigned int mode;

	mutex_lock(&data->lock);
	mode = data->mode;
	mutex_unlock(&data->lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", mode);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct hrs3xxx_data *data = dev_get_drvdata(dev);
	unsigned int mode;
	int ret;

	ret = kstrtouint(buf, 10, &mode);
	if (ret)
		return ret;
	if (mode > HRS3XXX_MODE_MAX)
		return -EINVAL;

	hrs3xxx_set_mode(data, mode);

	return count;
}

/* The HAL strcmp()s the content: no trailing newline, like stock. */
static ssize_t chipinfo_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct hrs3xxx_data *data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%s", data->chipinfo);
}

static DEVICE_ATTR(enable, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
		   enable_show, enable_store);
static DEVICE_ATTR_RO(chipinfo);

static struct attribute *hrs3xxx_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_chipinfo.attr,
	NULL,
};

static const struct attribute_group hrs3xxx_attr_group = {
	.attrs = hrs3xxx_attrs,
};

static const struct attribute_group *hrs3xxx_groups[] = {
	&hrs3xxx_attr_group,
	NULL,
};

static const char *hrs3xxx_chip_name(u8 id)
{
	if (IS_ENABLED(CONFIG_HEARTRATE_HRS3918) && id == HRS3XXX_ID_HRS3918)
		return "hrs3918";
	/*
	 * HRS3300: ENABLE (0x01) is the same register and 0 is also its
	 * disabled state, so the power sequencing here is shared. Only the
	 * ID is checked; no measurement path exists for either chip yet.
	 */
	if (IS_ENABLED(CONFIG_HEARTRATE_HRS3300) && id == HRS3XXX_ID_HRS3300)
		return "hrs3300";

	return NULL;
}

static int hrs3xxx_parse_dt(struct hrs3xxx_data *data)
{
	struct device *dev = &data->client->dev;
	int gpio;

	data->power_gpio = -ENOENT;

	if (!dev->of_node)
		return 0;

	gpio = of_get_named_gpio(dev->of_node, "power-gpio", 0);
	if (gpio == -EPROBE_DEFER)
		return gpio;
	if (!gpio_is_valid(gpio))
		return 0;	/* property is optional (DW100U has none) */

	data->power_gpio = gpio;
	return 0;
}

static int hrs3xxx_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct hrs3xxx_data *data;
	struct input_dev *input;
	u8 chip_id;
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
	INIT_DELAYED_WORK(&data->work, hrs3xxx_work);

	ret = hrs3xxx_parse_dt(data);
	if (ret)
		return ret;

	if (gpio_is_valid(data->power_gpio)) {
		ret = devm_gpio_request_one(dev, data->power_gpio,
					    GPIOF_OUT_INIT_HIGH,
					    "hrs3xxx-power-gpio");
		if (ret) {
			dev_err(dev, "cannot request power gpio %d: %d\n",
				data->power_gpio, ret);
			return ret;
		}
	}
	/* Without a power switch the supply is always on. */
	data->powered = true;

	msleep(HRS3XXX_ID_DELAY_MS);

	ret = hrs3xxx_read_reg(client, HRS3XXX_REG_ID, &chip_id);
	if (ret) {
		dev_err(dev, "cannot read chip id: %d\n", ret);
		goto err_off;
	}

	data->chipinfo = hrs3xxx_chip_name(chip_id);
	if (!data->chipinfo) {
		dev_err(dev, "unsupported chip id 0x%02x\n", chip_id);
		ret = -ENODEV;
		goto err_off;
	}
	dev_info(dev, "found %s (id 0x%02x)\n", data->chipinfo, chip_id);

	/* The sensor stays off until userspace enables it. */
	hrs3xxx_power_off(data);

	/*
	 * Stock registers this device without a parent so that it shows up
	 * under /devices/virtual/input; keep that layout.
	 */
	input = input_allocate_device();
	if (!input) {
		ret = -ENOMEM;
		goto err_off;
	}

	input->name = HRS3XXX_INPUT_NAME;
	input->id.bustype = BUS_HOST;
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);
	input_set_capability(input, EV_REL, REL_Z);
	input_set_capability(input, EV_REL, REL_RX);
	input_set_capability(input, EV_REL, REL_RY);
	input_set_capability(input, EV_REL, REL_RZ);
	input_set_capability(input, EV_REL, REL_HWHEEL);
	input_set_capability(input, EV_REL, REL_MISC);
	input_set_drvdata(input, data);
	/* Created with the device, before the KOBJ_ADD uevent. */
	input->dev.groups = hrs3xxx_groups;
	data->input = input;

	i2c_set_clientdata(client, data);

	if (IS_ENABLED(CONFIG_HRS3XXX_TYHX_BLOB)) {
		/* Starts "not live": nothing can start before probe is done. */
		data->blob = hrsb_create(client, input);
		if (IS_ERR(data->blob)) {
			ret = PTR_ERR(data->blob);
			data->blob = NULL;
			dev_err(dev, "cannot set up measurement code: %d\n", ret);
			input_free_device(input);
			goto err_off;
		}
	}

	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "cannot register input device: %d\n", ret);
		/* ready was never set, so no work was queued; be defensive. */
		cancel_delayed_work_sync(&data->work);
		hrsb_destroy(data->blob);
		data->blob = NULL;
		input_free_device(input);
		goto err_off;
	}

	/* Start anything requested through sysfs during registration. */
	mutex_lock(&data->lock);
	data->ready = true;
	if (data->blob)
		hrsb_set_live(data->blob, true);
	if (data->mode) {
		hrs3xxx_power_on(data);
		hrs3xxx_meas_start(data);
	}
	mutex_unlock(&data->lock);

	return 0;

err_off:
	hrs3xxx_power_off(data);
	return ret;
}

static int hrs3xxx_remove(struct i2c_client *client)
{
	struct hrs3xxx_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->lock);
	data->ready = false;
	if (data->blob)
		hrsb_set_live(data->blob, false);	/* no start from now on */
	mutex_unlock(&data->lock);

	/* Keep the input device alive until the work item is stopped. */
	input_get_device(data->input);

	/*
	 * Removes the attributes and waits for in-flight stores, so nothing
	 * can queue the work again once this returns.
	 */
	input_unregister_device(data->input);
	cancel_delayed_work_sync(&data->work);

	mutex_lock(&data->lock);
	data->mode = 0;
	hrs3xxx_meas_stop(data);	/* timers, works, exit; input still held */
	hrs3xxx_power_off(data);
	mutex_unlock(&data->lock);

	hrsb_destroy(data->blob);
	data->blob = NULL;
	input_put_device(data->input);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int hrs3xxx_suspend(struct device *dev)
{
	struct hrs3xxx_data *data = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&data->work);

	if (data->blob) {
		/*
		 * An enable/disable in progress holds the lock: refuse to
		 * suspend, restoring the resume work cancelled above (it
		 * restarts the sensor if it is still wanted).
		 */
		if (!mutex_trylock(&data->lock)) {
			schedule_delayed_work(&data->work, 0);
			return -EBUSY;
		}
	} else {
		mutex_lock(&data->lock);
	}
	hrs3xxx_meas_stop(data);
	hrs3xxx_power_off(data);	/* mode is kept for resume */
	mutex_unlock(&data->lock);

	return 0;
}

static int hrs3xxx_resume(struct device *dev)
{
	struct hrs3xxx_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->lock);
	/* No sleeping here: the work item powers the chip back on. */
	if (data->ready && data->mode)
		schedule_delayed_work(&data->work, 0);
	mutex_unlock(&data->lock);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(hrs3xxx_pm_ops, hrs3xxx_suspend, hrs3xxx_resume);

static const struct i2c_device_id hrs3xxx_id[] = {
	{ HRS3XXX_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, hrs3xxx_id);

static const struct of_device_id hrs3xxx_of_match[] = {
	{ .compatible = "tyhx,hrs3xxx" },
	{ }
};
MODULE_DEVICE_TABLE(of, hrs3xxx_of_match);

static struct i2c_driver hrs3xxx_driver = {
	.driver = {
		.name = HRS3XXX_DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = hrs3xxx_of_match,
		.pm = &hrs3xxx_pm_ops,
	},
	.probe = hrs3xxx_probe,
	.remove = hrs3xxx_remove,
	.id_table = hrs3xxx_id,
};
module_i2c_driver(hrs3xxx_driver);

MODULE_DESCRIPTION("TYHX HRS3xxx heart-rate sensor driver");
MODULE_LICENSE("GPL");
