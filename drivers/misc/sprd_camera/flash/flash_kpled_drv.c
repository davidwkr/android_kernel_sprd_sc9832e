/*
 * Copyright (C) 2015-2016 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>
#include <linux/sprd_otp.h>

#include "flash_drv.h"

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/mfd/sprd/pmic_glb_reg.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <soc/sprd/board.h>

#define PRINT_INFO(x...)  pr_info("[SPRD_KPLED_INFO]" x)

#define KPLED_V_SHIFT           12
#define KPLED_V_MSK             (0x0F << KPLED_V_SHIFT)
#define KPLED_PD			(1 << 11)
#define KPLED_PULLDOWN_EN		(1 << 10)
#define FLASH_GPIO_MAX 3

enum sprd_pmic_kpled_type {
	UNKNOWN_PMIC_KPLED,
	SC2723_KPLED,
	SC2731_KPLED,
	SC2721_KPLED,
};

enum sprd_pmic_kpled_switch {
	KPLED_SWITCH_OFF,
	KPLED_SWITCH_ON,
};
/* Structure Definitions */

struct flash_driver_data {
	struct regmap *sprd_kpled_regmap;
	spinlock_t slock;
	int gpio_tab[SPRD_FLASH_NUM_MAX][FLASH_GPIO_MAX];
	void *priv;
	///add by jinq
	enum led_brightness value;
	int brightness_max;
	int brightness_min;
	int run_mode;
	enum sprd_pmic_kpled_type chip_version;
	unsigned int reg_kpled_ctrl0;/* current and ldo mode in sc2731 */
	unsigned int reg_kpled_ctrl1; /* ldo mode in sc2721 */
};

/* Static Variables Definitions */

static const char *const flash_gpio_names[SPRD_FLASH_NUM_MAX] = {
	"flash0-gpios",
	"flash1-gpios",
	"flash2-gpios",
};

static inline unsigned int kpled_read(struct flash_driver_data *led, unsigned long reg)
{
	unsigned int val;

	regmap_read(led->sprd_kpled_regmap, reg, &val);
	return val;
} 

static void sprd_kpled_ldo_switch(struct flash_driver_data *led, int power)
{
	unsigned int ldo_reg;
	unsigned int ldo_pd_mask;
	unsigned int ldo_pd_value = 0;
	if (led->chip_version == SC2721_KPLED) {
		ldo_reg = led->reg_kpled_ctrl1;
		ldo_pd_mask = (1 << 15);
	} else {
		ldo_reg = led->reg_kpled_ctrl0,
		ldo_pd_mask = (1 << 8);
	}
	if (power == KPLED_SWITCH_ON)
		ldo_pd_value = ~ldo_pd_mask;

	if (power == KPLED_SWITCH_OFF) {
		ldo_pd_value = ldo_pd_mask;
		regmap_update_bits(led->sprd_kpled_regmap,
				CTL_BASE_ANA_GLB + led->reg_kpled_ctrl0,
				KPLED_PULLDOWN_EN, KPLED_PULLDOWN_EN);
	}
	PRINT_INFO("reg:0x%08X ldo_pd_mask:0x%08X  ldo_pd_value:0x%08X\n",
			CTL_BASE_ANA_GLB + ldo_reg,
			ldo_pd_mask, ldo_pd_value);
	regmap_update_bits(led->sprd_kpled_regmap,
			CTL_BASE_ANA_GLB + ldo_reg,
			ldo_pd_mask, ldo_pd_value);
}

static void sprd_kpled_current_switch(struct flash_driver_data *led, int power)
{
	PRINT_INFO("%s power=%d\n", __func__, power);
	if (power == KPLED_SWITCH_ON)
		regmap_update_bits(led->sprd_kpled_regmap,
				   CTL_BASE_ANA_GLB + led->reg_kpled_ctrl0,
				   KPLED_PD, ~KPLED_PD);
	if (power == KPLED_SWITCH_OFF)
		regmap_update_bits(led->sprd_kpled_regmap,
				   CTL_BASE_ANA_GLB + led->reg_kpled_ctrl0,
				   KPLED_PD, KPLED_PD);
} 


