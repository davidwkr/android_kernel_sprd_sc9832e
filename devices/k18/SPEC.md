# K18

Specs derived only from the stock boot image of one unit (header and DT-table info, its embedded config, its DT and the ramdisk file list). Nothing here was read from a running unit, so "bound on the unit" facts are **not verified**. Common platform: [`../README.md`](../README.md). The K18 is a **separate model**, not an alias of another watch; the closest is the MT107 ([`../mt107/SPEC.md`](../mt107/SPEC.md)), but the battery, sensor, panel and touch data differ. **Never use the MT107 DT (or any other model's DT) on this unit.**

## Identity

| | |
|---|---|
| Firmware build number | `K18_V10A_HM_H20_EN_ZX_HSC_FC4.4V_20260115` |
| Stock kernel | 4.4.83, built 2026-01-15 |
| SoC | UNISOC SL8541E (SC9832E), model `Spreadtrum SL8541e Board`, compatible `"sprd,sp9832e-1h10", "sprd,sc9832e"`, `androidboot.hardware=sl8541e_1h10` |
| Boot image | `ANDROID!` v0, page 2048, arm64 Image 18 020 512 B (image_size 20 066 304), ramdisk 1 374 736 B, no second stage, `SPRD` DT table 137 216 B holding **1 DTB** (135 123 B, id 0x2668); os_version 8.1.0, patch 2019-11; AVB footer and `boot` hash ok |
| Header cmdline | `console=ttyS1,115200n8 buildvariant=user` (U-Boot appends `lcd_id`, ...) |
| DT bootargs | `earlycon=sprd_serial,0x70100000,115200n8 console=ttyS1,115200n8 loglevel=1 init=/init root=/dev/ram0 rw androidboot.hardware=sl8541e_1h10` |
| Storage | **32 GB eMMC**: the partition list read by FDL2 gives `userdata` 25 288 MB + 4 531 MB fixed partitions = 29 819 MB (29.1 GiB, the usual user area of a 32 GB part). RPMB 4 MB (U-Boot log) |
| RAM | **2 GB, not confirmed**: the stock DT `memory@80000000` is 2 GB (U-Boot may rewrite it; its log does not print the size), and the system `build.prop` maps a 32 GB unit to `ram.set32g.true=2GB` |
| Reported RAM / storage | **faked by the firmware**: `ro.hsc.fake_romram=true`, `ram.set*g=8GB`, `rom.set*g=128G`; Settings and the watch UI show these, not the real sizes |

Files: `stock.config` (IKCONFIG from the Image, 4263 lines, 1237 options enabled), `stock.dts` (the single DTB, `dtc -I dtb -O dts`, 5433 lines).

**Redaction:** nothing needed redacting. The DT, the config and the boot info contain no serial numbers, IMEIs, MAC / BT addresses or keys (pattern check: MAC-like, 15-digit, `serial`, `imei`, `bdaddr`, `uuid`; the only `serial` hits are Kconfig option names). The header SHA-1 id field was not copied. The ramdisk was listed from a gzip cpio extracted (module directory only) into a temporary directory with `--no-absolute-filenames`, then deleted; no file from it was executed.

Single DTB, same 1H10 board compatible as the other devices. The panel is selected at run time by `lcd_id` (U-Boot). The unit's U-Boot log (`uboot_log` partition, 6 boots) shows `icna3311_mipi_init` and `icna3311_readid` = `0x33 0x11` every time: the panel controller is the **ICNA3311**. The log does not print the `lcd_id` itself; U-Boot has a single ICNA3311 driver, so it is one of the two ICNA3311 nodes (`3311148` or `3311214`).

## Repack round trip

Check on a copy of the stock `boot.img` (sha256 `a2a5ef273dbce95b81d22ffc4301adad1545df243336f5b4ad593991ee931955`): `unpack` then `pack` with no substitutions gives a byte-identical file (same sha256, `cmp` clean), as for the six other stock dumps. Id check OK (recipe sha1+dt).

```
python3 -I tools/sprd_bootimg.py unpack copy/boot.img dir/
python3 -I tools/sprd_bootimg.py pack dir/ rt.img
sha256sum copy/boot.img rt.img
```

A candidate image built from this: [`BOOT_IMAGE.md`](BOOT_IMAGE.md) (untested on hardware).

## Ramdisk `/lib/modules` (names and sizes)

| Module | Size (B) |
|---|---|
| `mali.ko` | 612 104 |
| `sprd_fm.ko` | 58 736 |
| `sprdbt_tty.ko` | 27 328 |
| `sprdwl_ng.ko` | 182 368 |

Same four modules as the other watches; rebuild them for the new kernel (`../README.md`). The stock ramdisk is gzip.

## Display

DT `lcds { }` holds **23 panel nodes** (22 panels plus the dummy HD one; the node unit address is the `lcd_id`). `sprd-panel-if` `panel-drivers` lists 24 phandles: the CO5300 213 node (`0xda`) is listed twice in the stock DT (harmless duplicate, left as is). All DSI, 1 data lane except the dummy.

Touch config (`TP_MAX_X/Y` 480x480, `hynitron,display-coords` 480 480) points to a **480x480** panel, i.e. one of the two ICNA3311 nodes (`3311148`, `3311214`), consistent with the ICNA3311 seen in the U-Boot log. The MT107's 412x502 touch size does not apply.

| lcd_id (unit address) | panel-name | active | pixel clk (kHz) | fps | AOD cmds |
|---|---|---|---|---|---|
| 3311214 (new) | `lcd_icna3311_oled214_mipi` | 480x480 | 460000 | 60 | yes |
| 3311148 | `lcd_icna3311_oled148_mipi` | 480x480 | 460000 | 60 | yes |
| 7801206 (new) | `lcd_st7801_oled206_mipi` | 412x502 | 409000 | 60 | yes |
| 13613206 (new) | `lcd_ch13613_oled206_mipi` | 412x502 | 460000 | 60 | no |
| 5300213 | `lcd_co5300_oled_213_mipi` (name differs from MT107) | 412x502 | 460000 | 60 | yes |
| 5300206 | `lcd_co5300_oled206_mipi` | 412x502 | 409000 | 60 | yes |
| 53001952 | `lcd_co5300_oled1952_mipi` | 412x502 | 409000 | 60 | yes |
| 86011952 | `lcd_sh8601_oled1952_mipi` | 412x502 | 460000 | 60 | yes |
| 13613213 | `lcd_ch13613_oled213_mipi` | 412x502 | 460000 | 60 | no |
| 5300204 | `lcd_co5300_oled204_mipi` | 368x448 | 334000 | 60 | yes |
| 5300178 | `lcd_co5300_oled178_mipi` | 368x448 | 334000 | 60 | yes |
| 8601204 | `lcd_sh8601z1_oled204_mipi` | 368x448 | 306000 | 58 | yes |
| 13613 | `lcd_ch13613_oled204_mipi` | 368x448 | 306000 | 58 | yes |
| 7801 | `lcd_st7801_oled204_mipi` | 368x448 | 306000 | 60 | no |
| 7797 | `lcd_st7797_mipi` | 400x400 | 400000 | 60 | no |
| 9b71 | `lcd_gc9b71_mipi_boe1389` | 360x360 | 360000 | 60 | no |
| 9b71182 | `lcd_gc9b71_mipi_boe182` | 320x386 | 360000 | 60 | no |
| 77916 | `lcd_st77916_mipi_boe196` | 320x386 | 300000 | 50 | no |
| 779167 (`0x779167`) | `lcd_st77916_k7_mipi_boe196` | 320x385 | 300000 | 55 | no |
| 77916385 | `lcd_st77916_mipi_cto200` | 320x385 | 300000 | 55 | no |
| 77916200 | `lcd_st77916_mipi_boe200` | 320x385 | 300000 | 55 | no |
| 985520 | `lcd_jd9855_oled_cto20_mipi` | 320x385 | 262000 | 60 | no |
| ffff | `lcd_dummy_mipi_hd` | 720x1280 (4 lanes) | 500000 | 60 | no |

(Active sizes are `hactive` x `vactive` of the `display-timings`; fps from the `fps` property.) Compared with the MT107 DT the three new nodes are `lcd7801206`, `lcd3311214`, `lcd13613206`; the ICNA3311 148 node gained `display-timings`, `oled-backlight` and `aod-in` / `aod-out`, and the CO5300 213 node has different `init-data` and brightness commands. Backlight: `pwm-backlight`, AMOLED panels use DSI-command brightness (`oled-backlight`). No new controller type needs a kernel driver (panel data is all DT, parsed by `sprd_panel.c`).

## Touch

Three candidates on i2c-3 (`i2c@70800000`, 100 kHz), as on the MT107; U-Boot / driver probe decides.

| Addr | Node / compatible | Notes |
|---|---|---|
| 0x38 | `cst0xx_ts`, `cst0xx,cst0xx_ts` | reset gpio 145, irq gpio 144, `vdd_name = "vddsdcore"`, `TP_MAX_X/Y` 480x480 |
| 0x2d | `bl_ts`, `btl,btl_ts` | BetterLife, same gpios, 480x480 |
| 0x5a | `hynitron`, `hynitron,hyn_ts` | CST9217 style, 5 touches, 480x480, `status = "okay"` set explicitly |

## Sensors

Two buses, unlike the MT107 (all on i2c-2):

`i2c@70600000` (i2c-1, **enabled here, 400 kHz**; disabled on the MT107):

| Addr | Node / compatible | Function | GPIOs |
|---|---|---|---|
| 0x12 | `qmax981_acc`, `QST,qmax981` | accelerometer (QMA6100P layout 5, `layout` 1) | none |
| 0x18 | `sc7a22_acc`, `sprd,sc7a22` | accelerometer + steps (`layout` 1, `angle` 0x75) | irq 93 |

`i2c@70700000` (i2c-2, 100 kHz):

| Addr | Node / compatible | Function | GPIOs |
|---|---|---|---|
| 0x0d | `och1970`, `magnetic,och1970` | magnetic (hall) sensor, open / rotate thresholds (`open-bop` -5000, `open-brp` -6000, `rotate-bop` -1500, `rotate-brp` -1800, axes `x1` / `y2` / `z`) | irq 154, rst 155 |
| 0x44 | `hrs3xxx`, `tyhx,hrs3xxx` | heart rate (TYHX HRS3918 / HRS3300 family) | power 55 |
| 0x38 | `bd1xxx_hr`, `heartrate,bd1xxx` | heart rate (BD1xxx / BD1820), `vdd_name = "vddcammot"` | power 55 (same pin as `hrs3xxx`) |

The two heart-rate nodes share GPIO 55: alternatives or variants, not both fitted. **The fitted chip is the Rohm BD1820** (from the stock system and vendor images, see below), so `hrs3xxx` is the unused alternative on this unit. Not in this DT (present on the MT107): `msp891_gas` (pressure) and `af6837_mag`. **No `rotary@0` node.**

### From the stock system and vendor images

Read with `debugfs` from the `system` and `vendor` dumps (no mount):

- `system/build.prop`: `ro.product.model=M99`, `ro.product.brand=hesc`, `ro.product.name=sl8541e_1h10_oversea`, `ro.build.display.id=K18_V10A_HM_H20_EN_ZX_HSC_FC4.4V_20260115`, and **`heartrate.spo2=true`** (the stock heart-rate service reports SpO2). `vendor/build.prop`: `jx.heartrate=false`.
- `vendor/lib/hw/sensors.sp9832e.so` supports only **`bd1820 HeartrateSensor`**, with the BD1820 SpO2 code in the HAL (`BD_Spo2_math`, `spo2_fifo_result`, `in_spo2_adcdata`). It has no TYHX HRS support; the MT107 HAL is the opposite (`hrs3300/3602/3605/3918` only). The SpO2 math runs in the HAL, so a kernel driver only has to deliver the raw PPG data in the stock interface.
- The accelerometer is reported under a generic name (`ST Xr 3-axis Accelerometer`), so SC7A22 vs QMA981 stays open. The HAL's light/proximity classes (EPL2182, TMD2771, AL3006, LTR558) are generic UNISOC leftovers with no DT node: none fitted.

## Charger, PMIC, fuel gauge

Same PMIC SC2721 / `sc2721-charger` / `-fchg` / `-fgu` path as the MT107; the FAN54015 and `ocp8137` flash on `i2c@70900000` are `status = "disabled"` (not used). USB, WCN, GNSS and modem nodes are identical to the MT107 (peripheral only, `typec`; integrated Marlin / GNSS; LTE `scproc`).

Battery (`sprd,sprd-battery`), **K18 stock values (never the MT107's):**

| | K18 | MT107 (for contrast) |
|---|---|---|
| Nominal capacity (`cnom`) | 1069 mAh (0x42d) | 730 mAh |
| Internal resistance (`rint`) | 280 mOhm (0x118); `rsense` 200 | 330 mOhm |
| Charge current CDP/DCP/SDP | 1050 mA (0x41a) | 420 mA |
| Unknown adapter current | 1000 mA (0x3e8) | 400 mA |
| Current limits CDP / DCP / SDP | 1500 / 3000 / 500 mA (unchanged) | same |
| Charge end voltage | 4395 mV (`chg-end-vol` 0x112b), full-check 4390 mV, safety 4480 mV (unchanged) | same |
| Recharge voltage | 4374 mV (0x1116) | 4320 mV |
| `jeita-cccv-tab` | 4400 mV in all six bands (0x1130) (unchanged) | same |
| `temp-support` | 1: battery NTC read (`temp-tab-val` / `temp-tab-temp` tables new) | 0 |
| `chgr-otp-stop` | 1580 (0x62c), new property | none |
| OCV table | 4374 mV (100 %) ... 3400 mV (0 %), 21 points (slightly different from MT107) | 4369 mV |

JEITA (`jeita-temp-tab` 900 1000 1100 1450 1500, unchanged vs MT107): with the stock convention (0.1 C units, offset 1000, as in `temp-tab-temp` 750..1600 = -25..+60 C) the edges are -10, 0, 10, 45 and 50 C. `jeita-cur-tab` = 0 / 700 / 900 / 0x7fff / 800 / 0 mA:

| Band | Charge current |
|---|---|
| below -10 C | 0 (no charge) |
| -10 to 0 C | 700 mA |
| 0 to 10 C | 900 mA |
| 10 to 45 C | no JEITA limit (0x7fff; adapter current applies, 1050 mA) |
| 45 to 50 C | 800 mA |
| above 50 C | 0 (no charge) |

(The MT107 table is 0 / 100 / 500 / no limit / 700 / 0 mA.) The driver convention for the edges was not re-checked against `sprd_battery.c` for this unit. Low voltage `alm-vol` 3450 mV, `soft-vbat-uvlo` 3050 mV, timeouts 21600 s / 5400 s: unchanged.

## Keys and crown

`gpio-keys` (`sprd-gpio-keys`): **Volume Down Key** (code 0x43 = KEY_F9, gpio EIC-async 2, debounce 50 ms), **Power Key** (0x74, PMIC EIC 1), **Function Key** (0x57 = KEY_F11, EIC-async 3, debounce 50 ms), all wakeup, level trigger: identical to the MT107. **There is no rotary encoder node**: the crown is decorative / push only. `CONFIG_INPUT_GPIO_ROTARY_ENCODER=y` is still in the stock config (leftover); the driver is inert without the node.

## Modem RF profile

From the unit's NV (analysed separately, not in this repo): LTE FDD B1/2/3/4/5/7/8/12/20/28/66 and TDD B34/39/40/41, all calibrated; WCDMA B1/2/5/8. Same profile as the MT108, DM82 and DM63.

## Peripheral table (DT compatible to driver in this tree)

Same as the MT107 table except:

| Compatible | Driver in tree | Status |
|---|---|---|
| `heartrate,bd1xxx` | `bd1xxx.c` (`INPUT_SENSORS_BD1XXX`, `HEARTRATE_BD1820`), interface only | **fitted chip; partial**: probe, power, ID check and the stock HAL interface (`heartrate` input device, `enable` / `chipinfo`, `/dev/bd1662rate`) exist, but no measurement back end, so heart rate and SpO2 still give no data. Interface and open items: `devices/BD1XXX_INTERFACE.md`. Also in the 4.14 tree |
| `btl,btl_ts` | none | missing, only needed if fitted |
| `magnetic,och1970` | `drivers/input/misc/och1970.c` (`SENSORS_OCH1970`) | yes (polled; the stock HAL's thresholds and open / rotate logic are not implemented) |
| `pressure,msp891`, `compass,af6837` | none | no node in this DT: not needed |
| `rotary-encoder` | `rotary_encoder.c` | no node: not needed |

## Kernel config comparison (`stock.config`)

Against the MT107 `stock.config` the enabled options differ only by `CONFIG_INPUT_SENSORS_BD1XXX=y` and `CONFIG_HEARTRATE_BD1820=y` (now provided by the interface-only `bd1xxx.c`). Everything else (touch, `INPUT_QMAX981`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `SENSORS_COMPASS_AF6837`, `INPUT_GPIO_ROTARY_ENCODER`, `SUNWAVE_FINGERPRINT`, `CHARGER_FAN54015`, ...) is identical; `MSP891` and `AF6837` stay in the config although the DT has no node.

## Differences vs MT107

- No `rotary@0`; no `msp891_gas`, no `af6837_mag`.
- Sensors moved: `qmax981` and `sc7a22` on the now-enabled i2c-1 (layouts 1), `och1970` / `hrs3xxx` / `bd1xxx_hr` on i2c-2 with other GPIOs (och1970 irq 154 / rst 155, HR power 55; MT107: 90 / 121 / 130, BD1xxx power 90) and no `linux,front` / `linux,back` on the hall sensor, new open / rotate thresholds.
- 480x480 touch (MT107: 412x502); three extra panel nodes; CO5300 213 and ICNA3311 148 nodes changed.
- Battery and charging values (table above), `temp-support` 1 and `chgr-otp-stop`.
- Config: stock adds `BD1820` and `BD1XXX` only.
- The DT phandle numbers after the first changed node are shifted; the labelled DT keeps the stock numbers.

## Gap list

1. **Touch:** `cst0xx_ts` and `hyn_ts` are in the tree; `btl_ts` is not. Find which one answers (i2c-3 bind) before porting anything.
2. **Heart rate:** the fitted chip is the BD1820 (stock vendor HAL). `bd1xxx.c` provides the stock interface but no measurement yet (chip init register order, sample read and polling still to recover, `devices/BD1XXX_INTERFACE.md`), so heart rate and SpO2 give no data; `hrs3xxx` covers only the unused node.
3. **Panel:** controller confirmed as ICNA3311 from the U-Boot log (148 or 214 node; the exact `lcd_id` is not logged). `SPRD_AOD_IDLE` is **not** enabled in `k18.fragment` until it is known (DT candidates include non-CO5300 controllers); enable it for the ICNA3311 / CO5300 / SH8601 nodes with `aod-in` once confirmed.
4. **Modules:** rebuild the four ramdisk modules for the new kernel.
5. **Unverified:** everything above is from the dumps (no running unit): touch, accelerometer and the NV-derived modem profile. Heart-rate chip (BD1820) from the vendor HAL and panel controller (ICNA3311) from the U-Boot log.

## Config fragment

`arch/arm64/configs/k18.fragment` (and the generated `k18_defconfig`): `TOUCHSCREEN_CST0XX`, `TOUCHSCREEN_CST9217`, `hrs3xxx` (HRS3918), `sc7a22`, `QMA6981_I2C` (for `qmax981`), `SENSORS_OCH1970`; `SPRD_AOD_IDLE` off; no rotary option of its own (parity keeps the inert driver); `INPUT_SENSORS_BD1XXX` + `HEARTRATE_BD1820` (interface-only driver). Build-checked only, not run on a unit.
