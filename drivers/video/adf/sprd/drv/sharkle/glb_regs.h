/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Sharkle AP_AHB / AON_APB register offsets and bits used by the
 * global_dispc.c/global_dphy.c/global_dsi.c clock and reset glue.
 *
 * The 4.4 tree gets these from
 * include/linux/mfd/syscon/sprd/sharkle/{ap_ahb,aon_apb}.h, which are not
 * part of the 4.14 tree (its DRM sprd driver reaches the same hardware
 * through pm_runtime/genpd instead of raw syscon pokes). These are SoC
 * register facts, not kernel-version-dependent code, so the offsets/bits
 * below are copied unchanged from that 4.4 header pair rather than
 * guessed.
 */
#ifndef __SPRD_ADF_SHARKLE_GLB_REGS_H
#define __SPRD_ADF_SHARKLE_GLB_REGS_H

#include <linux/bitops.h>

/* sprd,sys-ap-ahb */
#define REG_AP_AHB_AHB_RST			(0x0004)
#define REG_AP_AHB_MISC_CKG_EN			(0x0040)
#define BIT_AP_AHB_DISPC_SOFT_RST		BIT(1)
#define BIT_AP_AHB_DSI_SOFT_RST		BIT(0)
#define BIT_AP_AHB_DPHY_REF_CKG_EN		BIT(1)
#define BIT_AP_AHB_DPHY_CFG_CKG_EN		BIT(0)

/* sprd,sys-aon-apb */
#define REG_AON_APB_PWR_CTRL			(0x0024)
#define BIT_AON_APB_MIPI_DSI_PS_PD_S		BIT(15)
#define BIT_AON_APB_MIPI_DSI_PS_PD_L		BIT(14)

#endif /* __SPRD_ADF_SHARKLE_GLB_REGS_H */
