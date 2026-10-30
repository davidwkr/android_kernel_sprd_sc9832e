# DM82

Static analysis of the stock `boot` partition dump (`dm82-stock/boot.bin`) and the partition table (`dm82_stock.xml`). **No live unit was examined**: everything below comes from the stock device tree and kernel config, not from a running watch. Items marked "unverified" need a unit (bound drivers, `lcd_id`, which touch chip is fitted). Common platform: [`../README.md`](../README.md).

## Identity

| | |
|---|---|
| Stock kernel | 4.4.83, `#1 SMP PREEMPT Tue Apr 15 20:40:01 CST 2025`, GCC 4.9.x, built by user/host `powersys` (the DM63 and DW100U were built by `power` on `power-X99`) |
| Boot image | `ANDROID!` v0, page 2048, kernel 18077216 bytes (uncompressed arm64 Image), ramdisk 1374868 bytes, DT table 108544 bytes |
| DT table | `SPRD` table, version 1, **1 entry** (one DTB, 105108 bytes). It is the board's DT: `stock.dts` |
| Header cmdline | `console=ttyS1,115200n8 buildvariant=user` (same as the others) |
| DT compatible / model | `"sprd,sp9832e-1h10", "sprd,sc9832e"`, `Spreadtrum SL8541e Board`; DT `chosen/bootargs` has `androidboot.hardware=sl8541e_1h10` |
| RAM | DT `memory` node: 2 GB at `0x80000000` (U-Boot adjusts it at boot, as on the DW100U) |
| Partitions | same layout as the others (`boot` 35 MB, `recovery` 35 MB, `system` 4.4 GB, `vendor` 400 MB); the dump has `logo`..`logo_4` and no `logo_5/6`, `vbmeta*` present |
| PMIC | SC2721 (`sprd,sc2721`), same as the others |

Extracted files: `stock.config` (1235 options enabled, IKCONFIG from the Image, `RTC_START_YEAR=2025`) and `stock.dts` (dtc output of the single DTB).

Redaction: the DT, the config and the header cmdline were scanned for serial numbers, IMEI, MAC/BT addresses, per-unit IDs and keys. **None were present**, so nothing was replaced. The dump has no `/proc/cmdline` (U-Boot adds `lcd_id`, `androidboot.serialno` etc. at boot, not stored in the image). The builder string is written as `powersys` without the host separator. The other partitions in the dump directory (`prodnv`, `l_fixnv*`, `miscdata`, `persist`, ...) were not opened.

## Display

