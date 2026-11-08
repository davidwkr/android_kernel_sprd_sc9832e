// SPDX-License-Identifier: GPL-2.0
/*
 * Orient-Chip OCH1970 3-axis Hall sensor used as an open/close/front/back
 * key source (input device).
 *
 * Behaviour follows the stock vendor driver (see devices/OCH1970_INTERFACE.md
 * for the evidence). The stock driver exists in three generations, told apart
 * by the device tree:
 *   "open-bop" (string) present       K18 generation  (gen3)
 *   "open-threshold" (u32 pair)       MT108/DW100U    (gen2)
 *   neither                           DM82/MT107      (gen1), fixed axis and
 *                                     thresholds; "och1970,variant" selects
 *                                     "dm82" (default) or "mt107"
 *
 * Output: input device "magnetic", EV_KEY (linux,open/close/back/front, each
 * a press+release pulse) and, while polling is enabled, EV_REL X/Y/Z.
 * The chip is interrupt driven (irq-gpio); the thresholds are evaluated in
 * software on the sample read from the interrupt.
 *
 * sysfs (on the input device): enable, wakeup, chipinfo, threshold{x,y,z}{1,2}
 * misc device /dev/och1970: write "open" / "close".
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define OCH1970_DRV_NAME	"och1970"
#define OCH1970_INPUT_NAME	"magnetic"

/* Registers */
#define OCH1970_REG_WIA		0x00	/* 2 bytes: company 0x48, device 0xc0 */
#define OCH1970_REG_ST		0x10	/* 2 bytes, bit 0 of the second: data ready */
#define OCH1970_REG_DATA	0x17	/* 8 bytes; Z, Y, X at +2, +4, +6, big endian */
#define OCH1970_REG_CNTL1	0x20	/* read/written as a 2 byte block with CNTL2 */
#define OCH1970_REG_CNTL2	0x21
#define OCH1970_REG_THR_BASE	0x22	/* x1 x2 y1 y2 z1 z2: BOP, BRP (2 x s16 BE) */

#define OCH1970_WIA_COMPANY	0x48
#define OCH1970_WIA_DEVICE	0xc0

#define OCH1970_MODE_MASK	0x0f
#define OCH1970_MODE_KEEP	0x0a	/* measurement mode kept while wake armed */
#define OCH1970_MODE_OPEN	0x0e	/* sysfs enable=4, misc "open" */

/*
 * Per-axis interrupt enable bits, in the second byte of the CNTL1 block as
 * the stock driver uses them. They share CNTL2 with the MODE field; the
 * stock sequence is reproduced as is.
 */
#define OCH1970_EN_X1		0x02
#define OCH1970_EN_X2		0x04
#define OCH1970_EN_Y1		0x08
#define OCH1970_EN_Y2		0x10
#define OCH1970_EN_Z1		0x20
#define OCH1970_EN_Z2		0x40

#define OCH1970_POLL_MS		100
#define OCH1970_WAKE_JIFFIES	500	/* stock: jiffies_to_msecs(500) */
#define OCH1970_DEF_DEBOUNCE_MS	5

enum och1970_gen {
	OCH1970_GEN1_DM82,
	OCH1970_GEN1_MT107,
	OCH1970_GEN2,
	OCH1970_GEN3,
};

struct och1970_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct miscdevice misc;
	struct wakeup_source *ws;
	struct mutex lock;		/* flags, thresholds, register sequences */
	struct delayed_work poll_work;
	struct delayed_work irq_work;
	enum och1970_gen gen;
	int irq_gpio;
	int rst_gpio;
	int irq;
	unsigned int irq_flags;
	unsigned int debounce_ms;
	unsigned int enable_val;

	s16 x, y, z;
	u32 key_open, key_close, key_back, key_front;
	s16 open_bop, open_brp, rot_bop, rot_brp;
	s16 control_data;
	int open_axis;			/* 0..2 = x, y, z; open = level 1 */
	int rot_axis;			/* level 2 */
	int ctl_axis;
	bool wake;			/* stock +496: wakeup / key enable */
	bool state;			/* stock +497 */
	bool extra_back;		/* stock +498 (gen3 only) */
};

