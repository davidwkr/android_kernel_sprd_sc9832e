# MT107

Specs derived only from the stock boot partition dump (`boot.bin`), its embedded config and its DT, plus the partition XML and two project notes. Nothing here was read from a running unit, so "bound on the unit" facts are **not verified**: which of the DT candidates are really populated is open. Common platform: [`../README.md`](../README.md).

## Identity

| | |
|---|---|
| Stock kernel | 4.4.83, `#1 SMP PREEMPT Fri Feb 14 13:18:42 CST 2025`, built as user and host `powersys`, GCC 4.9.x (the DM63 and DW100U kernels were built as `power` on host `power-X99`, Dec 2025 / Jan 2026: this is an older, separate build) |
| SoC | UNISOC SL8541E (SC9832E), model `Spreadtrum SL8541e Board`, compatible `"sprd,sp9832e-1h10", "sprd,sc9832e"`, `androidboot.hardware=sl8541e_1h10` |
| Boot image | `ANDROID!` v0, page 2048, uncompressed arm64 Image 17 953 952 B, ramdisk 1 375 254 B, no second stage, `SPRD` DT table 131 072 B holding **1 DTB** (128 314 B) |
| Header cmdline | `console=ttyS1,115200n8 buildvariant=user` (same as the other devices; U-Boot appends `lcd_id`, `lcd_size`, `ltemode`, ...) |
| DT bootargs | `earlycon=sprd_serial,0x70100000,115200n8 console=ttyS1,115200n8 loglevel=1 init=/init root=/dev/ram0 rw androidboot.hardware=sl8541e_1h10` |
| Memory node | 2 GB at `0x80000000` (U-Boot adjusts it) |
| Partitions | 38 entries in `mt107_part.xml` (sizes in MB): `boot` 35, `recovery` 35, `system` 4300, `vendor` 400, `cache` 150, `l_modem` 25, `l_ldsp` 20, `wcnmodem` 10, `gpsgl`/`gpsbd` 1 each, `uboot`/`trustos`/`sml` with `_bak`, `vbmeta` with `_bak`, `userdata` rest. Same set as DM63 / DW100U, plus nothing extra. |

Files: `stock.config` (IKCONFIG from the Image, 4042 lines, 1235 options enabled), `stock.dts` (the single DTB, `dtc -I dtb -O dts`, 5319 lines; dtc emitted only `unit_address_vs_reg` warnings).

**Redaction:** nothing needed redacting. The cmdline, the DT and the config contain no serial numbers, IMEIs, MAC / BT addresses or keys (checked by pattern: MAC-like, 15-digit, `serial`, `imei`, `bdaddr`, `uuid`). Nothing was replaced with `<REDACTED>`. The DT table and header SHA-1 id field were not copied.

Single DTB, so there is no per-board selection; the board is the 1H10 one, same compatible as the DM63 / DW100U. The panel is selected at run time by `lcd_id` (U-Boot).

## Display

- **Panel controller confirmed from the stock U-Boot log** (`uboot_log.bin`): U-Boot runs `co5300_mipi_init` and `co5300_readid` reads ID `0x33 0x11`, so the panel is a **CO5300** (412x502 per the touch config, so `lcd_id` 5300213 or 5300206; the exact node is still to read from `/proc/cmdline`).

- DT `lcds { }` holds **19 panel nodes**; the node unit address is the `lcd_id` (the `ID` prefix in the cmdline is added by U-Boot, e.g. `ID5300213`). All DSI, 1 data lane except the dummy HD one.
- The actual `lcd_id` of the unit is **unknown** (needs `/proc/cmdline` or the U-Boot log). Touch config (`TP_MAX_X/Y` 412x502, `hynitron,display-coords` 412 502) points to a **412x502** panel.

