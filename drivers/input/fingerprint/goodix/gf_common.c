/* Goodix's GF316M/GF318M/GF3118M/GF518M/GF5118M/GF516M/GF816M/GF3208/GF5206
 *  fingerprint sensor linux driver for TEE
 *
 * 2010 - 2015 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Provenance: derived from drivers/input/fingerprint/gf3626/gf_common.c of
 * https://github.com/Supernote-Ratta/kernel_a6x_a5x (commit bdc77c244cd3,
 * 2022-08-03). The file's own header (above) grants GPL-2.0-or-later; the
 * GitHub repo metadata carries no license. The same uapi (node goodix_fp,
 * input "gf-keys", netlink 29, ioctl magic 'g') appears in the Infinix
 * NOTE5 4.4 tree (gf_spi_tee.c, same header). Reworked for this tree: misc
 * device, refcounted lifetime, devm GPIOs, bounds-checked ioctls, compat
 * ioctl, restricted netlink registration, no sensor-specific SPI probing.
 */

#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <net/sock.h>

#include "gf_common.h"

#define GF_DEV_NAME		"goodix_fp"
#define GF_INPUT_NAME		"gf-keys"
#define GF_NETLINK_ROUTE	29
#define GF_BUFSIZ		(30 * 1024)
#define WAKELOCK_HOLD_MS	500

/* the HAL runs as root or system; others may not claim the event socket */
#define GF_UID_SYSTEM		1000

static struct gf_device *gf_global;	/* single sensor; netlink input has no ctx */
static atomic_t gf_nl_pid = ATOMIC_INIT(0);

static void gf_enable_irq(struct gf_device *gf)
{
	if (!gf->irq_enabled) {
		gf->irq_enabled = true;
		enable_irq(gf->irq);
	}
}

static void gf_disable_irq(struct gf_device *gf)
{
	if (gf->irq_enabled) {
		gf->irq_enabled = false;
		disable_irq(gf->irq);
	}
}

/* ---- netlink ---- */
static void gf_netlink_send(struct gf_device *gf, int command)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	int pid = atomic_read(&gf_nl_pid);

	if (!gf->nl_sk || !pid)
		return;

	skb = nlmsg_new(sizeof(char), GFP_ATOMIC);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, 0, sizeof(char), 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}
	NETLINK_CB(skb).portid = 0;
	NETLINK_CB(skb).dst_group = 0;
	*(char *)nlmsg_data(nlh) = command;
	/* netlink_unicast frees the skb on failure */
	netlink_unicast(gf->nl_sk, skb, pid, MSG_DONTWAIT);
}

static void gf_netlink_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	kuid_t uid = NETLINK_CB(skb).creds.uid;

	if (skb->len < NLMSG_HDRLEN)
		return;
	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len))
		return;
	if (!uid_eq(uid, GLOBAL_ROOT_UID) &&
	    !uid_eq(uid, make_kuid(&init_user_ns, GF_UID_SYSTEM)))
		return;
	atomic_set(&gf_nl_pid, nlh->nlmsg_pid);
}

/* ---- screen events ---- */
static int gf_fb_notifier(struct notifier_block *nb, unsigned long event,
			  void *data)
{
	struct gf_device *gf = container_of(nb, struct gf_device, fb_nb);
	struct fb_event *evdata = data;
	int *blank;

	if (event != FB_EARLY_EVENT_BLANK || !evdata || !evdata->data)
		return 0;
	blank = evdata->data;
	if (*blank == FB_BLANK_UNBLANK)
		gf_netlink_send(gf, GF_NETLINK_SCREEN_ON);
	else if (*blank == FB_BLANK_POWERDOWN)
		gf_netlink_send(gf, GF_NETLINK_SCREEN_OFF);
	return 0;
}

static irqreturn_t gf_irq(int irq, void *handle)
{
	struct gf_device *gf = handle;

	wake_lock_timeout(&gf->wake_lock, msecs_to_jiffies(WAKELOCK_HOLD_MS));
	gf_netlink_send(gf, GF_NETLINK_IRQ);
	return IRQ_HANDLED;
}

/* ---- hardware ---- */
static void gf_hw_reset(struct gf_device *gf)
{
	gpio_set_value(gf->reset_gpio, 0);
	mdelay(3);
	gpio_set_value(gf->reset_gpio, 1);
}

static void gf_hw_power(struct gf_device *gf, bool on)
{
	if (gpio_is_valid(gf->pwr_gpio))
		gpio_set_value(gf->pwr_gpio, on);
}