static const u8 och1970_axis_en[2][3] = {
	{ OCH1970_EN_X1, OCH1970_EN_Y1, OCH1970_EN_Z1 },
	{ OCH1970_EN_X2, OCH1970_EN_Y2, OCH1970_EN_Z2 },
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

static int och1970_write(struct i2c_client *client, u8 reg, const u8 *val,
			 int len)
{
	u8 buf[5];
	int ret;

	if (len > 4)
		return -EINVAL;
	buf[0] = reg;
	memcpy(buf + 1, val, len);
	ret = i2c_master_send(client, buf, len + 1);
	if (ret < 0)
		return ret;
	return ret == len + 1 ? 0 : -EIO;
}

/* CNTL2 read-modify-write, one register */
static int och1970_rmw_cntl2(struct och1970_data *data, u8 clear, u8 set)
{
	u8 v;
	int ret;

	ret = och1970_read(data->client, OCH1970_REG_CNTL2, &v, 1);
	if (ret)
		return ret;
	v = (v & ~clear) | set;
	return och1970_write(data->client, OCH1970_REG_CNTL2, &v, 1);
}

static int och1970_measurement_mode(struct och1970_data *data, u8 mode)
{
	return och1970_rmw_cntl2(data, OCH1970_MODE_MASK, mode);
}

static int och1970_powerdown_mode(struct och1970_data *data)
{
	return och1970_rmw_cntl2(data, OCH1970_MODE_MASK, 0);
}

/* read-modify-write of the 2 byte block at CNTL1; idx selects the byte */
static int och1970_rmw_block(struct och1970_data *data, int idx, u8 clear,
			     u8 set)
{
	u8 b[2];
	int ret;

	ret = och1970_read(data->client, OCH1970_REG_CNTL1, b, 2);
	if (ret)
		return ret;
	b[idx] = (b[idx] & ~clear) | set;
	return och1970_write(data->client, OCH1970_REG_CNTL1, b, 2);
}

static int och1970_axis_enable(struct och1970_data *data, int level, int axis,
			       bool on)
{
	u8 bit;

	if (axis < 0)
		return 0;
	bit = och1970_axis_en[level - 1][axis];
	return och1970_rmw_block(data, 1, on ? 0 : bit, on ? bit : 0);
}

static int och1970_thr_reg(int level, int axis)
{
	return OCH1970_REG_THR_BASE + axis * 2 + (level - 1);
}

static int och1970_write_thr(struct och1970_data *data, int level, int axis,
			     s16 bop, s16 brp)
{
	u8 b[4] = { (u16)bop >> 8, (u16)bop & 0xff, (u16)brp >> 8,
		    (u16)brp & 0xff };

	if (axis < 0)
		return 0;
	return och1970_write(data->client, och1970_thr_reg(level, axis), b, 4);
}

/* stock och1970_get_xyz_data: no data-ready check, chip runs continuously */
static int och1970_get_xyz(struct och1970_data *data)
{
	u8 b[8];
	int ret;

	ret = och1970_read(data->client, OCH1970_REG_DATA, b, sizeof(b));
	if (ret)
		return ret;
	data->x = (s16)((b[6] << 8) | b[7]);
	data->y = (s16)((b[4] << 8) | b[5]);
	data->z = (s16)((b[2] << 8) | b[3]);
	return 0;
}

static s16 och1970_val(struct och1970_data *data, int axis)
{
	switch (axis) {
	case 0:
		return data->x;
	case 1:
		return data->y;
	case 2:
		return data->z;
	}
	return 0;
}

/* key press + release + sync, as the stock work function */
static void och1970_pulse(struct och1970_data *data, u32 code)
{
	input_report_key(data->input, code, 1);
	input_report_key(data->input, code, 0);
	input_sync(data->input);
}

static void och1970_front_back(struct och1970_data *data, s16 ctl)
{
	data->state = true;
	och1970_pulse(data, data->control_data <= ctl ? data->key_back :
							data->key_front);
}

/* gen3 (K18), stock och1970_irq_work_fun */
static void och1970_eval_gen3(struct och1970_data *data)
{
	s16 o = och1970_val(data, data->open_axis);
	s16 r = och1970_val(data, data->rot_axis);

	if (o > data->open_bop && r > data->rot_bop) {
		och1970_front_back(data, och1970_val(data, data->ctl_axis));
	} else if (o > data->open_bop && r < data->rot_brp) {
		och1970_axis_enable(data, 2, data->rot_axis, true);
		if (data->wake && data->state) {
			data->state = false;
			och1970_pulse(data, data->key_open);
		}
	} else if (o < data->open_brp) {
		och1970_axis_enable(data, 2, data->rot_axis, false);
		if (data->wake && !data->state) {
			data->state = true;
			och1970_pulse(data, data->key_close);
			if (data->extra_back)
				och1970_pulse(data, data->key_back);
		}
	}
}

/* gen2 (MT108, DW100U): the sign of open_bop selects the polarity */
static void och1970_eval_gen2(struct och1970_data *data)
{
	s16 o = och1970_val(data, data->open_axis);
	s16 r = och1970_val(data, data->rot_axis);

	if (data->open_bop < 0) {
		if (r > data->rot_bop) {
			och1970_front_back(data, och1970_val(data, data->ctl_axis));
		} else if (o > data->open_bop) {
			if (data->state) {
				data->state = false;
				och1970_pulse(data, data->key_back);
			} else if (data->wake) {
				och1970_pulse(data, data->key_open);
			}
		} else if (o < data->open_brp && data->wake) {
			och1970_pulse(data, data->key_close);
		}
	} else {
		if (r < data->rot_brp) {
			och1970_front_back(data, och1970_val(data, data->ctl_axis));
		} else if (o < data->open_brp) {
			if (data->state) {
				data->state = false;
				och1970_pulse(data, data->key_front);
			} else if (data->wake) {
				och1970_pulse(data, data->key_open);
			}
		} else if (o > data->open_bop && data->wake) {
			och1970_pulse(data, data->key_close);
		}
	}
}

/* gen1 (DM82: X axis; MT107: Y axis), z picks back/front */
static void och1970_eval_gen1(struct och1970_data *data)
{
	bool dm82 = data->gen == OCH1970_GEN1_DM82;
	s16 v = dm82 ? data->x : data->y;
	bool a, b, c;

	if (dm82) {
		a = v > data->rot_bop;
		b = v > data->open_brp && v < data->rot_brp;
		c = v < data->open_bop && v < data->rot_brp;
	} else {
		a = v < data->rot_brp;
		b = v > data->rot_bop && v < data->open_brp;
		c = v > data->open_bop && v > data->rot_bop;
	}

	if (a) {
		data->state = true;
		och1970_pulse(data, data->z >= 0 ? data->key_back :
						    data->key_front);
	} else if (b) {
		if (data->state) {
			data->state = false;
			och1970_pulse(data, data->key_back);
		} else if (data->wake) {
			och1970_pulse(data, data->key_close);
		}
	} else if (c && data->wake) {
		och1970_pulse(data, data->key_open);
	}
}

static void och1970_irq_work(struct work_struct *work)
{
	struct och1970_data *data =
		container_of(work, struct och1970_data, irq_work.work);

	mutex_lock(&data->lock);
	if (device_may_wakeup(&data->client->dev))
		pm_relax(&data->client->dev);
	switch (data->gen) {
	case OCH1970_GEN3:
		och1970_eval_gen3(data);
		break;
	case OCH1970_GEN2:
		och1970_eval_gen2(data);
		break;
	default:
		och1970_eval_gen1(data);
		break;
	}
	enable_irq(data->irq);
	mutex_unlock(&data->lock);
}

/* stock och1970_handle_fun: threaded, level or edge, oneshot */
static irqreturn_t och1970_irq_thread(int irq, void *dev_id)
{
	struct och1970_data *data = dev_id;
	unsigned int delay = data->gen <= OCH1970_GEN1_MT107 ?
			     data->debounce_ms : 0;

	disable_irq_nosync(irq);
	__pm_wakeup_event(data->ws, jiffies_to_msecs(OCH1970_WAKE_JIFFIES));
	if (device_may_wakeup(&data->client->dev))
		pm_stay_awake(&data->client->dev);
	och1970_get_xyz(data);
	schedule_delayed_work(&data->irq_work, msecs_to_jiffies(delay));

	return IRQ_HANDLED;
}

/* polling mode (sysfs enable = 2 or 3): EV_REL X/Y/Z */
static void och1970_poll_work(struct work_struct *work)
{
	struct och1970_data *data =
		container_of(work, struct och1970_data, poll_work.work);
	u8 st[2];

	mutex_lock(&data->lock);
	if (!och1970_read(data->client, OCH1970_REG_ST, st, 2) &&
	    (st[1] & 1) && !och1970_get_xyz(data)) {
		input_report_rel(data->input, REL_X, data->x);
		input_report_rel(data->input, REL_Y, data->y);
		input_report_rel(data->input, REL_Z, data->z);
		input_sync(data->input);
	}
	mutex_unlock(&data->lock);
	schedule_delayed_work(&data->poll_work,
			      msecs_to_jiffies(OCH1970_POLL_MS));
}

/* gen2 only: write the DT thresholds and arm the rotate axis (stock set_axis_threshold) */
static void och1970_gen2_thresholds(struct och1970_data *data)
{
	och1970_write_thr(data, 1, data->open_axis, data->open_bop,
			  data->open_brp);
	och1970_axis_enable(data, 2, data->rot_axis, true);
	och1970_write_thr(data, 2, data->rot_axis, data->rot_bop,
			  data->rot_brp);
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct och1970_data *data = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%u\n", data->enable_val);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct och1970_data *data = dev_get_drvdata(dev->parent);
	struct device *cdev = &data->client->dev;
	unsigned int val;
	bool stop_poll = false;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&data->lock);
	data->enable_val = val;
	switch (val) {
	case 1:
		data->wake = true;
		device_init_wakeup(cdev, true);
		och1970_axis_enable(data, 1, data->open_axis, true);
		if (data->gen == OCH1970_GEN2)
			och1970_gen2_thresholds(data);
		break;
	case 2:
	case 3:
		if (val == 3)
			och1970_axis_enable(data, 1, data->open_axis, true);
		schedule_delayed_work(&data->poll_work,
				      msecs_to_jiffies(OCH1970_POLL_MS));
		break;
	case 4:
		data->extra_back = false;
		och1970_measurement_mode(data, OCH1970_MODE_OPEN);
		break;
	case 5:
		data->extra_back = true;
		if (data->wake)
			och1970_measurement_mode(data, OCH1970_MODE_KEEP);
		else
			och1970_powerdown_mode(data);
		break;
	default:
		data->wake = false;
		device_init_wakeup(cdev, false);
		och1970_axis_enable(data, 1, data->open_axis, false);
		och1970_powerdown_mode(data);
		stop_poll = true;
		break;
	}
	mutex_unlock(&data->lock);

	if (stop_poll)
		cancel_delayed_work_sync(&data->poll_work);

	return count;
}