| lcd_id (unit address) | panel-name | active | pixel clk (kHz) | fps | AOD cmds |
|---|---|---|---|---|---|
| 5300213 | `lcd_co5300_oled213_mipi` | 412x502 | 460000 | 60 | yes |
| 5300206 | `lcd_co5300_oled206_mipi` | 412x502 | 409000 | 60 | yes |
| 53001952 | `lcd_co5300_oled1952_mipi` | 412x502 | 409000 | 60 | yes |
| 86011952 | `lcd_sh8601_oled1952_mipi` | 412x502 | 460000 | 60 | yes |
| 13613213 | `lcd_ch13613_oled213_mipi` | 412x502 | 460000 | 60 | no |
| 5300204 | `lcd_co5300_oled204_mipi` | 368x448 | 334000 | 60 | yes |
| 5300178 | `lcd_co5300_oled178_mipi` | 368x448 | 334000 | 60 | yes |
| 8601204 | `lcd_sh8601z1_oled204_mipi` | 368x448 | 306000 | 58 | yes |
| 13613 | `lcd_ch13613_oled204_mipi` | 368x448 | 306000 | 58 | yes |
| 7801 | `lcd_st7801_oled204_mipi` | 368x448 | 306000 | 60 | no |
| 3311148 | `lcd_icna3311_oled148_mipi` | 480x480 | 460000 | 60 | no |
| 7797 | `lcd_st7797_mipi` | 400x400 | 400000 | 60 | no |
| 9b71 | `lcd_gc9b71_mipi_boe1389` | 360x360 | 360000 | 60 | no |
| 9b71182 | `lcd_gc9b71_mipi_boe182` | 320x386 | 360000 | 60 | no |
| 77916 | `lcd_st77916_mipi_boe196` | 320x386 | 300000 | 50 | no |
| 779167 (`0x779167`) | `lcd_st77916_k7_mipi_boe196` | 320x385 | 300000 | 55 | no |
| 77916385 | `lcd_st77916_mipi_cto200` | 320x385 | 300000 | 55 | no |
| 77916200 | `lcd_st77916_mipi_boe200` | 320x385 | 300000 | 55 | no |
| 985520 | `lcd_jd9855_oled_cto20_mipi` | 320x385 | 262000 | 60 | no |
| ffff | `lcd_dummy_mipi_hd` | 720x1280 (4 lanes) | 500000 | 60 | no |

(20 `panel-drivers` phandles are listed in `sprd-panel-if`, 19 panels plus the dummy, matching the table.) Backlight: `pwm-backlight`, `brightness_max` 256, default level 25; AMOLED panels use DSI-command brightness (`oled-backlight`). The CO5300 entries carry `aod-in` / `aod-out`, so `sprd_aod_idle` can drive them.

Panel nodes use the same properties as the DM63 / DW100U DTs (`work-mode`, `init-data`, `aod-in`, `sleep-in`, ...), parsed by `sprd_panel.c`. The DT has 19 entries against 14 (DM63) and 23 (DW100U); no new controller type compared with the other two except possibly `ch13613`, `icna3311`, `jd9855`, `st7801`, `st7797`, `gc9b71`: all come from the DT table, no kernel driver per controller is required.

## Touch

Three candidates on i2c-3 (`i2c@70800000`), all `status = "okay"` or unset in the DT; U-Boot / driver probe decides.

| Addr | Node / compatible | Notes |
|---|---|---|
| 0x38 | `cst0xx_ts`, `cst0xx,cst0xx_ts` | reset gpio 145, irq gpio 144, `vdd_name = "vddsdcore"`, `TP_MAX_X/Y` 412x502 |
| 0x2d | `bl_ts`, `btl,btl_ts` | BetterLife, same gpios |
| 0x5a | `hynitron`, `hynitron,hyn_ts` | CST9217 style, 5 touches, 412x502, reset 145, irq 144 |

The touch is at 412x502, matching a 412x502 CO5300 / SH8601 panel (DM63 `ID5300213` is 410x502 with the cst0xx at 0x38 and DW100U 368x448 uses hyn at 0x5a). Which one is fitted is not determinable from the dump; both in-tree drivers are likely needed in the first build (each probes by I2C address and fails harmlessly if absent).

## Sensors (all on `i2c@70700000`, i2c-2 by alias order)