static void sprd_kpled_set_brightness(struct flash_driver_data *led)
{
	unsigned long brightness = led->value;
	unsigned long brightness_level;
	unsigned int ldo_reg;
	unsigned int ldo_v_shift;
	unsigned int ldo_v_mask;

	brightness_level = brightness;

	if (brightness_level > 255)
		brightness_level = 255;

	if (brightness_level > led->brightness_max)
		brightness_level = led->brightness_max;

	if (brightness_level < led->brightness_min)
		brightness_level = led->brightness_min;

	#if (defined(ZCFG_SPRD_KPLED_BRIGHTNESS))
		if(brightness_level>ZCFG_SPRD_KPLED_BRIGHTNESS)	
		brightness_level = ZCFG_SPRD_KPLED_BRIGHTNESS;   ////modify by jinq 00Ϊ 1.2v     255Ϊ  1.2+2.55=3.75 v
	#else
		brightness=brightness_level=210;  //// 3.3v 
	#endif

	PRINT_INFO("sprd_kpled_set_brightness:led->run_mode = %d brightness_level=%ld\n",
		   led->run_mode,brightness_level);

	/*brightness steps = 16 */

	if (led->run_mode == 1) {
		regmap_update_bits(led->sprd_kpled_regmap,
				   CTL_BASE_ANA_GLB + led->reg_kpled_ctrl0,
				   KPLED_V_MSK,
				   ((brightness_level << KPLED_V_SHIFT) &
				    KPLED_V_MSK));
		PRINT_INFO("reg:0x%08X set_val:0x%08X  brightness:%ld\n",
			CTL_BASE_ANA_GLB + led->reg_kpled_ctrl0,
			kpled_read(led,
			 CTL_BASE_ANA_GLB + led->reg_kpled_ctrl0),
			brightness);
	} else {
		if (led->chip_version == SC2721_KPLED) {
			ldo_reg = led->reg_kpled_ctrl1,
			ldo_v_shift = 7;
			ldo_v_mask = 0xff << ldo_v_shift;
		} else {
			ldo_reg = led->reg_kpled_ctrl0,
			ldo_v_shift = 0;
			ldo_v_mask = 0xff << ldo_v_shift;
		}
			regmap_update_bits(led->sprd_kpled_regmap,
					CTL_BASE_ANA_GLB + ldo_reg,
					ldo_v_mask,
					((brightness_level << ldo_v_shift)
					 & ldo_v_mask));
			PRINT_INFO("reg:0x%08X set_val:0x%08X brightness:%ld\n",
					CTL_BASE_ANA_GLB + ldo_reg,
					kpled_read(led,
						CTL_BASE_ANA_GLB + ldo_reg),
					brightness);
	}
}

static void kpled_init(struct flash_driver_data *drv_data)
{
	/* flash ctrl */
	if (drv_data->run_mode == 1)  /* current mode */
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_ON);
	else  /* ldo mode */
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_ON);
}

#define BITSINDEX(b, o)  ((b) * 16 + (o))


/* API Function Implementation */

static int sprd_flash_kpled_open_torch(void *drvd, uint8_t idx)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	if (!drv_data)
		return -EFAULT;
	kpled_init(drv_data);
	if (SPRD_FLASH_LED0 & idx) {
		sprd_kpled_set_brightness(drv_data);
		}
	if (SPRD_FLASH_LED1 & idx) {
		sprd_kpled_set_brightness(drv_data);
		}
	return 0;
}

static int sprd_flash_kpled_close_torch(void *drvd, uint8_t idx)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;

	if (!drv_data)
		return -EFAULT;
	if (SPRD_FLASH_LED0 & idx) {
		if (drv_data->run_mode == 1)
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_OFF);
	else
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_OFF);
	}

	if (SPRD_FLASH_LED1 & idx) {
				if (drv_data->run_mode == 1)
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_OFF);
	else
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_OFF);
	}

	return 0;
}