static ssize_t wakeup_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct och1970_data *data = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%s\n", data->wake ? "true" : "false");
}

static ssize_t wakeup_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct och1970_data *data = dev_get_drvdata(dev->parent);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;
	data->wake = val == 1;
	device_init_wakeup(&data->client->dev, data->wake);

	return count;
}

static ssize_t chipinfo_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "och1970");
}

static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(wakeup);
static DEVICE_ATTR_RO(chipinfo);

/* threshold{x,y,z}{1,2}: hardware registers only, as the stock attributes */
static ssize_t thr_show(struct device *dev, int level, int axis, char *buf)
{
	struct och1970_data *data = dev_get_drvdata(dev->parent);
	u8 b[4];
	int ret;

	mutex_lock(&data->lock);
	ret = och1970_read(data->client, och1970_thr_reg(level, axis), b, 4);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "BOP:%d\nBRP:%d\n",
			 (s16)((b[0] << 8) | b[1]), (s16)((b[2] << 8) | b[3]));
}

static ssize_t thr_store(struct device *dev, int level, int axis,
			 const char *buf, size_t count)
{
	struct och1970_data *data = dev_get_drvdata(dev->parent);
	int bop, brp, ret = 0;

	if (sscanf(buf, "%d %d", &bop, &brp) != 2)
		return -EINVAL;
	/* stock ignores 0 and 0xffff */
	if ((u16)bop == 0 || (u16)bop == 0xffff || (u16)brp == 0 ||
	    (u16)brp == 0xffff)
		return count;

	mutex_lock(&data->lock);
	ret = och1970_write_thr(data, level, axis, bop, brp);
	mutex_unlock(&data->lock);

	return ret ? ret : (ssize_t)count;
}

