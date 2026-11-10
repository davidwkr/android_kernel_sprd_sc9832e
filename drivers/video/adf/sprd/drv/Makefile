subdir-ccflags-y += -I$(src)

obj-y += dispc/
obj-y += dsi/
obj-y += dphy/
obj-y += panel-if/
obj-y += sysfs/

# Only the sharkle SoC glue is built: this skeleton targets the K18
# (sp9832e/sharkle) board only. The 4.4 tree's other SoC directories
# (whale2/iwhale2/sharkl2/isharkl2/sharklj1/sharkl3/pike2/sharkl5) and the
# encoder/ (DSC/BV3 compression, unused by the K18 panel) and
# sprd_spi_intf.o (SPI panel interface; K18 is MIPI DSI) were not ported.
obj-y += sharkle/

obj-y += sprd_panel.o
obj-y += sprd_dphy.o
obj-y += sprd_dsi.o
obj-y += sprd_dispc.o
obj-y += sprd_adf.o sprd_adf_bridge.o
obj-y += disp_lib.o
obj-y += disp_notify.o
obj-y += sprd_round_corner.o
obj-$(CONFIG_SPRD_AOD_IDLE) += sprd_aod_idle.o
