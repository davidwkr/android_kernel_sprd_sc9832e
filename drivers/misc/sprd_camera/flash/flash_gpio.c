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
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>

#include "flash_drv.h"

#define FLASH_GPIO_MAX 3

/* Structure Definitions */

struct flash_driver_data {
	int gpio_tab[SPRD_FLASH_NUM_MAX][FLASH_GPIO_MAX];
	u32 torch_led_index;
};

/* Static Variables Definitions */

static const char *const flash_gpio_names[SPRD_FLASH_NUM_MAX] = {
	"flash0-gpios",
	"flash1-gpios",
	"flash2-gpios",
};


/* API Function Implementation */
static int sprd_flash_gpio_open_torch(void *drvd, uint8_t idx)
{
	int ret = 0;
	int gpio_id = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	idx = drv_data->torch_led_index;
	pr_info("sprd_flash_gpio_open_torch: drv_data->torch_led_index =0x%x\n", drv_data->torch_led_index);
	if (!drv_data)
		return -EFAULT;

	pr_info("sprd_flash_gpio_open_torch: idx =0x%x\n", idx);
	if (SPRD_FLASH_LED0 & idx) {
		gpio_id = drv_data->gpio_tab[0][0];
		if (gpio_is_valid(gpio_id)) {
	//			pr_info("sprd_flash_gpio_open_torch: gpio_id =%d\n", gpio_id);
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_ON);
			if (ret)
				goto exit;
		}
	}

	if (SPRD_FLASH_LED1 & idx) {
		gpio_id = drv_data->gpio_tab[1][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

exit:
	return ret;
}

static int sprd_flash_gpio_close_torch(void *drvd, uint8_t idx)
{
	int ret = 0;
	int gpio_id = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	idx = drv_data->torch_led_index;
	pr_info("sprd_flash_gpio_close_torch: drv_data->torch_led_index =0x%x\n", drv_data->torch_led_index);

	if (!drv_data)
		return -EFAULT;

	pr_info("sprd_flash_gpio_close_torch: idx =0x%x\n", idx);
	if (SPRD_FLASH_LED0 & idx) {
		gpio_id = drv_data->gpio_tab[0][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

	if (SPRD_FLASH_LED1 & idx) {
		gpio_id = drv_data->gpio_tab[1][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

exit:
	return ret;
}

static int sprd_flash_gpio_open_preflash(void *drvd, uint8_t idx)
{
	int ret = 0;
	int gpio_id = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	idx = drv_data->torch_led_index;
	pr_info("sprd_flash_gpio_open_preflash: idx =0x%x\n", idx);


	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx) {
		gpio_id = drv_data->gpio_tab[0][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_ON);
			if (ret)
				goto exit;
		}
	}

	if (SPRD_FLASH_LED1 & idx) {
		gpio_id = drv_data->gpio_tab[1][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

exit:
	return ret;
}

static int sprd_flash_gpio_close_preflash(void *drvd, uint8_t idx)
{
	int ret = 0;
	int gpio_id = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	idx = drv_data->torch_led_index;
	pr_info("sprd_flash_gpio_close_preflash: idx =0x%x\n", idx);


	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx) {
		gpio_id = drv_data->gpio_tab[0][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

	if (SPRD_FLASH_LED1 & idx) {
		gpio_id = drv_data->gpio_tab[1][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

exit:
	return ret;
}

static int sprd_flash_gpio_open_highlight(void *drvd, uint8_t idx)
{
	int ret = 0;
	int gpio_id = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	idx = drv_data->torch_led_index;
	pr_info("sprd_flash_gpio_open_highlight: idx =0x%x\n", idx);


	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx) {
		gpio_id = drv_data->gpio_tab[0][0];
		if (gpio_is_valid(gpio_id)) {
			
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
	
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);
			
			gpio_set_value(gpio_id, SPRD_FLASH_OFF);
			gpio_set_value(gpio_id, SPRD_FLASH_ON);

			
			if (ret)
				goto exit;
		}
	}

	if (SPRD_FLASH_LED1 & idx) {
		gpio_id = drv_data->gpio_tab[1][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_ON);
			if (ret)
				goto exit;
		}
	}

exit:
	return ret;
}

static int sprd_flash_gpio_close_highlight(void *drvd, uint8_t idx)
{
	int ret = 0;
	int gpio_id = 0;
	struct flash_driver_data *drv_data = (struct flash_driver_data *)drvd;
	idx = drv_data->torch_led_index;
	pr_info("sprd_flash_gpio_close_highlight: idx =0x%x\n", idx);

	if (!drv_data)
		return -EFAULT;

	if (SPRD_FLASH_LED0 & idx) {
		gpio_id = drv_data->gpio_tab[0][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

	if (SPRD_FLASH_LED1 & idx) {
		gpio_id = drv_data->gpio_tab[1][0];
		if (gpio_is_valid(gpio_id)) {
			ret = gpio_direction_output(gpio_id, SPRD_FLASH_OFF);
			if (ret)
				goto exit;
		}
	}

exit:
	return ret;
}

static const struct sprd_flash_driver_ops flash_gpio_ops = {
	.open_torch = sprd_flash_gpio_open_torch,
	.close_torch = sprd_flash_gpio_close_torch,
	.open_preflash = sprd_flash_gpio_open_preflash,
	.close_preflash = sprd_flash_gpio_close_preflash,
	.open_highlight = sprd_flash_gpio_open_highlight,
	.close_highlight = sprd_flash_gpio_close_highlight,
};

static int sprd_flash_gpio_probe(struct platform_device *pdev)
{
	int ret = 0;
	int i, j;
	u32 gpio_node = 0;
	struct flash_driver_data *drv_data = NULL;
	int gpio[SPRD_FLASH_NUM_MAX][FLASH_GPIO_MAX];

	if (IS_ERR_OR_NULL(pdev))
		return -EINVAL;

	ret = of_property_read_u32(pdev->dev.of_node, "sprd,gpio", &gpio_node);
	if (ret) {
		pr_info("no gpio flash\n");
		return -EINVAL;
	}
	pr_info("sprd_flash_gpio_probe\n");

	drv_data = devm_kzalloc(&pdev->dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	for (i = 0; i < SPRD_FLASH_NUM_MAX; i++) {
		for (j = 0; j < FLASH_GPIO_MAX; j++) {
			gpio[i][j] = of_get_named_gpio(pdev->dev.of_node,
						       flash_gpio_names[i], j);
			if (gpio_is_valid(gpio[i][j])) {
				ret = devm_gpio_request(&pdev->dev,
							gpio[i][j],
							flash_gpio_names[i]);
					pr_info("sprd_flash_gpio_probe gpio[i][j]=%d\n",gpio[i][j]);
				if (ret)
					goto exit;
			}
		}
	}

	//ret = of_property_read_u32(pdev->dev.of_node,
		//		"torch-led-idx", &drv_data->torch_led_index);
	//if (ret)
	drv_data->torch_led_index = SPRD_FLASH_LED0|SPRD_FLASH_LED1;

	memcpy((void *)drv_data->gpio_tab, (void *)gpio, sizeof(gpio));

	ret = sprd_flash_register(&flash_gpio_ops, drv_data, SPRD_FLASH_REAR);

exit:
	return ret;
}

static int sprd_flash_gpio_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id sprd_flash_gpio_of_match[] = {
	{ .compatible = "sprd,flash_gpio", },
	{},
};

static struct platform_driver sprd_flash_gpio_driver = {
	.probe = sprd_flash_gpio_probe,
	.remove = sprd_flash_gpio_remove,
	.driver = {
		.name = "flash-gpio",
		.of_match_table = of_match_ptr(sprd_flash_gpio_of_match),
	},
};

module_platform_driver(sprd_flash_gpio_driver);