#define OCH1970_THR_ATTR(name, level, axis)				\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *buf)	\
{									\
	return thr_show(dev, level, axis, buf);				\
}									\
static ssize_t name##_store(struct device *dev,				\
			    struct device_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	return thr_store(dev, level, axis, buf, count);			\
}									\
static DEVICE_ATTR_RW(name)

OCH1970_THR_ATTR(thresholdx1, 1, 0);
OCH1970_THR_ATTR(thresholdx2, 2, 0);
OCH1970_THR_ATTR(thresholdy1, 1, 1);
OCH1970_THR_ATTR(thresholdy2, 2, 1);
OCH1970_THR_ATTR(thresholdz1, 1, 2);
OCH1970_THR_ATTR(thresholdz2, 2, 2);

static struct attribute *och1970_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_wakeup.attr,
	&dev_attr_chipinfo.attr,
	&dev_attr_thresholdx1.attr,
	&dev_attr_thresholdx2.attr,
	&dev_attr_thresholdy1.attr,
	&dev_attr_thresholdy2.attr,
	&dev_attr_thresholdz1.attr,
	&dev_attr_thresholdz2.attr,
	NULL
};

static const struct attribute_group och1970_attr_group = {
	.attrs = och1970_attrs,
};

/* /dev/och1970: write "open" or "close" */
static ssize_t och1970_misc_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct miscdevice *misc = file->private_data;
	struct och1970_data *data = container_of(misc, struct och1970_data, misc);
	char cmd[16] = { };

	if (count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	strim(cmd);

	mutex_lock(&data->lock);
	if (!strcmp(cmd, "open")) {
		och1970_axis_enable(data, 2, data->rot_axis, true);
		och1970_measurement_mode(data, OCH1970_MODE_OPEN);
	} else if (!strcmp(cmd, "close")) {
		och1970_axis_enable(data, 2, data->rot_axis, false);
		if (data->wake)
			och1970_measurement_mode(data, OCH1970_MODE_KEEP);
		else
			och1970_powerdown_mode(data);
	}
	mutex_unlock(&data->lock);

	return count;
}

