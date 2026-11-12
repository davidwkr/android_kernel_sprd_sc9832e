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

#include "sprd_dispc.h"

static struct dispc_clk_context {
	struct device_node *np;
	uint32_t clk_src[DISPC_CLK_ID_MAX];
	struct clk *clk_dispc_core_parent;
	struct clk *clk_dispc_dpi_parent;
	struct clk *clk_dispc_core;
	struct clk *clk_dispc_dpi;
	bool clk_enabled;
	bool clk_warned;
} dispc_clk_ctx;

static struct dispc_glb_context {
	struct device_node *np;
	struct regmap *ap_ahb;
	struct clk *clk_dispc_ahb_eb;
	bool clk_enabled;
	bool ap_ahb_warned;
	bool clk_warned;
} dispc_glb_ctx;

/*
 * The syscon and the clock provider may not be registered when parse_dt runs
 * (the 4.14 clock driver is a platform driver), so look them up again on use,
 * and log a missing one once instead of handing an error pointer on.
 */
static bool dispc_clk_get_one(struct device_node *np, const char *name,
			      struct clk **clk, bool *warned)
{
	struct clk *tmp;

	if (*clk)
		return true;
	if (!np)
		return false;

	tmp = of_clk_get_by_name(np, name);
	if (IS_ERR(tmp)) {
		if (!*warned) {
			pr_err("read %s failed (%ld)\n", name, PTR_ERR(tmp));
			*warned = true;
		}
		return false;
	}

	*clk = tmp;
	return true;
}

static bool dispc_clk_get_all(struct dispc_clk_context *clk_ctx)
{
	bool ok = true;

	ok &= dispc_clk_get_one(clk_ctx->np, "clk_dispc_core_parent",
				&clk_ctx->clk_dispc_core_parent,
				&clk_ctx->clk_warned);
	ok &= dispc_clk_get_one(clk_ctx->np, "clk_dispc_dpi_parent",
				&clk_ctx->clk_dispc_dpi_parent,
				&clk_ctx->clk_warned);
	ok &= dispc_clk_get_one(clk_ctx->np, "clk_dispc_core",
				&clk_ctx->clk_dispc_core,
				&clk_ctx->clk_warned);
	ok &= dispc_clk_get_one(clk_ctx->np, "clk_dispc_dpi",
				&clk_ctx->clk_dispc_dpi,
				&clk_ctx->clk_warned);

	return ok;
}

static int dispc_clk_parse_dt(struct dispc_context *ctx,
				struct device_node *np)
{
	int ret = 0;
	int status = 0;
	struct dispc_clk_context *clk_ctx = &dispc_clk_ctx;

	clk_ctx->np = np;

	ret = of_property_read_u32_array(np,
			 "clock-src", clk_ctx->clk_src, 2);
	if (ret) {
		pr_err("read clock-src fail (%d)\n", ret);
		status |= -1;
	}

	ctx->dpi_clk_src = clk_ctx->clk_src[1];
	pr_info("the dpi clock source from dts is %d\n", ctx->dpi_clk_src);

	return status;
}


static int dispc_clk_init(struct dispc_context *ctx)
{
	int ret = 0;
	struct dispc_clk_context *clk_ctx = &dispc_clk_ctx;

	if (!dispc_clk_get_all(clk_ctx))
		return -ENODEV;

	ret = clk_set_parent(clk_ctx->clk_dispc_core,
			clk_ctx->clk_dispc_core_parent);
	if (ret)
		pr_err("dispc set clk parent fail\n");

	ret = clk_set_parent(clk_ctx->clk_dispc_dpi,
			clk_ctx->clk_dispc_dpi_parent);
	if (ret)
		pr_err("dispc set dpi clk parent fail\n");

	return ret;
}


static int dispc_clk_enable(struct dispc_context *ctx)
{
	int ret = -1;
	struct dispc_clk_context *clk_ctx = &dispc_clk_ctx;

	if (clk_ctx->clk_enabled)
		return 0;
	if (!dispc_clk_get_all(clk_ctx))
		return -ENODEV;

	ret = clk_prepare_enable(clk_ctx->clk_dispc_core);
	if (ret) {
		pr_err("enable clk_dispc_core error\n");
		return ret;
	}
	ret = clk_prepare_enable(clk_ctx->clk_dispc_dpi);
	if (ret) {
		pr_err("enable clk_dispc_dpi error\n");
		clk_disable_unprepare(clk_ctx->clk_dispc_core);
		return ret;
	}

	clk_ctx->clk_enabled = true;
	return 0;
}