| Addr | Node / compatible | Function | GPIOs |
|---|---|---|---|
| 0x44 | `hrs3xxx`, `tyhx,hrs3xxx` | heart rate (TYHX HRS3918) | power 130 |
| 0x38 | `bd1xxx_hr`, `heartrate,bd1xxx` | heart rate (BD1xxx) | power 90 |
| 0x18 | `sc7a22_acc`, `sprd,sc7a22` | accelerometer + steps | irq 93 |
| 0x12 | `qmax981_acc`, `QST,qmax981` | accelerometer (QMA6100P layout 5) | none |
| 0x0c | `af6837_mag`, `compass,af6837` | magnetometer | none |
| 0x0d | `och1970`, `magnetic,och1970` | magnetic (hall) sensor | irq 90, rst 121 |
| 0x20 | `msp891_gas`, `pressure,msp891` | pressure / gas sensor | none |

Note: heart rate pins overlap (GPIO 90 is `bd1xxx_hr` power and `och1970` irq): the HR and magnetic parts are alternatives or variants, not all fitted. The DM63 has the HRS on i2c-1 and the DW100U on i2c-2; here all sensors are on i2c-2 and `i2c@70600000` is disabled. This is the first of the three DTs with `bd1xxx` and `msp891` enabled.

## Charger, PMIC, fuel gauge

- PMIC SC2721 on ADI (`spi@40380000`): `sc2721-charger` (0xec0), `sc2721-fchg`, `sc2721-chg-timer`/`-chg-wdt`, `sc2721-fgu` (0xa00, `ocv-type` 1), ADC, regulators, RTC, EIC, vibrator, `bltc-rgb`, `kpled`, 7s reset, poweroff, `sprd,typec` (0x380, mode 2).
- Charge path is the SC2721 internal charger. An external `fairchild,fan54015_chg` (i2c-4, 0x6a, fault gpio 8, vbus-det EIC 0) and the `sprd,flash-ocp8137` flash IC (0x63) sit under `i2c@70900000`, which is **`status = "disabled"`**, so neither is used. (`CONFIG_CHARGER_FAN54015=y` in the config; harmless.)
- Battery (`sprd,sprd-battery`):

| | |
|---|---|
| Charge end voltage | 4395 mV (`chg-end-vol` 0x112b); full-check 4390 mV; safety 4480 mV |
| Recharge voltage | 4320 mV |
| Nominal capacity (`cnom`) | 730 mAh |
| Internal resistance (`rint`) | 330 mOhm; `rsense` 200 |
| Charge current CDP/DCP/SDP | 420 mA (unknown adapter 400 mA); limits CDP 1500 / DCP 3000 / SDP 500 mA |
| Battery NTC (`temp-support`) | 0: no battery temperature input |
| JEITA CC/CV table | CV 4400 mV in all bands (`jeita-cccv-tab` 0x1130) |
| Low voltage | `alm-vol` 3450 mV, `soft-vbat-uvlo` 3050 mV, `alm-soc` 5 |
| OCV table | 4369 mV (100 %) ... 3400 mV (0 %), 21 points |
| `chg-timeout` / `chg-rechg-timeout` | 21600 s / 5400 s |

The 4395 mV end voltage equals the DW100U silver (V11H) values; it is not the DM63's 4480 mV. Do not use another device's DT for this unit.

## WCN, GNSS, modem

- `cpwcn_btwf` (`sprd,integrate_marlin`), `cpwcn_gnss` (`sprd,integrate_gnss`), `gnss_common_ctl`, `wcn_wifi_cmd/data0/data1`, `wcn_bt`, `wcn_fm`, SIPC (`sipc@88180000`, `sipc@8841b000`), spipe nodes. Config: `CONFIG_INTEGRATED_MARLIN2_GE2=y`, `..._SOC=y`: the GE2 GNSS path as on the other watches. GNSS firmware partitions `gpsgl` and `gpsbd` exist (1 MB each).
- Modem: `scproc@50800000`, `sipc@87800000`, `seth0..7-lte`, `sdiag-lte`, `stty-lte` (LTE, as DM63 / DW100U).
- Wi-Fi `sprd-wlan` compatible `sprd,sp9832e`; `sprdwl_ng.ko`, `sprdbt_tty.ko`, `sprd_fm.ko` and `mali.ko` must be built for the new kernel (not checked in this dump; the ramdisk was not unpacked).

