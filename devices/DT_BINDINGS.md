# Stock DT vs. this tree's drivers

Scope: the six stock DTs in `arch/arm64/boot/dts/sprd/sp9832e-1h10-{dw100u-silver,dw100u-v13m,dm63,mt107,mt108,dm82}.dts`
(byte-identical to stock, not modified here) against the drivers of this 4.4.302-based tree,
built with the merged config of `scripts/watch_build.sh` (`sprd_sharkle_defconfig` + `watch_base`,
`watch_stock_parity`, `watch_hardening` + the device fragments). Static analysis only, no device access.

Method: `tools/dt_driver_check.py` (python3 -I -B, read-only) lists every enabled node with a
`compatible`, finds the `of_device_id` / `*_OF_DECLARE` / `of_find_compatible_node` hit, checks the
Makefile gate against the merged `.config`, and greps the property reads of the matched file. The
output was then triaged by hand against the driver code; the tool alone is noisy (about 420 raw
"MISSING" lines per watch, nearly all optional properties, reads on other nodes, or child nodes of
a different compatible). Rows below are the triaged result.

Severity: **boot-blocking** (probe failure of a driver the boot or the recovery path needs),
**feature broken** (a peripheral does not work), **cosmetic** (no functional effect).

## Result in one paragraph

No boot-blocking mismatch was found for any of the six watches. Every property the display,
WCN/GNSS, battery/charger/FGU, regulator, USB and SIPC drivers treat as mandatory is present in
the stock DTs, and the 4.4.83 BSP bindings still match the 4.4.302 code (names, cell counts,
`sprd,ip`/`sprd,soc` selector strings). The residual risks are the ones in "Watch the following".

## Common findings (all six watches)

### Matched, mandatory properties verified