static int dispc_clk_disable(struct dispc_context *ctx)
{
	struct dispc_clk_context *clk_ctx = &dispc_clk_ctx;

	if (!clk_ctx->clk_enabled)
		return 0;

	clk_disable_unprepare(clk_ctx->clk_dispc_dpi);
	clk_disable_unprepare(clk_ctx->clk_dispc_core);
	clk_ctx->clk_enabled = false;

	clk_set_parent(clk_ctx->clk_dispc_dpi, NULL);
	clk_set_parent(clk_ctx->clk_dispc_core, NULL);

	return 0;
}


static int dispc_clk_update(struct dispc_context *ctx, int clk_id, int val)
{
	int ret = -1;
	struct dispc_clk_context *clk_ctx = &dispc_clk_ctx;

	switch (clk_id) {
	case DISPC_CLK_ID_CORE:
		pr_err("dispc core clk doesn't support update\n");
		break;

	case DISPC_CLK_ID_DPI:
		pr_info("dpi_clk = %d\n", val);
		if (!dispc_clk_get_all(clk_ctx))
			return -ENODEV;
		ret = clk_set_rate(clk_ctx->clk_dispc_dpi, val);
		if (ret)
			pr_err("dispc update dbi clk rate fail\n");
		break;

	default:
		pr_err("clk id %d doesn't support\n", clk_id);
		break;
	}

	return ret;
}

static struct regmap *dispc_glb_regmap_get(struct dispc_glb_context *glb_ctx)
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

static int dispc_glb_parse_dt(struct dispc_context *ctx,
				struct device_node *np)
{
	struct dispc_glb_context *glb_ctx = &dispc_glb_ctx;

	glb_ctx->np = np;
	dispc_glb_regmap_get(glb_ctx);

	return 0;
}

static void dispc_glb_enable(struct dispc_context *ctx)
{
	int ret = 0;
	struct dispc_glb_context *glb_ctx = &dispc_glb_ctx;

	if (glb_ctx->clk_enabled)
		return;
	if (!dispc_clk_get_one(glb_ctx->np, "clk_dispc_ahb_eb",
			       &glb_ctx->clk_dispc_ahb_eb,
			       &glb_ctx->clk_warned))
		return;

	ret = clk_prepare_enable(glb_ctx->clk_dispc_ahb_eb);
	if (ret) {
		pr_err("enable clk_dispc_ahb_eb failed!\n");
		return;
	}

	glb_ctx->clk_enabled = true;
}

static void dispc_glb_disable(struct dispc_context *ctx)
{
	struct dispc_glb_context *glb_ctx = &dispc_glb_ctx;

	if (!glb_ctx->clk_enabled)
		return;

	clk_disable_unprepare(glb_ctx->clk_dispc_ahb_eb);
	glb_ctx->clk_enabled = false;
}

static void dispc_reset(struct dispc_context *ctx)
{
	struct regmap *ap_ahb = dispc_glb_regmap_get(&dispc_glb_ctx);

	if (!ap_ahb)
		return;

	regmap_update_bits(ap_ahb,
		    REG_AP_AHB_AHB_RST, BIT_AP_AHB_DISPC_SOFT_RST,
		    BIT_AP_AHB_DISPC_SOFT_RST);
	udelay(10);
	regmap_update_bits(ap_ahb,
		    REG_AP_AHB_AHB_RST, BIT_AP_AHB_DISPC_SOFT_RST,
		    (unsigned int)(~BIT_AP_AHB_DISPC_SOFT_RST));
}

static void dispc_power_domain(struct dispc_context *ctx, int enable)
{
}

static struct dispc_clk_ops dispc_clk_ops = {
	.parse_dt = dispc_clk_parse_dt,
	.init = dispc_clk_init,
	.enable = dispc_clk_enable,
	.disable = dispc_clk_disable,
	.update = dispc_clk_update,
};

static struct dispc_glb_ops dispc_glb_ops = {
	.parse_dt = dispc_glb_parse_dt,
	.reset = dispc_reset,
	.enable = dispc_glb_enable,
	.disable = dispc_glb_disable,
	.power = dispc_power_domain,
};

static struct ops_entry clk_entry = {
	.ver = "sharkle",
	.ops = &dispc_clk_ops,
};

static struct ops_entry glb_entry = {
	.ver = "sharkle",
	.ops = &dispc_glb_ops,
};

static int __init dispc_glb_register(void)
{
	dispc_clk_ops_register(&clk_entry);
	dispc_glb_ops_register(&glb_entry);
	return 0;
}

subsys_initcall(dispc_glb_register);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("junxiao.feng@spreadtrum.com");
MODULE_DESCRIPTION("sprd sharkle dispc global and clk regs config");