static const struct file_operations och1970_misc_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.write = och1970_misc_write,
	.llseek = no_llseek,
};

static int och1970_axis_idx(const char *s, char level)
{
	if (!s || s[0] < 'x' || s[0] > 'z')
		return -EINVAL;
	if (level) {
		if (s[1] != level || s[2])
			return -EINVAL;
	} else if (s[1]) {
		return -EINVAL;
	}
	return s[0] - 'x';
}

static int och1970_parse_axis(struct device_node *np, const char *prop,
			      char level, int *axis)
{
	const char *s;
	int ret;

	ret = of_property_read_string(np, prop, &s);
	if (ret)
		return ret;
	ret = och1970_axis_idx(s, level);
	if (ret < 0)
		return ret;
	*axis = ret;
	return 0;
}

static int och1970_parse_s16_str(struct device_node *np, const char *prop,
				 s16 *val)
{
	const char *s;
	int v, ret;

	ret = of_property_read_string(np, prop, &s);
	if (ret)
		return ret;
	ret = kstrtoint(s, 10, &v);
	if (ret)
		return ret;
	*val = v;
	return 0;
}

static int och1970_parse_thr_pair(struct device_node *np, const char *prop,
				  s16 *bop, s16 *brp)
{
	u32 v[2];
	int ret;

	ret = of_property_read_u32_array(np, prop, v, 2);
	if (ret)
		return ret;
	*bop = v[0];
	*brp = v[1];
	return 0;
}