## Camera

- Sensors on `i2c@70500000` (i2c-0): `sensor-main@20` (`dcam0`, reset gpio 45, pwdn 47) and `sensor-sub@21` (`dcam1`, reset 44, pwdn 46); rails `vddio`, `vddcama`, `vddcamd`, `vddcammot`; CSI / DCAM / ISP / CPP / JPG / VSP nodes present.
- Flash IC `sprd,flash-ocp8137` on the disabled i2c-4.
- Project note `MT107_CAMERA.md`: on the unit the HAL gets no ACK on i2c-0, with the stock kernel, DT and cmdline byte-identical to the dump. The note concludes ~95 % hardware (flex / connector), not software. Nothing in the DT is missing for camera support.

## Keys and crown

`gpio-keys` (`sprd-gpio-keys`): **Volume Down Key** (code 0x43 = KEY_F9, gpio EIC-async 2, debounce 50 ms), **Power Key** (0x74, PMIC EIC 1), **Function Key** (0x57 = KEY_F11, EIC-async 3, debounce 50 ms), all wakeup, level trigger. `rotary@0` (`rotary-encoder`, gpios 78 and 92, axis 0, relative): crown present, as on the DM63.

## USB

`usb@20200000` (`sprd,usb`, MUSB): `dr-mode = "peripheral"`, `sprd,cable-detection-method = "typec"`, `sprd,vbus-gpios` = PMIC EIC 0 (also `charger-det-gpios` of the battery and `vbus-det-gpios` of the unused FAN54015). `hsphy` `sprd,usb-phy` tune value 0x5af33. No `id-gpios`, no OTG role: peripheral only. Type-C controller `sprd,typec` mode 2 on the SC2721. Same as the DM63 node.

## Peripheral table (DT compatible to driver in the kwt-dev tree)

| Compatible | Driver file in tree | Status |
|---|---|---|
| `cst0xx,cst0xx_ts` | `drivers/input/touchscreen/cst0xx_ts.c` (`CONFIG_TOUCHSCREEN_CST0XX`) | yes |
| `hynitron,hyn_ts` | `drivers/input/touchscreen/hynitron_cst9217.c` (`CONFIG_TOUCHSCREEN_CST9217`) | yes |
| `btl,btl_ts` | none (stock `TOUCHSCREEN_BETTERLIFE_COB_JR`, `..._CTS0XX_*` not in source) | no: port needed only if the unit has it |
| `tyhx,hrs3xxx` | `drivers/input/misc/hrs3xxx.c` (`INPUT_SENSORS_HRS3XXX`, `HEARTRATE_HRS3918`, `watch_hrs.fragment`) | yes |
| `heartrate,bd1xxx` | none (`INPUT_SENSORS_BD1XXX` is off in the MT107 stock config) | not needed (HRS3xxx fitted on the other devices; check the unit) |
| `sprd,sc7a22` | `drivers/input/misc/sc7a22.c` (`SENSORS_SC7A22`) | yes |
| `QST,qmax981` | `drivers/input/misc/qma6981.c` (`INPUT_QMA6981_I2C`; the stock symbol is `INPUT_QMAX981`) | yes (as a sibling driver; the DM63 / DW100U note it unbound) |
| `compass,af6837` | none | not needed (unbound on DM63 / DW100U) |
| `magnetic,och1970` | none (`SENSORS_OCH1970` missing) | not needed unless the hall sensor is wanted |
| `pressure,msp891` | none (`SENSORS_MSP891` missing) | not needed unless the sensor is wanted; port if it is fitted |
| `rotary-encoder` | `drivers/input/misc/rotary_encoder.c` (`INPUT_GPIO_ROTARY_ENCODER`) | yes |
| `gpio-keys` | generic | yes |
| `fairchild,fan54015_chg` | `drivers/power/fan54015*.c` | yes, but disabled in DT |
| `sprd,flash-ocp8137` | `drivers/misc/sprd_camera/flash/flash_ic_ocp8137_drv.c` | yes, disabled in DT |
| `sprd,sensor-main` / `-sub` | UNISOC camera sensor framework (`sprd_camera`) | yes |
| `sprd,sc2721-charger`, `-fchg`, `-fgu`, `sprd,sprd-battery` | `drivers/power/sprd_2721_charge.c`, `sprd_2721_fchg.c`, `sprd_27xx_fgu.c`, `sprd_battery.c` | yes |
| `sprd,typec`, `sprd,usb`, `sprd,usb-phy` | `drivers/usb/phy/phy-sprd-typec.c`, `drivers/usb/musb/musb_sprd.c` | yes |
| `sprd,display-controller`, `sprd,dsi-controller`, `sprd-panel-if` | `drivers/video/adf/sprd/drv/` (`sprd_dispc.c`, `sprd_panel.c`) | yes |
| `sprd,integrate_marlin`, `sprd,integrate_gnss` | `drivers/misc/sprdwcn_bsp/` | yes |
| `sprd,mali-midgard` | `drivers/gpu/arm/midgard/` | yes (module in ramdisk) |
| `sprd,sc2721-bltc-rgb`, `-kpled`, `-vibrator`, `-7sreset`, `-audio-codec` | `drivers/leds`, `drivers/soc/sprd`, `drivers/misc/sprd_7sreset.c`, `sound/soc/sprd/codec/sprd/sc2721` | yes |
| SoC IP (`sprd,r8p0-i2c`, `sprd,spi-r4p0`, `sprd,sdhc-r10`, `sprd,pwm-r3p0`, `sprd,sharkle-vsp`, clocks, pinctrl, DMA, iommu ...) | present for the shared SL8541E platform | yes |