| Area | Node(s) | Driver code | Verdict |
|---|---|---|---|
| Display | `dispc` (`sprd,display-controller`), `dsi` (`sprd,dsi-controller`), `dphy` (`sprd,mipi-dsi-phy`), `sprd-adf` | `drivers/video/adf/sprd/drv/dispc/dpu_lite_r1p0.c:954`, `dsi/core/mipi_dsi_r1p0.c:1312`, `dphy/pll/megacores_sharkle.c:668`, `sharkle/global_*.c` | `sprd,ip` / `sprd,soc` strings all have a table entry. OK |
| Display | `dsi` optional reads `sprd,max-read-time`, `sprd,int0_mask`, `sprd,int1_mask`, `dphy` `dev-id` | `sprd_dsi.c:298-308`, `sprd_dphy.c:196` | Absent in stock, driver has defaults (0x6000 / all ones). cosmetic |
| Display | `sprd-panel-if` + `lcds/*` children | `sprd_panel.c:773-799` (panel pick by `lcd%x` of the bootloader `lcd_id=` against the node name) | `panel-drivers` phandle list present; node names `lcd<hex>` match the scheme. Stock panel props are all read, except the ones in the next rows. OK |
| Display | `mipi-cmd-mode`, `color-mode-pol`, `shut-down-pol` in `lcds/*` | only the fbdev driver reads them (`drivers/video/fbdev/sprdfb/sprdfb_mipi_panel.c:447,452`), not built | cosmetic. ADF takes `work-mode`. |
| Display | `aod-in` / `aod-out` in `lcds/*` (all but DM63) | not read; `drivers/video/adf/sprd/drv/sprd_aod_idle.c` hard-codes DCS 0x38/0x39 (standard MIPI DCS idle off/on, documented in its header) | cosmetic. `SPRD_AOD_IDLE` is enabled for DM63, DW100U, MT107 and DM82 (`mt108.fragment` disables it). Behaviour on non-CO5300 panels is unverified, not known broken. Proposed DTS fix: none |
| Display | `sprd_backlight` (`pwm-backlight`) | `drivers/video/backlight/pwm_bl.c:197`, `BACKLIGHT_PWM` is off, as in the stock config (`watch_stock_parity.fragment`) | OLED panels carry an `oled-backlight` child (`sprd_panel.c:623`) and use it. Non-OLED panels (see list per watch in the DT: `lcd9b71`, `lcd77916*`, `lcd7797`, `lcdffff`, ...) have no brightness path through this node. Same as stock. cosmetic / unchanged |
| IOMMU | 7 `sprd,iommuexle-*` nodes | `drivers/soc/sprd/iommu/sprd_iommu.c:199-217`, reads regs 0..2 (`:1578-1602`) | 3 `reg` regions + `reg_name` present; `sprd,reserved-*-page` optional. The tool's "reg has 12, driver wants 4" is a false positive (`:1727` reads the `memory-region` node). OK |
| WCN / GNSS | `cpwcn_btwf` (`sprd,integrate_marlin`), `cpwcn_gnss` (`sprd,integrate_gnss`) | `drivers/misc/sprdwcn_bsp/sc23xx/boot/wcn_integrate_dev.c:505-550` | `vddwcn-supply`, `reg`, `sprd,file-length`, `sprd,syscon-*`, `sprd,ctrl-*` present. `sprd,file-name(-ext)` absent, optional (`:534-541`). `vddwifipa-supply` only required for MARLIN (`:517`), GNSS node has none, OK |
| WCN / GNSS | `gnss_common_ctl` | `sc23xx/platform/gnss/gnss_common_ctl.c:85-86` | reads no DT property; chip version is match data (0x22). The "UNKGATE" in the tool is its limit: `SC23XX=y`, `WCN_PLATFORM=y` build it in. OK |
| WCN | `wcn_bt`, `wcn_fm` (`sprd,wcn_internal_chip`) | `sipc_modules/bluetooth/driver/tty-sipc/tty.c:643`, `fm/driver/fm_sipc/fmdrv_ops.c:275` | external modules (`sprdbt_tty.ko`, `sprd_fm.ko`, built by `watch_build.sh`), not in the in-tree config. OK |
| WCN | `sprd-wlan` (`sprd,sp9832e`) | `drivers/misc/sprdwcn_bsp/sc2332/main.c:1180` (module `sprdwl_ng`, `SPRDWL_NG=m`) | OK |
| SIPC | `sipc@*` (`sprd,sipc-v1`) | `drivers/staging/sprd/modem/sipc/sipc.c:234-246` | `sprd,dst` has 2 cells; the 3-cell read is under `CONFIG_SPRD_MAILBOX_SENSOR` and falls back to 2. The "CELLS" lines are expected. OK |
| Battery | `battery` (`sprd,sprd-battery`) | `drivers/power/sprd_battery.c:2650-2990` (`sprdbat_parse_dt`) | every stock property name is still read; the missing ones (`cap-valid-range-poweron`, `only-vol-mode`, `cnom-track-support`, `charge-vol-tab*`, `discharge-vol-tab*`, `battery-det-gpios`, `battery-id-gpios`, `chgr-otp-*`) are optional with defaults. OK |
| Charger / FGU | `sprd,sc2721-charger`, `sprd,sc2721-fgu` | `drivers/power/sprd_2721_charge.c:1136`, `sprd_27xx_fgu.c:1938-1947` | FGU needs `ocv-type` and `reg`: present. `sprd,cv-override-microvolt` is a property added in this tree, stock has none (cosmetic, optional). OK |
| Charger | `charger@6a` `fairchild,fan54015_chg` | `drivers/power/fan54015.c:478,483` | OF table has the typo `"fairchild, fan54015_chg"` (space), so the OF match fails, but the I2C core falls back to the id-table `fan54015_chg` (`:478`). Works. cosmetic. Proposed fix is in the driver (remove the space), not the DTS |
| PMIC | `sc2721-regulator` children (`dcdc@N`, `ldo@N`) | `drivers/regulator/sc2721-regulator.c:700-760` | `sprd,step-microvolt` and `regulator-min/max-microvolt` are **mandatory**: `:757-759` divides by `uV_step`, so a missing value would crash probe (boot-blocking). All 24 children on all six watches have them. `reg`, `sprd,default-microvolt`, `sprd,hide-offset`, `sprd,default-on` are optional; the tool's "MISSING" lines are those. OK |
| Thermal | `sprd,r2p0-thm`, `cooling-devices/*` | `drivers/thermal/sprd_cpu_device.c:582-606,707` (found by node name `cooling-devices`, alias `cooling-device`) | `sprd,cluster-base` / `sprd,cluster-temp-scale` present (2 matches per DT). The compatible `sprd,cluster-cooling` has no `of_match`, bound by name. expected |
| USB | `usb@...` (musb), `hsphy`, `typec` | `drivers/usb/musb/musb_sprd.c:1485-1571`, `phy-sprd-musb.c:411-468`, `phy-sprd-typec.c:159,908` | Mandatory and present on all six: `sprd,vbus-gpios` (`:1527`), `sprd,cable-detection-method` (`:1485`), `usb-phy` (`:1499`); hsphy `sprd,tune-value`, `sprd,vdd-voltage`, `vdd-supply`, `sprd,syscon-enable` (`phy-sprd-musb.c:411-468`). Optional: `sprd,otg-gpios`, `sprd,usb-id-gpios`, `sprd,efuse-*`, `ldo-gpios`, `usb-2.0-only`. OK |
| Camera | `sensor-main/sub`, `dcam`, `isp`, `cpp`, `csi`, `mipi-csi-phy` | `sprd_camera/sensor/sprd_sensor_drv.c:208-246` | `sprd,csi` is read on the CSI-phy node and is present (2 per DT); the "missing" on the sensor node is a false positive. OK |
| Audio | `vbc`, `i2s`, `sprd,sc2721-audio-codec`, `saudio`, `audio-mem-mgr`, `sound@1` | `sound/soc/sprd/...`, `drivers/misc/sprd_audio/audiomem/audio_mem.c:398-570` | optional/other-node reads only. OK |
| Input | `hrs3xxx`, `sc7a22`, `qmax981`, `cst0xx_ts`, `hyn_ts` | `hrs3xxx.c:271`, `sc7a22.c:577`, `cst0xx_ts.c:277-317`, `hynitron_cst9217.c:283-342` | mandatory props (`power-gpio`, `vdd_name`, `TP_MAX_X/Y`, `hynitron,*-gpio`, `hynitron,display-coords`) all present. OK |