static int sprd_flash_kpled_open_preflash(void *drvd, uint8_t idx)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	if (!drv_data)
		return -EFAULT;

	kpled_init(drv_data);
	if (SPRD_FLASH_LED0 & idx) {
	sprd_kpled_set_brightness(drv_data);
	}

	if (SPRD_FLASH_LED1 & idx) {
	sprd_kpled_set_brightness(drv_data);
	}
	return 0;
}

static int sprd_flash_kpled_close_preflash(void *drvd, uint8_t idx)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;

	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx) {
		if (drv_data->run_mode == 1)
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_OFF);
	else
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_OFF);
	}
	if (SPRD_FLASH_LED1 & idx) {
		if (drv_data->run_mode == 1)
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_OFF);
	else
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_OFF);
	}
	return 0;
}

static int sprd_flash_kpled_open_highlight(void *drvd, uint8_t idx)
{
	int ret = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	if (!drv_data)
		return -EFAULT;	
	kpled_init(drv_data);
	if (SPRD_FLASH_LED0 & idx) {
	sprd_kpled_set_brightness(drv_data);
	}

	if (SPRD_FLASH_LED1 & idx) {
	sprd_kpled_set_brightness(drv_data);
	}

	return ret;
}

static int sprd_flash_kpled_close_highlight(void *drvd, uint8_t idx)
{
	int ret = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;

	if (!drv_data)
		return -EFAULT;
	if (SPRD_FLASH_LED0 & idx) {
		if (drv_data->run_mode == 1)
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_OFF);
	else
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_OFF);
	}
	if (SPRD_FLASH_LED1 & idx) {
		if (drv_data->run_mode == 1)
		sprd_kpled_current_switch(drv_data, KPLED_SWITCH_OFF);
	else
		sprd_kpled_ldo_switch(drv_data, KPLED_SWITCH_OFF);
	}

	return ret;
}

static int sprd_flash_kpled_cfg_value_preflash(void *drvd, uint8_t idx,
					  struct sprd_flash_element *element)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	unsigned long brightness = drv_data->value;
	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx)
		brightness=element->index;

	if (SPRD_FLASH_LED1 & idx)
		brightness=element->index;

	return 0;
}

static int sprd_flash_kpled_cfg_value_highlight(void *drvd, uint8_t idx,
					   struct sprd_flash_element *element)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	unsigned long brightness = drv_data->value;
	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx)
		brightness=element->index;
	if (SPRD_FLASH_LED1 & idx)
		brightness=element->index;

	return 0;
}

static int sprd_flash_kpled_cfg_value_torch(void *drvd, uint8_t idx,
					   struct sprd_flash_element *element)
{
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	unsigned long brightness = drv_data->value;
	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx)
		brightness=element->index;

	if (SPRD_FLASH_LED1 & idx)
		brightness=element->index;

	return 0;
}

static const struct sprd_flash_driver_ops flash_kpled_ops = {
	.open_torch = sprd_flash_kpled_open_torch,
	.close_torch = sprd_flash_kpled_close_torch,
	.open_preflash = sprd_flash_kpled_open_preflash,
	.close_preflash = sprd_flash_kpled_close_preflash,
	.open_highlight = sprd_flash_kpled_open_highlight,
	.close_highlight = sprd_flash_kpled_close_highlight,
	.cfg_value_preflash = sprd_flash_kpled_cfg_value_preflash,
	.cfg_value_highlight = sprd_flash_kpled_cfg_value_highlight,
	.cfg_value_torch = sprd_flash_kpled_cfg_value_torch,
};

static const struct of_device_id sc2721_flash_of_match[] = {
	{.compatible = "sprd,sc2723t-kpled", .data = (void *)SC2723_KPLED,},
	{.compatible = "sprd,sc2731-kpled", .data = (void *)SC2731_KPLED,},
	{.compatible = "sprd,sc2721-kpled", .data = (void *)SC2721_KPLED,},
	{}

};