- **Panel controller confirmed from the stock U-Boot log** (`uboot_log.bin`): U-Boot runs `co5300_mipi_init` and `co5300_readid` reads ID `0x33 0x11`, so the panel is a **CO5300** (most likely `ID5300175`, 1.75" 468x466).

- The DT has **9 panel entries** (`lcds { ... }`); U-Boot picks one by `lcd_id` (not in the dump). The DM63 has 14 entries, the DW100U 23.

| DT node (= `lcd_id` value) | `panel-name` | Size | Mode / lanes |
|---|---|---|---|
| `lcd5300175@5300175` | `lcd_co5300_oled175_mipi` | 468x466, 68x121 mm field, 60 fps | `work-mode 0` (command), 1 lane |
| `lcd3310143@3310143` | `lcd_icna3310_oled143_mipi` | 468x466 | command, 1 lane |
| `lcd13613143@13613143` | `lcd_ch13613_oled143_mipi` | 468x466 | command, 1 lane |
| `lcd8601143@8601143` | `lcd_sh8601z_oled143_mipi` | 468x466 | command, 1 lane |
| `lcd7797@7797` | `lcd_st7797_mipi` | 400x400 | video, 1 lane |
| `lcd9b71@9b71` | `lcd_gc9b71_mipi_boe1389` | 360x360 | video, 1 lane |
| `lcd77916150@77916150` | `lcd_st77916_mipi_cto150` | 360x360 | video, 1 lane |
| `lcd63353@63353` | `lcd_r63353_mipi_boe196` | 640x640 | video, 1 lane |
| `lcdffff@ffff` | `lcd_dummy_mipi_hd` | 720x1280 | video, 4 lanes (fallback) |

- Most likely panel: **`lcd_id=ID5300175` (CO5300 AMOLED 1.75", 468x466)**, matching the DM63/DW100U naming (`ID5300213`, `ID5300204`) and the touch coordinates (`TP_MAX_X/Y = 468/466`, `hynitron,display-coords = 468 466`). The three 1.43" OLED entries have the same 468x466 geometry, so the size alone does not decide it: read `lcd_id` from `/proc/cmdline` on the unit (unverified).
- The CO5300 node has `aod-in`/`aod-out`, `need-check-esd = 0` and `oled-backlight` (brightness over DSI writes), like the DM63 CO5300 nodes. `sprd_backlight` (`pwm-backlight`) is in the DT but unused for an OLED.
- DSI: `dsi@21800000` + DPHY + `dispc@20800000` (ADF), same IP as the others.

## Touch

The DT carries three candidates on **i2c-3 (`i2c@70800000`, 100 kHz)**, all sharing reset/irq GPIOs `0x91` / `0x90` (`vddsdcore` supply). All three have no `status = "disabled"`, so the DT alone does not say which is fitted:

| Address | Node / compatible | Notes |
|---|---|---|
| 0x38 | `cst0xx_ts` / `cst0xx,cst0xx_ts` | `TP_MAX_X=468`, `TP_MAX_Y=466`, 3 virtual keys |
| 0x2d | `bl_ts` / `btl,btl_ts` | BetterLife, same limits |
| 0x5a | `hynitron@5a` / `hynitron,hyn_ts` | 5 touches, 468x466 |

`TOUCHSCREEN_TLSC6X` is also enabled in `stock.config` but there is **no TLSC node in the DT** (probably a leftover, or probed through board info: unverified). The DM63 uses 0x38 (CST0xx), the DW100U uses 0x5a (hyn_ts); expect one of these two (unverified, `i2cdetect` or `/proc/bus/input/devices` on the unit).

## Sensors and other i2c devices

| Bus | Address | Node / compatible | Notes |
|---|---|---|---|
| i2c-0 (`70500000`, 400 kHz) | 0x20 / 0x21 | `sensor-main` / `sensor-sub` (`sprd,sensor-main/sub`) | back and front camera (dcam0 / dcam1); sensor model is picked by the camera driver, not in the DT |
| i2c-1 (`70600000`) | 0x12 | `qmax981_acc` / `QST,qmax981` | accelerometer |
| i2c-1 | 0x18 | `sc7a22_acc` / `sprd,sc7a22` | accelerometer and step counter; `layout=1`, `angle=0x75`, irq GPIO `0x5d` (DM63: layout 6, GPIO `0x5a`) |
| i2c-2 (`70700000`) | 0x0d | `och1970` / `magnetic,och1970` | hall/magnetic sensor (open/close/back/front key codes), irq `0x37`, rst `0x36` |
| i2c-2 | 0x44 | `hrs3xxx` / `tyhx,hrs3xxx` | TYHX heart rate, power GPIO `0x5a` |
| i2c-2 | 0x38 | `bd1xxx_hr` / `heartrate,bd1xxx` | second heart-rate candidate (BD1xxx), power GPIO `0x82` |
| i2c-3 (`70800000`) | see Touch | | |
| i2c-4 (`70900000`) | 0x63 | `flash-ic` / `sprd,flash-ocp8137` | camera flash IC; **bus `status = "disabled"`** |
| i2c-4 | 0x6a | `charger` / `fairchild,fan54015_chg` | external charger; **bus disabled**, SC2721 charger is used |

No fingerprint node and no AF6837 compass node in the DT (DM63 has an AF6837 on i2c-2; DW100U a Goodix fingerprint on SPI). `SUNWAVE_FINGERPRINT`, `SENSORS_MSP891`, `HEARTRATE_BD1820` are enabled in the config but unbound.

## Keys and crown

- `gpio-keys`: **Volume Down Key** (code 0x43, PMIC-side/AP GPIO 2, debounce 50 ms) and **Power Key** (code 0x74, PMIC EIC GPIO 1). Same as the DM63.
- Rotary crown: `rotary@0` (`rotary-encoder`, GPIOs `0x4e` / `0x5c`, relative axis), **identical to the DM63** node.
- `keypad@40250000` (`sprd,v1-keypad`) is present as in the SoC dtsi.

## USB

`usb@20200000` (`sprd,usb`, MUSB, `usb20_sprd_phy`): `dr-mode = "peripheral"`, **`sprd,cable-detection-method = "gpios"`**, `sprd,vbus-gpios = <PMIC EIC 0>`. No `id-gpios`. DM63 and DW100U use `"typec"` with the same GPIO. The battery node also has `charger-det-gpios` on the same PMIC EIC line. The DM82 is peripheral-only: no OTG host role in the DT.

## Battery, charger, fuel gauge

SC2721 charger (`sprd,sc2721-charger`, `chg-cv-gpios`, `chg-ovi-gpios`), SC2721 FGU (`ocv-type 1`, `sprd,sprd-battery` node). The external FAN54015 is on a disabled i2c bus.

| | DM82 | DM63 | DW100U silver |
|---|---|---|---|
| Charge end voltage (`chg-end-vol`) | 4480 mV | 4480 mV | 4395 mV |
| `chg-end-vol-check` / JEITA CC-CV | 4450 mV | n/a | n/a |
| Safety (`chg-bat-safety-vol`) | 4500 mV | | |
| Recharge voltage | 4320 mV | 4415 mV | 4320 mV |
| Nominal capacity (`cnom`) | 1250 mAh | 925 mAh | 940 mAh |
| Internal resistance (`rint`) | 300 mOhm | 360 mOhm | 590 mOhm |
| Charge current DCP/SDP/CDP | 650 mA (unknown adapter 630 mA) | 450 mA | 650 mA |
| Charge end current | 120 mA | 60 mA | |
| Battery NTC (`temp-support`) | on (18-point table) | on | off |

`cnom-temp-tab` / `rint-temp-tab` and a 21-point OCV table are in `stock.dts`. This is the DM63's 4480 mV charge end voltage on a bigger cell: the same `cv_override_mv` use case as on the DM63 applies, but the safe override value depends on the cell (unverified).

## WCN, GNSS, modem, audio

- WCN: `cpwcn_btwf` (`sprd,integrate_marlin`), `wcn_wifi_cmd/data0/data1`, `wcn_bt`, `wcn_fm` SIPC channels, `sprd-wlan` (`sprd,sp9832e`); same Marlin2 integration as the others.
- GNSS: `gnss_common_ctl` (`sprd,gnss_common_ctl`), `sipc-gnss`, `spipe_gnss` (`sttygnss0`), `gnss-mem@88300000`. Partitions `gpsgl`, `gpsbd` present.
- Modem: `l_modem`, `l_ldsp`, `l_gdsp`, `l_fixnv*`, `l_runtimenv*`, `pm_sys`. Audio: SC2721 codec + VBC (`sprd,vbc-r1p0v3-codec-sc2721`), headset detect node `sprd-headset` (`sprd,sc2721-headset`), vibrator `sprd,sc2721-vibrator`.
- Trusty (`android,trusty-*`) nodes present, as on the others.

## Peripheral table (DT compatible -> driver in this tree?)

| DT compatible | Driver in kwt-dev | Verdict |
|---|---|---|
| `cst0xx,cst0xx_ts` | `drivers/input/touchscreen/cst0xx_ts.c` (`TOUCHSCREEN_CST0XX`, `dm63_touch.fragment`) | yes |
| `hynitron,hyn_ts` | `drivers/input/touchscreen/hynitron_cst9217.c` / `hynitron_ts` (`dw100u_touch.fragment`) | yes |
| `btl,btl_ts` | none | not needed (unbound on DM63/DW100U) unless this unit uses it: unverified |
| `tyhx,hrs3xxx` | `drivers/input/misc/hrs3xxx.c` (`watch_hrs.fragment`) | yes |
| `heartrate,bd1xxx` | none (`INPUT_SENSORS_BD1XXX`, `HEARTRATE_BD1820`) | no: port needed only if this is the fitted HR sensor; stock `hrs3xxx` is the first candidate |
| `sprd,sc7a22` | `drivers/input/misc/sc7a22.c` | yes |
| `QST,qmax981` | tree has `qma6981.c`, but `INPUT_QMAX981` is not in the source | not needed (unbound on DM63/DW100U) |
| `magnetic,och1970` | none (`SENSORS_OCH1970`) | no: port needed to use the hall sensor; DW100U SPEC says "not needed" |
| `rotary-encoder` | `drivers/input/misc/rotary_encoder.c` | yes (`INPUT_GPIO_ROTARY_ENCODER`) |
| `gpio-keys` | generic | yes |
| `sprd,sensor-main/sub`, `sprd,flash-ocp8137` | `drivers/misc/sprd_camera/...` | yes |
| `fairchild,fan54015_chg` | `drivers/power/fan54015.c` | yes (bus disabled, not used) |
| `sprd,sc2721-charger`, `-fgu`, `sprd,sprd-battery` | `sprd_2721_charge.c`, `sprd_27xx_fgu.c`, `sprd_battery.c` | yes |
| `sprd,usb`, `sprd,usb-phy`, `sprd,typec` | `musb_sprd.c`, `phy-snps-usb2.c` | yes |
| `sprd,integrate_marlin`, `sprd,gnss_common_ctl`, `sprd,integrate_gnss` | `drivers/misc/sprdwcn_bsp/` | yes |
| `sprd,dsi-controller`, `sprd-adf`, panels | `drivers/video/adf/sprd/` (panels in DT) | yes |
| `sprd,sc2721-headset` | only DT users | not needed (no headset jack on a watch) |
| `sprd,sc2721-vibrator` | `drivers/soc/sprd/sprd_vibrator.c` | yes |
| `sprd,sp9832e-1h10`, `sprd,sc9832e` | board compatible | `sp9832e-1h10-native.dts` exists in the tree: not compared |

## Stock config vs DM63 and DW100U

Only a handful of options differ (both configs are the same 4.4.83 BSP):

| Option | DM82 | DM63 | DW100U |
|---|---|---|---|
| `TOUCHSCREEN_TLSC6X` | y | n | n |
| `SENSORS_OCH1970`, `SENSORS_MSP891` | y | n | y (OCH1970 only listed) |
| `HEARTRATE_BD1820` | y | n | y |
| `SUNWAVE_FINGERPRINT` | y | y | n |
| `GOODIX_TEE_FINGERPRINT`, `INPUT_UINPUT` | n | n | y |
| `HEALTH_GOODIX`, `INPUT_FALL_OFF` | n | y | n |
| `TOUCHSCREEN_CTS0XX_E10_GS17`, `LEDS_SPRD_BLTC_RGB`, `BACKLIGHT_GENERIC` | n | y | y |
| `RTC_START_YEAR` | 2025 | 2026 | |

Config options enabled but missing from this source (`kgap.py`): `HEARTRATE_BD1820`, `INPUT_FINGERPRINT`, `INPUT_QMAX981`, `INPUT_SENSORS_BD1XXX`, `SENSORS_COMPASS_AF6837`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `SUNWAVE_FINGERPRINT`, `TOUCHSCREEN_BETTERLIFE_COB_JR`, `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`. None blocks boot. `TOUCHSCREEN_TLSC6X` exists in the tree (`drivers/input/touchscreen/tlsc6x`).

## Do our drivers and fragments apply?

- `cst0xx_ts` (`dm63_touch.fragment`): yes, if the unit has the CST0xx at 0x38. Same i2c bus and address as the DM63, geometry 468x466 instead of 410x502 (driver takes `TP_MAX_X/Y` from the DT).
- `hyn_ts` (`dw100u_touch.fragment`): yes, if the unit has the Hynitron at 0x5a.
- `hrs3xxx` (`watch_hrs.fragment`): yes, `tyhx,hrs3xxx` at 0x44 (here on i2c-2, power GPIO `0x5a`). `bd1xxx_hr` at 0x38 on the same bus is the alternative: unverified which chip responds.
- `sprd_aod_idle` (`dm63_aod.fragment` / `dw100u_aod.fragment`): applies if the panel is the CO5300 (`ID5300175`) and touch is `cst0xx_ts` or `hyn_ts` (the module matches these input names). `aod-in/out` and `need-check-esd = 0` are present. The 1.43" panels (ICNA3310, CH13613, SH8601Z) are not covered by the module: unverified.
- SC2721 CV override (`cv_override_mv`): yes, same charger driver and `chg-end-vol` 4480 mV. Choose the value for the 1250 mAh cell; the stock-CV path is unchanged.
- "otg role" change: not applicable. This DT is peripheral-only with `cable-detection-method = "gpios"` and no ID pin. Check that the `gpios` method in `musb_sprd.c` / `usb_phy` is built and uses the PMIC EIC line.

## Gap list for DM82 bring-up

1. **DT**: `stock.dts` is the first-boot DT (as for the other devices). A source DT in `arch/arm64/boot/dts/sprd/` (like `dw99.dts`/`gm01.dts`) is still needed; start from the DM63 one. Do not copy DM63 battery values: use the DM82 values above.
2. **lcd_id**: read it on a live unit, make sure the CO5300 1.75" node is selected. If the unit has a 1.43" OLED, `sprd_aod_idle` has no match.
3. **Touch**: identify which of 0x38 / 0x2d / 0x5a answers, build the matching fragment. The `btl_ts` (BetterLife, `TOUCHSCREEN_BETTERLIFE_COB_JR`) driver is missing if 0x2d is the fitted chip. Look at `tlsc6x` only if `i2cdetect` shows nothing at those addresses.
4. **Heart rate**: confirm `hrs3xxx` (port exists) vs `bd1xxx` (no driver).
5. **USB gpios cable detection**: verify the code path in the MUSB driver (DM63 and DW100U take `typec`).
6. **Hall sensor `och1970`**: no driver; only needed if the open/close/front/back keys are wanted.
7. **Charger**: the 1250 mAh cell uses a 650 mA charge current and 4480 mV CV. Confirm before charging on the new kernel; apply the CV override if the same wear target as the DM63 is wanted.
8. **Modules**: `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`, `sprdwl_ng.ko` must be rebuilt for the new kernel (as on the others).
9. **Ramdisk**: 1374868 bytes, not unpacked here (outside the scope: only the Image, config and DT were read).

## Config fragment

`arch/arm64/configs/dm82.fragment`: both touch drivers (`TOUCHSCREEN_CST0XX`, `TOUCHSCREEN_CST9217`, whichever chip is fitted probes), `hrs3xxx` (HRS3918), `SPRD_AOD_IDLE` (CO5300 panel), `sc7a22`, `QMA6981_I2C` (for `qmax981`). The DT compatibles (`cst0xx,cst0xx_ts`, `hynitron,hyn_ts`, `QST,qmax981`, `sprd,sc7a22`, `tyhx,hrs3xxx`) all match the tree drivers. `watch_otg.fragment` is not included. Build-checked only, not run on a unit.