static int och1970_parse_dt(struct och1970_data *data)
{
	struct device *dev = &data->client->dev;
	struct device_node *np = dev->of_node;
	const char *s;
	u32 v;
	int ret;

	if (!np)
		return -EINVAL;

	if (of_find_property(np, "open-bop", NULL))
		data->gen = OCH1970_GEN3;
	else if (of_find_property(np, "open-threshold", NULL))
		data->gen = OCH1970_GEN2;
	else if (!of_property_read_string(np, "och1970,variant", &s) &&
		 !strcmp(s, "mt107"))
		data->gen = OCH1970_GEN1_MT107;
	else
		data->gen = OCH1970_GEN1_DM82;

	/* default key codes differ between the generations (stock) */
	if (data->gen <= OCH1970_GEN1_MT107) {
		data->key_open = KEY_F1;
		data->key_close = KEY_F8;
		data->key_back = KEY_F3;
		data->key_front = KEY_F2;
	} else {
		data->key_open = KEY_F8;
		data->key_close = KEY_F1;
		data->key_back = KEY_F2;
		data->key_front = KEY_F3;
	}
	of_property_read_u32(np, "linux,open", &data->key_open);
	of_property_read_u32(np, "linux,close", &data->key_close);
	of_property_read_u32(np, "linux,back", &data->key_back);
	of_property_read_u32(np, "linux,front", &data->key_front);

	data->debounce_ms = OCH1970_DEF_DEBOUNCE_MS;
	of_property_read_u32(np, "debounce-interval", &data->debounce_ms);

	switch (data->gen) {
	case OCH1970_GEN1_DM82:
		data->open_axis = 0;
		data->rot_axis = 0;
		data->open_bop = -4000;
		data->open_brp = -6000;
		data->rot_bop = -100;
		data->rot_brp = -280;
		data->irq_flags = IRQF_TRIGGER_FALLING;
		break;
	case OCH1970_GEN1_MT107:
		data->open_axis = 1;
		data->rot_axis = 1;
		data->open_bop = 4500;
		data->open_brp = 4000;
		data->rot_bop = 180;
		data->rot_brp = 50;
		data->irq_flags = IRQF_TRIGGER_LOW;
		break;
	default:
		data->irq_flags = IRQF_TRIGGER_LOW;
		ret = och1970_parse_axis(np, "control-axis", 0, &data->ctl_axis);
		if (ret)
			return ret;
		if (!of_property_read_u32(np, "control-data", &v))
			data->control_data = v;
		ret = och1970_parse_axis(np, "open-axis", '1', &data->open_axis);
		if (ret)
			return ret;
		ret = och1970_parse_axis(np, "rotate-axis", '2', &data->rot_axis);
		if (ret)
			return ret;
		if (data->gen == OCH1970_GEN3) {
			ret = och1970_parse_s16_str(np, "open-bop", &data->open_bop);
			if (!ret)
				ret = och1970_parse_s16_str(np, "open-brp", &data->open_brp);
			if (!ret)
				ret = och1970_parse_s16_str(np, "rotate-bop", &data->rot_bop);
			if (!ret)
				ret = och1970_parse_s16_str(np, "rotate-brp", &data->rot_brp);
		} else {
			ret = och1970_parse_thr_pair(np, "open-threshold",
						     &data->open_bop, &data->open_brp);
			if (!ret)
				ret = och1970_parse_thr_pair(np, "rotate-threshold",
							     &data->rot_bop, &data->rot_brp);
		}
		if (ret)
			return ret;
		break;
	}

	return 0;
}