/* ---- ioctl ---- */
static void gf_report_click(struct gf_device *gf, unsigned int code)
{
	input_report_key(gf->input, code, 1);
	input_sync(gf->input);
	input_report_key(gf->input, code, 0);
	input_sync(gf->input);
}

static int gf_do_key(struct gf_device *gf, void __user *argp)
{
	struct gf_key k;

	if (copy_from_user(&k, argp, sizeof(k)))
		return -EFAULT;
	switch (k.key) {
	case GF_KEY_HOME:
		input_report_key(gf->input, KEY_HOMEPAGE, !!k.value);
		input_sync(gf->input);
		break;
	case GF_KEY_POWER:
		if (k.value == 1)
			gf_report_click(gf, KEY_POWER);
		break;
	case GF_KEY_CAMERA:
		if (k.value == 1)
			gf_report_click(gf, KEY_CAMERA);
		break;
	default:	/* upstream forwarded raw codes; only declared keys now */
		break;
	}
	return 0;
}

static int gf_do_nav(struct gf_device *gf, void __user *argp)
{
	gf_nav_event_t ev;
	unsigned int code;

	if (copy_from_user(&ev, argp, sizeof(ev)))
		return -EFAULT;
	switch (ev) {
	case GF_NAV_UP:		code = KEY_UP; break;
	case GF_NAV_DOWN:	code = KEY_DOWN; break;
	case GF_NAV_LEFT:	code = KEY_LEFT; break;
	case GF_NAV_RIGHT:	code = KEY_RIGHT; break;
	case GF_NAV_CLICK:	code = KEY_VOLUMEDOWN; break;
	case GF_NAV_HEAVY:	code = KEY_CHAT; break;
	case GF_NAV_LONG_PRESS:	code = KEY_SEARCH; break;
	case GF_NAV_DOUBLE_CLICK: code = KEY_VOLUMEUP; break;
	default:
		return 0;
	}
	gf_report_click(gf, code);
	return 0;
}

static int gf_do_raw(struct gf_device *gf, u32 len, const void __user *wbuf,
		     void __user *rbuf, u32 speed)
{
	u8 *tx, *rx;
	int ret;

	if (len == 0 || len > gf->bufsiz || !wbuf || !rbuf)
		return -EINVAL;
	tx = kmalloc(len, GFP_KERNEL);
	rx = kzalloc(len, GFP_KERNEL);
	if (!tx || !rx) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(tx, wbuf, len)) {
		ret = -EFAULT;
		goto out;
	}
	gf_spi_setup_conf(gf, speed);
	ret = gf_spi_transfer_raw(gf, tx, rx, len);
	if (!ret && copy_to_user(rbuf, rx, len))
		ret = -EFAULT;
out:
	kfree(tx);
	kfree(rx);
	return ret;
}

static long gf_ioctl_locked(struct gf_device *gf, unsigned int cmd,
			    unsigned long arg, bool compat)
{
	void __user *argp = (void __user *)arg;
	struct gf_ioc_chip_info info;
	u8 val;

	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case GF_IOC_INIT:
		val = GF_NETLINK_ROUTE;
		if (put_user(val, (u8 __user *)argp))
			return -EFAULT;
		if (!gf->system_status)
			gf->system_status = true;
		return 0;
	case GF_IOC_CHIP_INFO:
		if (copy_from_user(&info, argp, sizeof(info)))
			return -EFAULT;
		gf->vendor_id = info.vendor_id;
		return 0;
	case GF_IOC_EXIT:
		gf_disable_irq(gf);
		gf->system_status = false;
		return 0;
	case GF_IOC_RESET:
		gf_hw_reset(gf);
		return 0;
	case GF_IOC_ENABLE_IRQ:
		gf_enable_irq(gf);
		return 0;
	case GF_IOC_DISABLE_IRQ:
		gf_disable_irq(gf);
		return 0;
	case GF_IOC_ENABLE_SPI_CLK:
	case GF_IOC_DISABLE_SPI_CLK:
	case GF_IOC_ENTER_SLEEP_MODE:
	case GF_IOC_REMOVE:
	case GF_IOC_SPI_INIT_CFG_CMD:
		return 0;	/* no-ops upstream too */
	case GF_IOC_ENABLE_POWER:
		gf_hw_power(gf, true);
		return 0;
	case GF_IOC_DISABLE_POWER:
		gf_hw_power(gf, false);
		return 0;
	case GF_IOC_INPUT_KEY_EVENT:
		return gf_do_key(gf, argp);
	case GF_IOC_NAV_EVENT:
		return gf_do_nav(gf, argp);
	case GF_IOC_GET_FW_INFO:
		val = 1;	/* upstream always reports 1 */
		return put_user(val, (u8 __user *)argp) ? -EFAULT : 0;
	case GF_IOC_TRANSFER_RAW_CMD: {
		struct gf_ioc_transfer_raw r;

		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;
		return gf_do_raw(gf, r.len, r.write_buf, r.read_buf,
				 r.high_time);
	}
#ifdef CONFIG_COMPAT
	case GF_IOC_TRANSFER_RAW_CMD_COMPAT: {
		struct gf_ioc_transfer_raw_compat r;

		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;
		return gf_do_raw(gf, r.len, compat_ptr(r.write_buf),
				 compat_ptr(r.read_buf), r.high_time);
	}
#endif
	default:
		return -ENOTTY;
	}
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_device *gf = filp->private_data;
	long ret;

	mutex_lock(&gf->lock);
	ret = gf->dead ? -ENODEV : gf_ioctl_locked(gf, cmd, arg, false);
	mutex_unlock(&gf->lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static long gf_compat_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct gf_device *gf = filp->private_data;
	long ret;

	arg = (unsigned long)compat_ptr(arg);
	mutex_lock(&gf->lock);
	ret = gf->dead ? -ENODEV : gf_ioctl_locked(gf, cmd, arg, true);
	mutex_unlock(&gf->lock);
	return ret;
}
#endif