### Stock compatibles with no `of_match` in this tree

Expected (no driver is meant to bind, or it is not an OF platform driver):

| Compatible | Reason |
|---|---|
| `sprd,sp9832e-1h10`, `sprd,sc9832e` (root), `arm,cortex-a53`/`arm,armv8` (cpus), `arm,gic-400`, `arm,cortex-a53-pmu`, `android,*` (firmware/fstab/vbmeta/CustomIPI) | machine, core and firmware nodes consumed by core code |
| `sprd,sys-*`, `sprd,anlg_*`, `sprd,*-clock`, `sprd,sc1000-*-clock`, `fixed-clock`, `fixed-factor-clock`, `arm,coresight-*` | syscon / clock / debug nodes: bound by `syscon`, `CLK_OF_DECLARE`, or by node lookup, not by a platform driver |
| `sprd,deep-sleep`, `sprd,cluster-cooling` | found by node name (`pm-sharkl*.c`, `sprd_cpu_device.c:707`) |
| `sprd,sc2721-headset` / `sc2731-headset` | consumed by the SC2721 codec headset code (`sprd-headset-sc2721.c`, `sprd-asoc-card-utils.c`), not an of_match |
| `sprd,csi-controller`, `sprd,mipi-csi-phy`, `sprd,gsp-core` | camera / GSP code finds them by phandle (documented in `Documentation/devicetree/bindings/soc/sprd/sprd,camera.txt`, `media/sprd-gsp.txt`) |
| `goodix,fingerprintree`, `heartrate,bd1xxx`, `pressure,msp891`, `compass,af6837`, `btl,btl_ts` | closed vendor drivers, not in this source (see `devices/*/SPEC.md`, `TODO.md`); the watches work without them |
| `android,trusty-*` | `TRUSTY` core is built; `trusty-log` (`drivers/trusty/trusty-log.c:379`) is not enabled on some watches; harmless |

Unexpected (worth a look, none breaks boot):