static int och1970_hw_reset(struct och1970_data *data)
{
	if (!gpio_is_valid(data->rst_gpio))
		return 0;

	/* stock: high 20 ms, low 20 ms, high, 200 ms */
	gpio_set_value_cansleep(data->rst_gpio, 1);
	msleep(20);
	gpio_set_value_cansleep(data->rst_gpio, 0);
	msleep(20);
	gpio_set_value_cansleep(data->rst_gpio, 1);
	msleep(200);

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

/* stock probe-time register sequence, per generation */
static int och1970_hw_init(struct och1970_data *data)
{
	int ret;

	ret = och1970_rmw_cntl2(data, 0x0f, 0);
	if (!ret)
		ret = och1970_rmw_cntl2(data, 0, 0x0a);
	if (!ret)
		ret = och1970_rmw_cntl2(data, 0x10, 0);
	if (!ret)
		ret = och1970_rmw_cntl2(data, 0, 0x20);
	if (ret)
		return ret;

	switch (data->gen) {
	case OCH1970_GEN1_DM82:
		och1970_write_thr(data, 1, 0, data->open_bop, data->open_brp);
		och1970_write_thr(data, 2, 0, data->rot_bop, data->rot_brp);
		och1970_axis_enable(data, 1, 0, false);
		och1970_rmw_block(data, 1, 0, OCH1970_EN_X2);
		och1970_rmw_block(data, 1, OCH1970_EN_Y1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Y2, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Z1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Z2, 0);
		break;
	case OCH1970_GEN1_MT107:
		och1970_write_thr(data, 1, 1, data->open_bop, data->open_brp);
		och1970_write_thr(data, 2, 1, data->rot_bop, data->rot_brp);
		och1970_rmw_block(data, 1, OCH1970_EN_X1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_X2, 0);
		och1970_axis_enable(data, 1, 1, false);
		och1970_rmw_block(data, 1, 0, OCH1970_EN_Y2);
		och1970_rmw_block(data, 1, OCH1970_EN_Z1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Z2, 0);
		break;
	default:
		och1970_rmw_block(data, 1, OCH1970_EN_X1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_X2, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Y1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Y2, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Z1, 0);
		och1970_rmw_block(data, 1, OCH1970_EN_Z2, 0);
		break;
	}
	och1970_rmw_block(data, 0, 0, 0x04);
	och1970_rmw_block(data, 1, 0x01, 0);

	if (data->gen == OCH1970_GEN3) {
		och1970_write_thr(data, 1, data->open_axis, data->open_bop,
				  data->open_brp);
		och1970_write_thr(data, 2, data->rot_axis, data->rot_bop,
				  data->rot_brp);
	}

	return 0;
}

static int och1970_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct och1970_data *data;
	struct input_dev *input;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->wake = true;		/* stock initial flags are all set */
	data->state = true;
	data->extra_back = true;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->poll_work, och1970_poll_work);
	INIT_DELAYED_WORK(&data->irq_work, och1970_irq_work);
	i2c_set_clientdata(client, data);

	ret = och1970_parse_dt(data);
	if (ret) {
		dev_err(dev, "bad device tree: %d\n", ret);
		return ret;
	}

	data->irq_gpio = of_get_named_gpio(dev->of_node, "irq-gpio", 0);
	if (!gpio_is_valid(data->irq_gpio))
		return data->irq_gpio < 0 ? data->irq_gpio : -EINVAL;
	data->rst_gpio = of_get_named_gpio(dev->of_node, "rst-gpio", 0);

	/* validate the chip before touching rst-gpio or writing anything */
	ret = och1970_check_id(client);
	if (ret)
		return ret;

	ret = devm_gpio_request_one(dev, data->irq_gpio, GPIOF_IN,
				    "och1970-irq");
	if (ret)
		return ret;
	if (gpio_is_valid(data->rst_gpio)) {
		ret = devm_gpio_request_one(dev, data->rst_gpio,
					    GPIOF_OUT_INIT_LOW, "och1970-rst");
		if (ret)
			return ret;
	}
	data->irq = gpio_to_irq(data->irq_gpio);
	if (data->irq < 0)
		return data->irq;

	och1970_hw_reset(data);
	ret = och1970_check_id(client);
	if (ret)
		return ret;
	ret = och1970_hw_init(data);
	if (ret)
		return ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	data->input = input;
	input->name = OCH1970_INPUT_NAME;
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;
	input_set_drvdata(input, data);
	input_set_capability(input, EV_KEY, KEY_F1);
	input_set_capability(input, EV_KEY, KEY_F2);
	input_set_capability(input, EV_KEY, KEY_F3);
	input_set_capability(input, EV_KEY, KEY_F8);
	input_set_capability(input, EV_KEY, data->key_open);
	input_set_capability(input, EV_KEY, data->key_close);
	input_set_capability(input, EV_KEY, data->key_back);
	input_set_capability(input, EV_KEY, data->key_front);
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);
	input_set_capability(input, EV_REL, REL_Z);
	input_set_capability(input, EV_REL, REL_MISC);
	ret = input_register_device(input);
	if (ret)
		return ret;

	data->ws = wakeup_source_register("och1970-time");
	if (!data->ws)
		return -ENOMEM;
	device_init_wakeup(dev, data->wake);

	ret = request_threaded_irq(data->irq, NULL, och1970_irq_thread,
				   data->irq_flags | IRQF_ONESHOT,
				   "och1970-irq", data);
	if (ret)
		goto err_ws;

	data->misc.minor = MISC_DYNAMIC_MINOR;
	data->misc.name = OCH1970_DRV_NAME;
	data->misc.fops = &och1970_misc_fops;
	ret = misc_register(&data->misc);
	if (ret)
		goto err_irq;

	ret = sysfs_create_group(&input->dev.kobj, &och1970_attr_group);
	if (ret)
		goto err_misc;

	dev_info(dev, "OCH1970 hall sensor, generation %d\n",
		 data->gen == OCH1970_GEN3 ? 3 : data->gen == OCH1970_GEN2 ? 2 : 1);
	return 0;

