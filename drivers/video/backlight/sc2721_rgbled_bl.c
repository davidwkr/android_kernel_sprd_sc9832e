// SPDX-License-Identifier: GPL-2.0
/*
 * Backlight on the SC2721 PMIC RGB LED current sink (BLTC), as used by the
 * stock MT108. Ported from the 4.4 sc2721_rgbled_bl.c; the register
 * offsets are the 4.4 ANA_REG_GLB_* ones (CTL_BASE_ANA_GLB 0xc00), the
 * same as the ones leds-sc27xx-bltc.c uses for the SC2721.
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SC2721_MODULE_EN0	0xc08
#define SC2721_RTC_CLK_EN0	0xc10
#define SC2721_RGB_CTRL		0xea0
#define SC2721_BLTC_CTRL	0x180

#define SC2721_BLTC_EN		BIT(9)
#define SC2721_RTC_BLTC_EN	BIT(7)
#define SC2721_RGB_PD_SW	BIT(0)
#define SC2721_SLP_RGB_PD_EN	BIT(2)

struct sc2721_rgbled_bl {
	struct regmap *regmap;
	unsigned int led_level;
};

static void sc2721_rgbled_off(struct sc2721_rgbled_bl *bl)
{
	regmap_update_bits(bl->regmap, SC2721_RGB_CTRL,
			   SC2721_RGB_PD_SW | SC2721_SLP_RGB_PD_EN,
			   SC2721_RGB_PD_SW | SC2721_SLP_RGB_PD_EN);
	regmap_write(bl->regmap, SC2721_BLTC_CTRL, 0);
	regmap_update_bits(bl->regmap, SC2721_RTC_CLK_EN0,
			   SC2721_RTC_BLTC_EN, 0);
	regmap_update_bits(bl->regmap, SC2721_MODULE_EN0, SC2721_BLTC_EN, 0);
}

static void sc2721_rgbled_set_level(struct sc2721_rgbled_bl *bl, u32 level)
{
	if (level < 25) {
		level = 0;
	} else if (level > 255) {
		level = 255;
	} else {
		level = (80 * level * 100) / 255 / 3;
		level = (level - 169) / 84;
		level = level << 4;
	}

	regmap_update_bits(bl->regmap, SC2721_MODULE_EN0, SC2721_BLTC_EN,
			   SC2721_BLTC_EN);
	regmap_update_bits(bl->regmap, SC2721_RTC_CLK_EN0, SC2721_RTC_BLTC_EN,
			   SC2721_RTC_BLTC_EN);
	regmap_write(bl->regmap, SC2721_RGB_CTRL, level);
	regmap_write(bl->regmap, SC2721_BLTC_CTRL, 0xfff);
}

static int sc2721_rgbled_update_status(struct backlight_device *bd)
{
	struct sc2721_rgbled_bl *bl = bl_get_data(bd);

	if ((bd->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK)) ||
	    bd->props.power != FB_BLANK_UNBLANK || bd->props.brightness == 0) {
		sc2721_rgbled_off(bl);
	} else {
		bl->led_level = bd->props.brightness & 0xff;
		sc2721_rgbled_set_level(bl, bl->led_level);
	}

	return 0;
}

static int sc2721_rgbled_get_brightness(struct backlight_device *bd)
{
	struct sc2721_rgbled_bl *bl = bl_get_data(bd);

	return bl->led_level;
}

static const struct backlight_ops sc2721_rgbled_ops = {
	.update_status	= sc2721_rgbled_update_status,
	.get_brightness	= sc2721_rgbled_get_brightness,
};

static int sc2721_rgbled_probe(struct platform_device *pdev)
{
	struct backlight_properties props = { };
	struct backlight_device *bd;
	struct sc2721_rgbled_bl *bl;

	bl = devm_kzalloc(&pdev->dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	bl->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!bl->regmap) {
		dev_err(&pdev->dev, "unable to get regmap\n");
		return -ENODEV;
	}

	props.max_brightness = 0xff;
	props.type = BACKLIGHT_RAW;
	/* default brightness: half of the maximum */
	props.brightness = 0xff >> 1;
	props.power = FB_BLANK_UNBLANK;

	bd = devm_backlight_device_register(&pdev->dev, "sprd_backlight",
					    &pdev->dev, bl,
					    &sc2721_rgbled_ops, &props);
	if (IS_ERR(bd)) {
		dev_err(&pdev->dev, "failed to register backlight device\n");
		return PTR_ERR(bd);
	}

	platform_set_drvdata(pdev, bd);
	backlight_update_status(bd);

	return 0;
}

static void sc2721_rgbled_shutdown(struct platform_device *pdev)
{
	struct backlight_device *bd = platform_get_drvdata(pdev);

	bd->props.brightness = 0;
	backlight_update_status(bd);
}

#ifdef CONFIG_PM_SLEEP
static int sc2721_rgbled_suspend(struct device *dev)
{
	struct backlight_device *bd = dev_get_drvdata(dev);

	sc2721_rgbled_off(bl_get_data(bd));

	return 0;
}

static int sc2721_rgbled_resume(struct device *dev)
{
	struct backlight_device *bd = dev_get_drvdata(dev);

	backlight_update_status(bd);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(sc2721_rgbled_pm_ops, sc2721_rgbled_suspend,
			 sc2721_rgbled_resume);

static const struct of_device_id sc2721_rgbled_of_match[] = {
	{ .compatible = "sprd,sc2721-bltc-rgb" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2721_rgbled_of_match);

static struct platform_driver sc2721_rgbled_driver = {
	.driver = {
		.name		= "rgbled-backlight",
		.pm		= &sc2721_rgbled_pm_ops,
		.of_match_table	= sc2721_rgbled_of_match,
	},
	.probe		= sc2721_rgbled_probe,
	.shutdown	= sc2721_rgbled_shutdown,
};
module_platform_driver(sc2721_rgbled_driver);

MODULE_DESCRIPTION("Spreadtrum SC2721 PMIC RGB LED backlight driver");
MODULE_AUTHOR("albert.zhang@spreadtrum.com");
MODULE_LICENSE("GPL");