static void gf_free(struct kref *ref)
{
	struct gf_device *gf = container_of(ref, struct gf_device, ref);

	kfree(gf->spi_buffer);
	kfree(gf);
}

static int gf_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *m = filp->private_data;
	struct gf_device *gf = container_of(m, struct gf_device, misc);

	if (gf != gf_global || gf->dead)
		return -ENODEV;
	kref_get(&gf->ref);
	filp->private_data = gf;
	nonseekable_open(inode, filp);
	return 0;
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_device *gf = filp->private_data;

	mutex_lock(&gf->lock);
	if (!gf->dead)
		gf_disable_irq(gf);
	mutex_unlock(&gf->lock);
	kref_put(&gf->ref, gf_free);
	return 0;
}

static const struct file_operations gf_fops = {
	.owner		= THIS_MODULE,
	.open		= gf_open,
	.release	= gf_release,
	.unlocked_ioctl	= gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= gf_compat_ioctl,
#endif
	.llseek		= no_llseek,
};

/* ---- probe/remove ---- */
static int gf_parse_gpios(struct gf_device *gf)
{
	struct device *dev = &gf->spi->dev;
	struct device_node *np = dev->of_node;
	int ret;

	gf->reset_gpio = of_get_named_gpio(np, "goodix,gpio_reset", 0);
	gf->irq_gpio = of_get_named_gpio(np, "goodix,gpio_irq", 0);
	gf->pwr_gpio = of_get_named_gpio(np, "goodix,gpio_pwr", 0);

	if (!gpio_is_valid(gf->reset_gpio) || !gpio_is_valid(gf->irq_gpio)) {
		dev_err(dev, "missing reset/irq gpio in DT\n");
		return -ENODEV;
	}
	ret = devm_gpio_request_one(dev, gf->reset_gpio, GPIOF_OUT_INIT_HIGH,
				    "goodix_reset");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(dev, gf->irq_gpio, GPIOF_IN, "goodix_irq");
	if (ret)
		return ret;
	if (gpio_is_valid(gf->pwr_gpio)) {	/* optional */
		ret = devm_gpio_request_one(dev, gf->pwr_gpio,
					    GPIOF_OUT_INIT_HIGH, "goodix_pwr");
		if (ret)
			return ret;
	}
	return 0;
}