err_misc:
	misc_deregister(&data->misc);
err_irq:
	free_irq(data->irq, data);
err_ws:
	wakeup_source_unregister(data->ws);
	return ret;
}

static int och1970_remove(struct i2c_client *client)
{
	struct och1970_data *data = i2c_get_clientdata(client);

	sysfs_remove_group(&data->input->dev.kobj, &och1970_attr_group);
	misc_deregister(&data->misc);
	free_irq(data->irq, data);
	cancel_delayed_work_sync(&data->irq_work);
	cancel_delayed_work_sync(&data->poll_work);
	wakeup_source_unregister(data->ws);
	och1970_powerdown_mode(data);

	return 0;
}

static void och1970_shutdown(struct i2c_client *client)
{
	struct och1970_data *data = i2c_get_clientdata(client);

	if (data)
		och1970_powerdown_mode(data);
}

#ifdef CONFIG_PM_SLEEP
static int och1970_suspend(struct device *dev)
{
	struct och1970_data *data = dev_get_drvdata(dev);

	if (!device_may_wakeup(dev)) {
		disable_irq(data->irq);
		return och1970_powerdown_mode(data);
	}
	och1970_measurement_mode(data, OCH1970_MODE_KEEP);
	if (data->gen == OCH1970_GEN1_DM82)
		irq_set_irq_type(data->irq, IRQ_TYPE_LEVEL_LOW);
	return irq_set_irq_wake(data->irq, 1);
}

static int och1970_resume(struct device *dev)
{
	struct och1970_data *data = dev_get_drvdata(dev);

	if (!device_may_wakeup(dev)) {
		och1970_powerdown_mode(data);
		enable_irq(data->irq);
		return 0;
	}
	och1970_measurement_mode(data, OCH1970_MODE_KEEP);
	if (data->gen == OCH1970_GEN1_DM82)
		irq_set_irq_type(data->irq, IRQ_TYPE_EDGE_FALLING);
	return irq_set_irq_wake(data->irq, 0);
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

MODULE_DESCRIPTION("Orient-Chip OCH1970 Hall sensor (open/close/front/back keys)");
MODULE_LICENSE("GPL v2");
