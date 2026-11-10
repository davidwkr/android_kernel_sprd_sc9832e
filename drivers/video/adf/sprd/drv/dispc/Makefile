# Only the lite_r1p0 DISPC/DPU backend is built: the K18 (sp9832e/sharkle)
# board's dispc/dsi nodes use "sprd,ip = \"dpu-lite-r1p0\"" /
# "sprd,dsi-ctrl-r1p0" in the stock 4.4 DTS (confirmed from
# devices/k18/stock.dts), not r2p0 despite the misleadingly-named
# CONFIG_DISP_LITE_R2P0 symbol (which only gates a DMA-mask/IOVA quirk in
# sprd_dispc.c, not backend selection). The other variants
# (r7p0/r8p0/lite_r1p0's r2p0 sibling/swdispc and their adf_config_*
# counterparts) are for other SoCs and were not ported.
obj-y += dispc_lite_r1p0.o adf_config_lite_r1p0.o
obj-y += dpu_lite_r1p0.o adf_config_dpu_lite_r1p0.o