static int gf_probe(struct spi_device *spi)
{
	struct netlink_kernel_cfg cfg = { .input = gf_netlink_recv };
	struct device *dev = &spi->dev;
	struct gf_device *gf;
	int ret;

	if (!dev->of_node)
		return -ENODEV;
	if (gf_global)
		return -EBUSY;

	gf = kzalloc(sizeof(*gf), GFP_KERNEL);
	if (!gf)
		return -ENOMEM;
	kref_init(&gf->ref);
	mutex_init(&gf->lock);
	gf->spi = spi;
	gf->bufsiz = GF_BUFSIZ;
	gf->spi_buffer = kzalloc(gf->bufsiz, GFP_KERNEL);
	if (!gf->spi_buffer) {
		ret = -ENOMEM;
		goto err_put;
	}

	ret = gf_parse_gpios(gf);
	if (ret)
		goto err_put;

	gf->irq = gpio_to_irq(gf->irq_gpio);
	if (gf->irq < 0) {
		ret = gf->irq;
		goto err_put;
	}

	gf_spi_setup_conf(gf, 4);
	wake_lock_init(&gf->wake_lock, WAKE_LOCK_SUSPEND, "gf_wake_lock");

	gf->input = input_allocate_device();
	if (!gf->input) {
		ret = -ENOMEM;
		goto err_wl;
	}
	gf->input->name = GF_INPUT_NAME;
	__set_bit(EV_KEY, gf->input->evbit);
	__set_bit(KEY_HOMEPAGE, gf->input->keybit);
	__set_bit(KEY_MENU, gf->input->keybit);
	__set_bit(KEY_BACK, gf->input->keybit);
	__set_bit(KEY_POWER, gf->input->keybit);
	__set_bit(KEY_CAMERA, gf->input->keybit);
	__set_bit(KEY_UP, gf->input->keybit);
	__set_bit(KEY_DOWN, gf->input->keybit);
	__set_bit(KEY_LEFT, gf->input->keybit);
	__set_bit(KEY_RIGHT, gf->input->keybit);
	__set_bit(KEY_VOLUMEDOWN, gf->input->keybit);
	__set_bit(KEY_VOLUMEUP, gf->input->keybit);
	__set_bit(KEY_SEARCH, gf->input->keybit);
	__set_bit(KEY_CHAT, gf->input->keybit);
	ret = input_register_device(gf->input);
	if (ret) {
		input_free_device(gf->input);
		goto err_wl;
	}

	gf_global = gf;
	gf->nl_sk = netlink_kernel_create(&init_net, GF_NETLINK_ROUTE, &cfg);
	if (!gf->nl_sk) {
		dev_err(dev, "netlink protocol %d unavailable\n",
			GF_NETLINK_ROUTE);
		ret = -ENOMEM;
		goto err_input;
	}

	/* IRQ stays masked until the HAL asks for it */
	irq_set_status_flags(gf->irq, IRQ_NOAUTOEN);
	ret = request_threaded_irq(gf->irq, NULL, gf_irq,
				   IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				   "goodix_fp_irq", gf);
	if (ret)
		goto err_nl;
	enable_irq_wake(gf->irq);

	gf->misc.minor = MISC_DYNAMIC_MINOR;
	gf->misc.name = GF_DEV_NAME;
	gf->misc.fops = &gf_fops;
	ret = misc_register(&gf->misc);
	if (ret)
		goto err_irq;

	gf->fb_nb.notifier_call = gf_fb_notifier;
	if (!fb_register_client(&gf->fb_nb))
		gf->fb_registered = true;

	spi_set_drvdata(spi, gf);
	dev_info(dev, "goodix fingerprint interface ready\n");
	return 0;

err_irq:
	disable_irq_wake(gf->irq);
	free_irq(gf->irq, gf);
err_nl:
	netlink_kernel_release(gf->nl_sk);
err_input:
	gf_global = NULL;
	input_unregister_device(gf->input);
err_wl:
	wake_lock_destroy(&gf->wake_lock);
err_put:
	kref_put(&gf->ref, gf_free);
	return ret;
}

static int gf_remove(struct spi_device *spi)
{
	struct gf_device *gf = spi_get_drvdata(spi);

	if (gf->fb_registered)
		fb_unregister_client(&gf->fb_nb);
	misc_deregister(&gf->misc);

	mutex_lock(&gf->lock);
	gf->dead = true;
	mutex_unlock(&gf->lock);

	disable_irq_wake(gf->irq);
	free_irq(gf->irq, gf);
	netlink_kernel_release(gf->nl_sk);
	gf->nl_sk = NULL;
	gf_global = NULL;
	atomic_set(&gf_nl_pid, 0);
	input_unregister_device(gf->input);
	gf->input = NULL;
	gf_hw_power(gf, false);
	wake_lock_destroy(&gf->wake_lock);
	kref_put(&gf->ref, gf_free);
	return 0;
}

static const struct of_device_id gf_of_match[] = {
	{ .compatible = "goodix,fingerprintree", },
	{ },
};
MODULE_DEVICE_TABLE(of, gf_of_match);

static struct spi_driver gf_spi_driver = {
	.driver = {
		.name = GF_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = gf_of_match,
	},
	.probe = gf_probe,
	.remove = gf_remove,
};
module_spi_driver(gf_spi_driver);

MODULE_AUTHOR("goodix");
MODULE_DESCRIPTION("Goodix fingerprint TEE/HAL kernel interface");
MODULE_LICENSE("GPL");
