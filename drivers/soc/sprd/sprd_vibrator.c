// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC27xx PMIC vibrator, exposed through the Android timed_output
 * class (/sys/class/timed_output/vibrator/enable).
 *
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 */

#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/timed_output.h>
#include <linux/workqueue.h>

#define SLP_LDOVIBR_PD_EN	BIT(9)
#define LDO_VIBR_PD		BIT(8)
#define LDO_VIBR_V_MASK		GENMASK(7, 0)

/* Longest vibration a single request can ask for */
#define VIBRATOR_MAX_MS		15000

struct sprd_vibrator_data {
	u32 ldo_trim;
};

struct sprd_vibrator {
	struct timed_output_dev tdev;
	struct hrtimer		timer;
	struct work_struct	work;
	struct regmap		*regmap;
	const struct sprd_vibrator_data *data;
	struct mutex		hw_lock;	/* register sequences in work */
	spinlock_t		lock;		/* on, timer rearm */
	u32			base;
	bool			on;
};

static const struct sprd_vibrator_data sc2721_data = { .ldo_trim = 0xb4 };
static const struct sprd_vibrator_data sc2720_data = { .ldo_trim = 0x2 };

static struct sprd_vibrator *to_vib(struct timed_output_dev *tdev)
{
	return container_of(tdev, struct sprd_vibrator, tdev);
}

static void sprd_vibrator_set(struct sprd_vibrator *vib, bool on)
{
	mutex_lock(&vib->hw_lock);
	regmap_update_bits(vib->regmap, vib->base, LDO_VIBR_PD,
			   on ? 0 : LDO_VIBR_PD);
	regmap_update_bits(vib->regmap, vib->base, SLP_LDOVIBR_PD_EN,
			   on ? 0 : SLP_LDOVIBR_PD_EN);
	mutex_unlock(&vib->hw_lock);
}

static void sprd_vibrator_work(struct work_struct *work)
{
	struct sprd_vibrator *vib = container_of(work, struct sprd_vibrator,
						 work);
	unsigned long flags;
	bool on;

	spin_lock_irqsave(&vib->lock, flags);
	on = vib->on;
	spin_unlock_irqrestore(&vib->lock, flags);

	sprd_vibrator_set(vib, on);
}

/* May be called in atomic context: only arms the timer and queues work. */
static void sprd_vibrator_enable(struct timed_output_dev *tdev, int value)
{
	struct sprd_vibrator *vib = to_vib(tdev);
	unsigned long flags;

	spin_lock_irqsave(&vib->lock, flags);
	if (value <= 0) {
		hrtimer_try_to_cancel(&vib->timer);
		vib->on = false;
	} else {
		value = min(value, VIBRATOR_MAX_MS);
		vib->on = true;
		hrtimer_start(&vib->timer, ms_to_ktime(value),
			      HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&vib->lock, flags);

	schedule_work(&vib->work);
}

/*
 * A stop may fail to cancel a timer rearmed while an old callback was still
 * running; that timer only turns the motor off again, so report from the
 * logical state.
 */
static int sprd_vibrator_get_time(struct timed_output_dev *tdev)
{
	struct sprd_vibrator *vib = to_vib(tdev);
	unsigned long flags;
	s64 ms = 0;

	spin_lock_irqsave(&vib->lock, flags);
	if (vib->on && hrtimer_active(&vib->timer))
		ms = max_t(s64, ktime_to_ms(hrtimer_get_remaining(&vib->timer)),
			   0);
	spin_unlock_irqrestore(&vib->lock, flags);

	return ms;
}

static enum hrtimer_restart sprd_vibrator_timer(struct hrtimer *timer)
{
	struct sprd_vibrator *vib = container_of(timer, struct sprd_vibrator,
						 timer);
	unsigned long flags;

	spin_lock_irqsave(&vib->lock, flags);
	/* A new request rearmed the timer while we were waiting for the lock */
	if (!hrtimer_is_queued(timer)) {
		vib->on = false;
		schedule_work(&vib->work);
	}
	spin_unlock_irqrestore(&vib->lock, flags);

	return HRTIMER_NORESTART;
}

/* No request can arrive once the sysfs node is gone (or before it exists) */
static void sprd_vibrator_shutdown(struct sprd_vibrator *vib)
{
	hrtimer_cancel(&vib->timer);
	cancel_work_sync(&vib->work);
	vib->on = false;
	sprd_vibrator_set(vib, false);
}

static int sprd_vibrator_probe(struct platform_device *pdev)
{
	struct sprd_vibrator *vib;
	int ret;

	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	vib->data = of_device_get_match_data(&pdev->dev);
	if (!vib->data)
		return -EINVAL;

	vib->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!vib->regmap) {
		dev_err(&pdev->dev, "failed to get the PMIC regmap\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &vib->base);
	if (ret) {
		dev_err(&pdev->dev, "missing reg property\n");
		return ret;
	}

	mutex_init(&vib->hw_lock);
	spin_lock_init(&vib->lock);
	INIT_WORK(&vib->work, sprd_vibrator_work);
	hrtimer_init(&vib->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->timer.function = sprd_vibrator_timer;

	/* LDO trim, then leave it powered down */
	ret = regmap_update_bits(vib->regmap, vib->base, LDO_VIBR_V_MASK,
				 vib->data->ldo_trim);
	if (ret)
		return ret;
	sprd_vibrator_set(vib, false);

	vib->tdev.name = "vibrator";
	vib->tdev.enable = sprd_vibrator_enable;
	vib->tdev.get_time = sprd_vibrator_get_time;
	ret = timed_output_dev_register(&vib->tdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register timed_output: %d\n",
			ret);
		return ret;
	}

	platform_set_drvdata(pdev, vib);

	return 0;
}

static int sprd_vibrator_remove(struct platform_device *pdev)
{
	struct sprd_vibrator *vib = platform_get_drvdata(pdev);

	timed_output_dev_unregister(&vib->tdev);
	sprd_vibrator_shutdown(vib);

	return 0;
}

static int __maybe_unused sprd_vibrator_suspend(struct device *dev)
{
	sprd_vibrator_shutdown(dev_get_drvdata(dev));

	return 0;
}

static SIMPLE_DEV_PM_OPS(sprd_vibrator_pm_ops, sprd_vibrator_suspend, NULL);

static const struct of_device_id sprd_vibrator_of_match[] = {
	/* SC2731/SC2723 also need their RTC vibrator clock enabled: not here */
	{ .compatible = "sprd,sc2721-vibrator", .data = &sc2721_data },
	{ .compatible = "sprd,sc2720-vibrator", .data = &sc2720_data },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_vibrator_of_match);

static struct platform_driver sprd_vibrator_driver = {
	.driver = {
		.name = "sprd-vibrator",
		.of_match_table = sprd_vibrator_of_match,
		.pm = &sprd_vibrator_pm_ops,
	},
	.probe = sprd_vibrator_probe,
	.remove = sprd_vibrator_remove,
};
module_platform_driver(sprd_vibrator_driver);

MODULE_DESCRIPTION("Spreadtrum SC27xx PMIC vibrator (timed_output)");
MODULE_LICENSE("GPL");
