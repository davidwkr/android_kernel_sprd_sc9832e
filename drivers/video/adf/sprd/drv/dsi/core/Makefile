# Only the r1p0 DSI host core is built: the K18 dsi node uses
# "sprd,ip = \"sprd,dsi-ctrl-r1p0\"" (devices/k18/stock.dts). The
# dwc_mipi_dsi_host.o/_ppi.o pair is the DesignWare DSI controller used by
# other/newer SoCs and was not ported.
obj-y += mipi_dsi_r1p0.o mipi_dsi_r1p0_ppi.o
