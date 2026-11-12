/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
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
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include "glb_regs.h"
#include <linux/regmap.h>

#include "sprd_dphy.h"

static struct dphy_glb_regmap {
	struct regmap *map;
	const char *compat;
	bool warned;
} dphy_glb_ctx[] = {
	{ .compat = "sprd,sys-ap-ahb" },
	{ .compat = "sprd,sys-aon-apb" },
};

#define DPHY_AP_AHB	(&dphy_glb_ctx[0])
#define DPHY_AON_APB	(&dphy_glb_ctx[1])

/*
 * The syscon may not be registered when parse_dt runs, and a board dts may
 * lack the node altogether: look it up again on use, and log a missing one
 * once instead of handing an error pointer to regmap.
 */
static struct regmap *dphy_glb_regmap_get(struct dphy_glb_regmap *r)
{
	struct regmap *map;

	if (r->map)
		return r->map;

	map = syscon_regmap_lookup_by_compatible(r->compat);
	if (IS_ERR(map)) {
		if (!r->warned) {
			pr_err("no %s regmap (%ld)\n", r->compat, PTR_ERR(map));
			r->warned = true;
		}
		return NULL;
	}

	r->map = map;
	return map;
}

static int dphy_glb_parse_dt(struct dphy_context *ctx,
				struct device_node *np)
{
	dphy_glb_regmap_get(DPHY_AP_AHB);
	dphy_glb_regmap_get(DPHY_AON_APB);

	return 0;
}

static void dphy_glb_enable(struct dphy_context *ctx)
{
	struct regmap *ap_ahb = dphy_glb_regmap_get(DPHY_AP_AHB);

	if (!ap_ahb)
		return;

	regmap_update_bits(ap_ahb, REG_AP_AHB_MISC_CKG_EN,
		BIT_AP_AHB_DPHY_REF_CKG_EN | BIT_AP_AHB_DPHY_CFG_CKG_EN,
		BIT_AP_AHB_DPHY_REF_CKG_EN | BIT_AP_AHB_DPHY_CFG_CKG_EN);
}

static void dphy_glb_disable(struct dphy_context *ctx)
{
	struct regmap *ap_ahb = dphy_glb_regmap_get(DPHY_AP_AHB);

	if (!ap_ahb)
		return;

	regmap_update_bits(ap_ahb,
		REG_AP_AHB_MISC_CKG_EN,
		BIT_AP_AHB_DPHY_REF_CKG_EN |
		BIT_AP_AHB_DPHY_CFG_CKG_EN,
		(unsigned int)
		(~(BIT_AP_AHB_DPHY_REF_CKG_EN |
		BIT_AP_AHB_DPHY_CFG_CKG_EN)));
}

static void dphy_power_domain(struct dphy_context *ctx, int enable)
{
	struct regmap *aon_apb = dphy_glb_regmap_get(DPHY_AON_APB);

	if (!aon_apb)
		return;

	if (enable) {
		regmap_update_bits(aon_apb,
			REG_AON_APB_PWR_CTRL, BIT_AON_APB_MIPI_DSI_PS_PD_S,
			(unsigned int)(~BIT_AON_APB_MIPI_DSI_PS_PD_S));
		udelay(10);
		regmap_update_bits(aon_apb,
			REG_AON_APB_PWR_CTRL, BIT_AON_APB_MIPI_DSI_PS_PD_L,
			(unsigned int)(~BIT_AON_APB_MIPI_DSI_PS_PD_L));
	} else {
		regmap_update_bits(aon_apb,
			REG_AON_APB_PWR_CTRL, BIT_AON_APB_MIPI_DSI_PS_PD_L,
			BIT_AON_APB_MIPI_DSI_PS_PD_L);
		udelay(10);
		regmap_update_bits(aon_apb,
			REG_AON_APB_PWR_CTRL, BIT_AON_APB_MIPI_DSI_PS_PD_S,
			BIT_AON_APB_MIPI_DSI_PS_PD_S);
	}
}

static struct dphy_glb_ops dphy_glb_ops = {
	.parse_dt = dphy_glb_parse_dt,
	.enable = dphy_glb_enable,
	.disable = dphy_glb_disable,
	.power = dphy_power_domain,
};

static struct ops_entry entry = {
	.ver = "sharkle",
	.ops = &dphy_glb_ops,
};

static int __init dphy_glb_register(void)
{
	return dphy_glb_ops_register(&entry);
}

subsys_initcall(dphy_glb_register);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("junxiao.feng@spreadtrum.com");
MODULE_DESCRIPTION("sprd sharkle dphy global AHB regs low-level config");