All other DT compatibles (clocks, syscons, coresight, trusty, cooling, ...) are the common SL8541E set and also appear in the DM63 / DW100U DTs. The tree also has `arch/arm64/boot/dts/sprd/sp9832e-1h10-native.dts` and `-overlay.dts`; the stock `stock.dts` should still be used for first boots.

## Kernel config comparison (`stock.config`)

Against DM63 (`../dm63/stock.config`), options that differ (all built in, `CONFIG_MODULES=y` as on the others, no `=m` entries differ):
- MT107 only: `SENSORS_MSP891=y`, `SENSORS_OCH1970=y`.
- DM63 only: `HEALTH_GOODIX`, `INPUT_FALL_OFF`, `INPUT_SENSORS_BD1XXX`.
- `RTC_START_YEAR` 2025 (MT107) vs 2026.

Against DW100U silver (`../dw100u-silver/stock.config`):
- MT107 only: `SUNWAVE_FINGERPRINT=y`.
- DW100U only: `GOODIX_TEE_FINGERPRINT`, `HEARTRATE_BD1820`, `INPUT_SENSORS_BD1XXX`, `INPUT_UINPUT`.

Same as both: `TOUCHSCREEN_CST0XX`-class (`CTS0XX_*`, `BETTERLIFE_COB_JR`), `TOUCHSCREEN_CST9217`, `INPUT_QMAX981`, `INPUT_SENSORS_HRS3XXX`, `HEARTRATE_HRS3918`, `SENSORS_SC7A22`, `SENSORS_COMPASS_AF6837`, `INPUT_GPIO_ROTARY_ENCODER`, `CHARGER_FAN54015`, `CHARGER_SC2721`, `BATTERY_SPRD`, `USB_SPRD_TYPEC`, `INTEGRATED_MARLIN2_GE2`, `ZRAM=y`. `CONFIG_CMDLINE="console=ttyAMA0"` with `CMDLINE_FROM_BOOTLOADER`.

Enabled in `stock.config` but missing from this source (`kgap.py`): `INPUT_FINGERPRINT`, `INPUT_QMAX981`, `SENSORS_COMPASS_AF6837`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `SUNWAVE_FINGERPRINT`, `TOUCHSCREEN_BETTERLIFE_COB_JR`, `TOUCHSCREEN_CTS0XX_E10_GS17`, `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`. Unlike the DM63 and DW100U lists, `INPUT_SENSORS_HRS3XXX` and `HEARTRATE_HRS3918` are **not** missing any more (ported). No fingerprint sensor node is in the MT107 DT, so `SUNWAVE_FINGERPRINT` / `INPUT_FINGERPRINT` are config leftovers.

