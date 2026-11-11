// SPDX-License-Identifier: GPL-2.0
/*
 * Android timed_output class
 *
 * Copyright (C) 2009 Google, Inc.
 * Author: Mike Lockwood <lockwood@android.com>
 */

#define pr_fmt(fmt) "timed_output: " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/timed_output.h>

static struct class *timed_output_class;
static DEFINE_MUTEX(timed_output_lock);
static atomic_t device_count = ATOMIC_INIT(0);

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct timed_output_dev *tdev = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", tdev->get_time(tdev));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct timed_output_dev *tdev = dev_get_drvdata(dev);
	int value;
	int rc;

	rc = kstrtoint(buf, 0, &value);
	if (rc)
		return rc;

	tdev->enable(tdev, value);

	return size;
}
static DEVICE_ATTR_RW(enable);

static struct attribute *timed_output_attrs[] = {
	&dev_attr_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(timed_output);

/* Drivers may register before this file's initcall has run. */
static int create_timed_output_class(void)
{
	struct class *cls;
	int ret = 0;

	mutex_lock(&timed_output_lock);
	if (!timed_output_class) {
		cls = class_create(THIS_MODULE, "timed_output");
		if (IS_ERR(cls)) {
			ret = PTR_ERR(cls);
		} else {
			cls->dev_groups = timed_output_groups;
			timed_output_class = cls;
		}
	}
	mutex_unlock(&timed_output_lock);

	return ret;
}

int timed_output_dev_register(struct timed_output_dev *tdev)
{
	int ret;

	if (!tdev || !tdev->name || !tdev->enable || !tdev->get_time)
		return -EINVAL;

	ret = create_timed_output_class();
	if (ret < 0)
		return ret;

	tdev->index = atomic_inc_return(&device_count);
	tdev->dev = device_create(timed_output_class, NULL,
				  MKDEV(0, tdev->index), tdev, "%s",
				  tdev->name);
	if (IS_ERR(tdev->dev)) {
		ret = PTR_ERR(tdev->dev);
		tdev->dev = NULL;
		return ret;
	}

	tdev->state = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(timed_output_dev_register);

/*
 * Removes the sysfs node (waiting for in-flight enable/get_time calls). The
 * output is not touched: the driver turns it off after this returns, when no
 * new request can arrive any more.
 */
void timed_output_dev_unregister(struct timed_output_dev *tdev)
{
	if (!tdev->dev)
		return;

	device_destroy(timed_output_class, MKDEV(0, tdev->index));
	tdev->dev = NULL;
}
EXPORT_SYMBOL_GPL(timed_output_dev_unregister);

static int __init timed_output_init(void)
{
	return create_timed_output_class();
}
device_initcall(timed_output_init);
