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
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include "glb_regs.h"
#include <linux/regmap.h>

#include "sprd_dsi.h"

static struct dsi_glb_context {
	struct device_node *np;
	struct regmap *ap_ahb;
	struct clk *clk_dsi0_ahb_eb;
	bool clk_enabled;
	bool ap_ahb_warned;
	bool clk_warned;
} dsi_glb_ctx;

/*
 * The syscon and the clock provider may not be registered when parse_dt runs
 * (the 4.14 clock driver is a platform driver), so look them up again on use,
 * and log a missing one once instead of handing an error pointer on.
 */
static struct regmap *dsi_glb_regmap_get(struct dsi_glb_context *glb_ctx)
{
	struct regmap *map;

	if (glb_ctx->ap_ahb)
		return glb_ctx->ap_ahb;

	map = syscon_regmap_lookup_by_compatible("sprd,sys-ap-ahb");
	if (IS_ERR(map)) {
		if (!glb_ctx->ap_ahb_warned) {
			pr_err("no sprd,sys-ap-ahb regmap (%ld)\n",
			       PTR_ERR(map));
			glb_ctx->ap_ahb_warned = true;
		}
		return NULL;
	}

	glb_ctx->ap_ahb = map;
	return map;
}

static struct clk *dsi_glb_clk_get(struct dsi_glb_context *glb_ctx)
{
	struct clk *clk;

	if (glb_ctx->clk_dsi0_ahb_eb)
		return glb_ctx->clk_dsi0_ahb_eb;
	if (!glb_ctx->np)
		return NULL;

	clk = of_clk_get_by_name(glb_ctx->np, "clk_dsi0_ahb_eb");
	if (IS_ERR(clk)) {
		if (!glb_ctx->clk_warned) {
			pr_err("read clk_dsi0_ahb_eb failed (%ld)\n",
			       PTR_ERR(clk));
			glb_ctx->clk_warned = true;
		}
		return NULL;
	}

	glb_ctx->clk_dsi0_ahb_eb = clk;
	return clk;
}

static int dsi_glb_parse_dt(struct dsi_context *ctx,
				struct device_node *np)
{
	struct dsi_glb_context *glb_ctx = &dsi_glb_ctx;

	glb_ctx->np = np;
	dsi_glb_regmap_get(glb_ctx);

	return 0;
}

static void dsi_glb_enable(struct dsi_context *ctx)
{
	int ret;
	struct dsi_glb_context *glb_ctx = &dsi_glb_ctx;
	struct clk *clk = dsi_glb_clk_get(glb_ctx);

	if (!clk || glb_ctx->clk_enabled)
		return;

	ret = clk_prepare_enable(clk);
	if (ret) {
		pr_err("enable clk_dsi0_ahb_eb failed!\n");
		return;
	}

	glb_ctx->clk_enabled = true;
}

static void dsi_glb_disable(struct dsi_context *ctx)
{
	struct dsi_glb_context *glb_ctx = &dsi_glb_ctx;

	if (!glb_ctx->clk_enabled)
		return;

	clk_disable_unprepare(glb_ctx->clk_dsi0_ahb_eb);
	glb_ctx->clk_enabled = false;
}

static void dsi_reset(struct dsi_context *ctx)
{
	struct regmap *ap_ahb = dsi_glb_regmap_get(&dsi_glb_ctx);

	if (!ap_ahb)
		return;

	regmap_update_bits(ap_ahb,
		    REG_AP_AHB_AHB_RST, BIT_AP_AHB_DSI_SOFT_RST,
		    BIT_AP_AHB_DSI_SOFT_RST);
	udelay(10);
	regmap_update_bits(ap_ahb,
		    REG_AP_AHB_AHB_RST, BIT_AP_AHB_DSI_SOFT_RST,
		    (unsigned int)(~BIT_AP_AHB_DSI_SOFT_RST));
}

static struct dsi_glb_ops dsi_glb_ops = {
	.parse_dt = dsi_glb_parse_dt,
	.reset = dsi_reset,
	.enable = dsi_glb_enable,
	.disable = dsi_glb_disable,
};

static struct ops_entry entry = {
	.ver = "sharkle",
	.ops = &dsi_glb_ops,
};

static int __init dsi_glb_register(void)
{
	return dsi_glb_ops_register(&entry);
}

subsys_initcall(dsi_glb_register);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("junxiao.feng@spreadtrum.com");
MODULE_DESCRIPTION("sprd sharkle dsi global AHB regs low-level config");