#ifdef CONFIG_OF
static struct flash_driver_data *sprd_kpled_parse_dt(struct
							    platform_device
							    *pdev)
{
	int ret;
	struct device_node *np = pdev->dev.of_node;
	struct flash_driver_data *pdata = NULL;
	const struct of_device_id *of_id;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		PRINT_INFO("sprd_kpled Could not allocate pdata");
		return NULL;
	}
	of_id = of_match_node(sc2721_flash_of_match,
		pdev->dev.of_node);
	if (!of_id) {
		PRINT_INFO("fail to get device id fail!\n");
		goto fail;
	}
	pdata->chip_version = (enum sprd_pmic_kpled_type)(of_id->data);
	PRINT_INFO("chip is %d\n", pdata->chip_version);
	ret =
	    of_property_read_u32(np, "brightness_max", &pdata->brightness_max);
	if (ret) {
		PRINT_INFO("fail to get pdata->brightness_max\n");
		goto fail;
	}
	ret =
	    of_property_read_u32(np, "brightness_min", &pdata->brightness_min);
	if (ret) {
		PRINT_INFO("fail to get pdata->brightness_min\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "run_mode", &pdata->run_mode);
	if (ret) {
		PRINT_INFO("fail to get pdata->run_mode\n");
		goto fail;
	}
	ret = of_property_read_u32_index(np, "reg", 0, &pdata->reg_kpled_ctrl0);
	if (ret) {
		PRINT_INFO("fail to get pdata->reg_kpled_ctrl0\n");
		goto fail;
	}
	PRINT_INFO("chip reg_kpled_ctrl0= %x\n", pdata->reg_kpled_ctrl0);

	if (pdata->chip_version == SC2721_KPLED) {
		ret = of_property_read_u32_index(np, "reg",
				1, &pdata->reg_kpled_ctrl1);
		if (ret) {
			PRINT_INFO("fail to get pdata->reg_kpled_ctrl0\n");
			goto fail;
		}
		PRINT_INFO("chip reg_kpled_ctrl1= %x\n",
				pdata->reg_kpled_ctrl1);
	}
	return pdata;
fail:
	kfree(pdata);
	return NULL;
}
#endif



static int sprd_flash_kpled_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct flash_driver_data *drv_data;
	struct device_node *np = pdev->dev.of_node;
	if (IS_ERR_OR_NULL(pdev))
		return -EINVAL;
	pdev->dev.platform_data = (void *)drv_data;
	#ifdef CONFIG_OF
	if (np) {
		drv_data = sprd_kpled_parse_dt(pdev);
		if (drv_data == NULL) {
			PRINT_INFO("get dts data failed!\n");
			return -ENODEV;
		}
	} else {
		PRINT_INFO("dev.of_node is NULL!\n");
		return -ENODEV;
	}
	#else
	drv_data = pdev->dev.platform_data;
	if (!drv_data) {
		PRINT_INFO("No kpled_platform data!\n");
		return -ENODEV;
	}	
	#endif

	drv_data->sprd_kpled_regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!drv_data->sprd_kpled_regmap) {
		PRINT_INFO("%s :NULL spi parent property for kpled!", __func__);
		ret = -ENOMEM;
		goto exit;
	}
	ret = sprd_flash_register(&flash_kpled_ops, drv_data,
							SPRD_FLASH_FRONT);
	if (ret < 0)
		goto exit;
exit:
	return ret;
}

static int sprd_flash_kpled_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver sprd_flash_kpled_drvier = {
	.probe = sprd_flash_kpled_probe,
	.remove = sprd_flash_kpled_remove,
	.driver = {
		.name = "sc2721-flash",
		.of_match_table = of_match_ptr(sc2721_flash_of_match),
	},
};

module_platform_driver(sprd_flash_kpled_drvier);
MODULE_AUTHOR("jqiang <jqiang@waterworld.com.cn>");
MODULE_DESCRIPTION("Sprd Keyboard backlight driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sprd_kpled");