## Existing drivers and fragments: do they apply?

| Item | Applies? |
|---|---|
| `cst0xx_ts` (`dm63_touch.fragment`) | yes if the 0x38 touch is fitted (probable: the same `cst0xx,cst0xx_ts` node as the DM63) |
| `hyn_ts` (`dw100u_touch.fragment`) | yes if the 0x5a touch is fitted; enable both in a first build |
| `hrs3xxx` (`watch_hrs.fragment`) | yes (node at 0x44, power gpio 130, i2c-2) |
| `sprd_aod_idle` (`dm63_aod.fragment`, `dw100u_aod.fragment`) | yes for 412x502 CO5300 / SH8601 panels (`aod-in` present); the double-tap wake needs one of the touch drivers; the match table in `sprd_aod_idle.c` may need the MT107 touch / panel entry (not checked) |
| CV override of `sc2721` (`cv_override_mv`) | yes, same charger path; MT107 stock CV is 4395 mV (jeita 4400): the override only lowers it, valid range 4000..4400 mV; the DM63 batt_idle target is not tested here |
| otg role | not applicable: peripheral only, no `id-gpios`, no fragment for it was found in the tree |
| `watch_hardening.fragment`, `watch_release.fragment` | device independent (not reviewed for MT107) |

## Gap list

1. **Touch:** both `cst0xx_ts` and `hyn_ts` are in the tree; BetterLife `btl_ts` (0x2d) is not. Find which touch is fitted (live `i2c-3` bind) before porting anything. If it is `btl_ts`, port the stock `TOUCHSCREEN_BETTERLIFE_COB_JR` driver.
2. **Sensors:** `msp891_gas` (pressure) and `och1970` (hall) have no driver; `bd1xxx_hr` and `af6837` have none either. Port only the ones the product needs; HR is already covered by `hrs3xxx`. Stock `INPUT_QMAX981` maps to `INPUT_QMA6981_I2C`: check the symbol rename in the config fragment.
3. **Panel:** need the real `lcd_id` (cmdline); all 19 panel nodes are in `stock.dts`, so no panel driver work is expected. AOD on non-CO5300 panels (ST7801, ICNA3311, ...) has no `aod-in` data.
4. **DT:** use `stock.dts` for the first boot; compared to DM63 the differences are in the sensor bus (i2c-2, `i2c@70600000` disabled), `hrs3xxx@44` plus `bd1xxx`, `msp891`, `och1970`, battery values (4395 mV, 730 mAh, 330 mOhm) and the dispc IOMMU iova pool (0x9600000, not 0x8000000). If the tree's own `sp9832e-1h10-*` DTS is used, it needs these nodes.
5. **Config fragment:** a new `mt107` fragment would need `TOUCHSCREEN_CST0XX=y`, `TOUCHSCREEN_CST9217=y`, `SPRD_AOD_IDLE`, `watch_hrs`, and optionally a `msp891` / `och1970` driver; `CHARGER_FAN54015` can stay off (i2c-4 disabled). Keep `INTEGRATED_MARLIN2_GE2`.
6. **Modules / ramdisk:** rebuild `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`, `sprdwl_ng.ko` for the new kernel; the ramdisk was not unpacked for this analysis, so the module list is assumed to match the DM63.
7. **Camera:** the open problem is hardware (no ACK on i2c-0 per `MT107_CAMERA.md`); no kernel gap known. The stock boot chain (`uboot`, `splloader`, `trustos`, `boot`) remains to be restored to rule out software.
8. **Unverified:** stock firmware build, which touch / HR / accelerometer is fitted, panel id, and the module set. This needs a connected unit or the cmdline from the U-Boot log (`uboot_log.bin` was not read).

## Config fragment

`arch/arm64/configs/.fragment`: . The DT compatibles (`cst0xx,cst0xx_ts`, `hynitron,hyn_ts`, `QST,qmax981`, `sprd,sc7a22`, `tyhx,hrs3xxx`) all match the tree drivers. `watch_otg.fragment` is not included. Build-checked only, not run on a unit.