| Compatible | Watches | Note |
|---|---|---|
| `sprd,sharkl2-audio-codec` (`audio-codec@40000000`) | all | no driver or lookup in the tree. The SC2721 PMIC codec (`sprd,sc2721-audio-codec`) is what `SND_SOC_SPRD_CODEC_SC2721` binds; the AON-side codec node is not used. Check that VBC/DAI routing does not need it (it is read by name nowhere). feature broken only if the AON codec path is wanted |
| `sprd,v1-keypad` (`keypad@40250000`) | MT108, DM82 | `drivers/input/keyboard/sprd_keypad.c:409` exists but `KEYBOARD_SPRD` is not set in the MT108/DM82 configs. The stock node has no key child nodes (the driver counts child keys at `:367`), so it is not needed; hardware keys come from `gpio-keys`. cosmetic |
| `magnetic,och1970` (`och1970@0d`) | DW100U (both) | `drivers/input/misc/och1970.c:455` exists, `SENSORS_OCH1970` is enabled for MT107/MT108/DM82 but not for the DW100U configs. `devices/dw100u-silver/SPEC.md` marks it "unbound, not needed". If the hall sensor is wanted: add `CONFIG_SENSORS_OCH1970=y` to the DW100U fragments (config, not DTS) |
| `pwm-backlight`, `QST,qmax981`, `cst0xx,cst0xx_ts` / `hynitron,hyn_ts`, `tyhx,hrs3xxx`, `sprd,sc7a22` | per watch | driver exists but its Kconfig is off in that watch: by design for the touch ICs (one per watch); `QST,qmax981` (`qma6981.c`) is enabled only by `dm82.fragment`, so DM63/DW100U/MT107/MT108 leave that accelerometer unbound (checked against `devices/*/SPEC.md` before enabling) |

## Per-watch deltas

Common rows above apply to all. Differences between the six DTs:

| Watch | Delta | Severity | Driver code | Minimal DTS fix |
|---|---|---|---|---|
| DM63 | touch candidates `cst0xx_ts` (built), `bl_ts` (no driver), `hynitron@5a` (built since the fragment change below). Probing is by I2C address; two nodes share the same reset/irq GPIOs | cosmetic | `cst0xx_ts.c:451`, `hynitron_cst9217.c:472` | none for the DTS; fragment done: `CONFIG_TOUCHSCREEN_CST9217=y` added to `dm63_touch.fragment` (as `dm82.fragment` already does), `arm64/configs/dm63_defconfig` regenerated; both drivers fail probe cleanly when their chip is absent |
| DW100U silver / v13m | `cst0xx_ts@38` not built (touch is `hyn_ts`, `dw100u_touch.fragment`); `och1970` not built; `goodix,fingerprintree` no driver. The two DTs differ from each other only in non-bound details (same compatible set) | cosmetic | `och1970.c:455` | none |
| MT107 | `heartrate,bd1xxx`, `pressure,msp891`, `compass,af6837` closed drivers | cosmetic | n/a | none |
| MT108 | `sprd,v1-keypad` not built; `bd1xxx` closed; `sprd,cable-detection-method = "gpios"` instead of `"typec"` | cosmetic | `musb_sprd.c:1485-1531` | none: the "gpios" path also needs `sprd,vbus-gpios`, present |
| DM82 | same as MT108 (keypad, `gpios` cable detection, `bd1xxx`); `cst0xx_ts`+`hyn_ts` both built by `dm82.fragment`, one fails probe by design | cosmetic | as above | none |

## Watch the following (not mismatches, but what to verify on hardware)

1. **Panel selection (display, boot visible).** `sprd_panel.c:773-799` picks the panel by the bootloader `lcd_id=`
   against `lcd<hex>` node names; an ID with no node returns NULL with "get panel failed" and the screen stays
   dark. Every ID the bootloader can pass must exist in the `lcds` list of that watch's DT (verified structurally,
   not against hardware).
2. **Non-OLED panels have no backlight path** (`BACKLIGHT_PWM` off, like the stock config): same behaviour as
   stock, but different from `sprd_sharkle_defconfig`.
3. **AOD on non-CO5300 panels** (above): DCS commands are hard-coded; unverified.
4. **fan54015 OF string** typo (works via I2C id).
5. **WCN/GNSS**: bindings match; the remaining GNSS risks are firmware/timing, not DT. Relevant nodes are
   `cpwcn_gnss`, `gnss_common_ctl` and the SIPC `sipc@...` for the GNSS channel, all verified above.

## Reproducing

```
OUT=$(mktemp -d); scripts/watch_build.sh dm63 "$OUT"   # or merge the fragments + olddefconfig into a mktemp dir
python3 -I -B tools/dt_driver_check.py arch/arm64/boot/dts/sprd/sp9832e-1h10-dm63.dts "$OUT/obj-dm63/.config" --src .
```

Tags in the output: `NODRV`, `NOTBUILT`, `UNKGATE` (gate not decidable from the Makefile: external
modules, `ifdef`), `MISSING`, `CELLS`. Treat `MISSING` / `CELLS` as leads, not as findings.
