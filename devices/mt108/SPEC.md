# MT108

Spec derived only from the stock `boot.bin` (kernel, embedded IKCONFIG, DT table) and `part_mt108.xml`. No unit was connected: nothing here is verified on hardware (no `lcd_id`, no bound-driver list, no firmware name). Common platform: [`../README.md`](../README.md).

## Identity

| | |
|---|---|
| Firmware | unknown (the system image was not examined) |
| Stock kernel | 4.4.83, `#1 SMP PREEMPT Tue Feb 25 11:38:06 CST 2025`, built by `powersys@powersys`, GCC 4.9 (older than the DM63 and DW100U kernels, which are Dec 2025 / Jan 2026 builds by `power@power-X99`) |
| Boot image | header v0, page size 2048, kernel 18 044 832 B (uncompressed arm64 `Image`), ramdisk 1 374 921 B, no second stage, SPRD DT table 129 024 B |
| Header cmdline | `console=ttyS1,115200n8 buildvariant=user` |
| DT table | magic `SPRD`, **1 DTB** (platform 9832, variant 1, 127 554 B at offset 2048 in the table), so no board selection is needed. Saved as `stock.dts` |
| DT compatible | `"sprd,sp9832e-1h10", "sprd,sc9832e"`, model `Spreadtrum SL8541e Board`, `androidboot.hardware=sl8541e_1h10` (same as the other devices) |
| RAM | DT `memory` node: 2 GB at `0x80000000` (U-Boot adjusts it; as on DW100U) |
| Partitions | `part_mt108.xml`: same set as the other three, `boot` 35 MB, `system` 4500 MB, `vendor` 400 MB, `cache` 150 MB, `recovery` 35 MB; no `userdata` size limit |

Extra files: `stock.config` (IKCONFIG from the Image, 4.4.83), `stock.dts` (dtc output of the single DTB; dtc prints only graph warnings for the camera/funnel nodes).

## Redaction

Scanned `stock.dts` and `stock.config` for MAC / BT addresses, IMEI, serial numbers, e-mail addresses, credentials: **no hits, nothing was redacted**. The header ID field (SHA digest) and the DT contain no per-unit data. NV / calibration partitions were not read.

## Display

- **Panel confirmed from the stock U-Boot log** (`uboot_log.bin`): U-Boot runs `lcd_st77916_init`, so the panel is the **ST77916** (360x360 TFT LCD, not an AMOLED). `sprd_aod_idle` (CO5300 idle-mode commands) does **not** apply to the MT108.

- `lcd_id` unknown (comes from U-Boot at boot; not in the boot image). The DT lists 19 panel nodes, node suffix = lcd_id:

| lcd_id | panel-name | active | mode |
|---|---|---|---|
| 7797 | st7797 | 400x400 | video |
| 9b71 | gc9b71 (BOE 1.39) | 360x360 | video |
| 3310143 | icna3310 oled 1.43 | 468x466 | cmd |
| 3312148 | icna3312 oled 1.48 | 480x480 | cmd |
| 5300175 | co5300 oled 1.75 | 468x466 | cmd |
| 5300143 | co5300 oled 1.43 | 468x466 | cmd |
| 13613143 / 13613150 / 13613154 | ch13613 oled 1.43 / 1.504 / 1.54 | 468x466 / 468x466 / 480x480 | cmd |
| 8021504 | st802 oled 1.504 | 468x466 | cmd |
| 8601143 / 86011508 | sh8601z oled 1.43 / 1.508 | 468x466 | cmd (ESD check on) |
| 77916150 / 77916185 | st77916 (CTO 1.50 / HSD 1.85) | 360x360 | video / cmd |
| 9855180 / 9855 / 985522 | jd9855 (BOE 1.80 / generic / oled 2.2) | 360x360 | cmd |
| 7802148 | st7802n oled 1.48 | 480x480 | cmd |
| ffff | dummy | 720x1280, 4 lanes | video |

- All real panels use 1 DSI lane. Backlight: `pwm-backlight` on the SC2721/PWM block (default level 25, max 256). Panel selection, init sequences and power-on timing are DT data (ADF `sprd-panel-if`), so the DT carries them without a driver change.
- **Likely panel** (inference, not proven): the touch node declares `TP_MAX_X/Y = 360`, which matches the 360x360 round/square group (jd9855, st77916, gc9b71). The CO5300 panels of the DM63 / DW100U are also present (5300143, 5300175, different sizes), but the 360x360 touch limit points elsewhere. Confirm `lcd_id` from a running unit or `uboot.bin` strings.

## Peripherals (from `stock.dts`; "bound" cannot be checked without a unit)

| Bus | Addr | DT node / compatible | Driver in our tree? |
|---|---|---|---|
| i2c-0 | 0x20 / 0x21 | `sensor-main` / `sensor-sub` (`sprd,sensor-main/sub`; reset gpio 45/44, pwdn 47/46) | yes (UNISOC camera) |
| i2c-1 | 0x12 | `qmax981_acc` / `QST,qmax981` | yes: `drivers/input/misc/qma6981.c`; stock config enables `INPUT_QMAX981` (not needed if sc7a22 is the fitted part) |
| i2c-1 | 0x18 | `sc7a22_acc` / `sprd,sc7a22` (irq gpio 93) | yes (`SENSORS_SC7A22`) |
| i2c-2 | 0x0d | `och1970` / `magnetic,och1970` (irq gpio 55, rst gpio 54, open/rotate thresholds, axes x1/x2/z) | **yes, basic**: `drivers/input/misc/och1970.c` (`SENSORS_OCH1970`, in `mt108.fragment`) reports raw X/Y/Z only; the open/rotate threshold logic of the stock driver is not implemented (a magnetic open/rotate sensor, likely the crown or lid detection) |
| i2c-2 | 0x44 | `hrs3xxx@0x44` / `tyhx,hrs3xxx` (power gpio 90) | yes: `drivers/input/misc/hrs3xxx.c` (accepts the HRS3918 id 0x27 and, since `HEARTRATE_HRS3300`, the HRS3300 id 0x21: probe and standby only, no measurement). Stock config enables `HEARTRATE_HRS3300`, **not** `HRS3918` |
| i2c-2 | 0x38 | `bd1xxx_hr` / `heartrate,bd1xxx` (power gpio 130) | **no: port needed** (alternative heart-rate part; stock config `HEARTRATE_BD1820`, `INPUT_SENSORS_BD1XXX`) |
| i2c-3 | 0x38 | `cst0xx_ts` / `cst0xx,cst0xx_ts` (gpios 145, 144; `vdd_name=vddsdcore`; `TP_MAX_X/Y=360`) | yes: `drivers/input/touchscreen/cst0xx_ts.c` (`TOUCHSCREEN_CST0XX`) |
| i2c-3 | 0x2d | `btl_ts` (`btl,btl_ts`) / `btl,btl_ts` (same gpios, 360x360) | no (not needed unless the unit carries a Betterlife IC; alternative touch part on the same bus) |
| i2c-4 | 0x63 | `flash-ic` / `sprd,flash-ocp8137` (bus **disabled**) | yes (driver), bus disabled |
| i2c-4 | 0x6a | `charger` / `fairchild,fan54015_chg` (bus **disabled**) | no (not needed: bus disabled, the SC2721 charger is used) |
| GPIO | - | `rotary@0` / `rotary-encoder` (gpios 78, 92, relative axis 0) | yes (`INPUT_GPIO_ROTARY_ENCODER`) |
| PMIC | - | `sprd,sc2721-{charger,fgu,fchg,vibrator,bltc-rgb,typec,adc,eic,efuse,rtc,wdt,poweroff,7sreset,regulator,audio-codec}` | yes (`sprd_2721_charge`, `sprd_27xx_fgu`, ...); `sc2721-fchg` is `status = "disabled"` |
| SoC | - | `sprd,usb` / `sprd,usb-phy`, `sprd,sprd-battery`, `sprd,sc2721-headset`, `sprd,vaudio`/`saudio`, `sprd,vbc-r1p0v3-codec-sc2721` | yes (`musb_sprd`, `sprd_battery.c`, sound/soc/sprd); not re-checked against the compatible strings |
| WCN | - | `sprd,integrate_marlin`, `sprd,integrate_gnss`, `sprd,gnss_common_ctl`, `sprd,swcnblk`, `sprd,wcn_internal_chip`, `sprd,sipc-v1`, `sprd,spipe` | yes (`drivers/misc/sprdwcn_bsp`) |
| - | - | `sprd-wlan` (`sprd,sp9832e`) | out-of-tree `sprdwl_ng.ko` from the ramdisk (as on the others) |
| TEE | - | `android,trusty-*` | not needed (TEE runs in `trustos`; DT nodes only) |

Not present on this DT (compared with DW100U / DM63): no Hynitron `hyn_ts` node, no fingerprint, no Goodix heart-rate node, no `btl_ts`-only layout. Input from the stock config but with no DT node: `TLSC6X` touch, `HEALTH_GOODIX`, `SUNWAVE_FINGERPRINT`, `INPUT_FALL_OFF`, `SENSORS_MSP891`.

## Keys and crown

`gpio-keys`: **Volume Down Key** (code 0x43 = `KEY_F9`, gpio 2 of the controller `0xe4`, active low), **Power Key** (`KEY_POWER` 0x74, PMIC EIC pin 1, active low). Rotary encoder `rotary@0` on gpios 78 and 92. No Function key (the DW100U has one).

## USB / charging inputs

`sprd,usb`: `dr-mode = "peripheral"`, `sprd,cable-detection-method = "gpios"`, `sprd,vbus-gpios` = PMIC EIC pin 0. **No `sprd,usb-id-gpios`**, so no OTG/host role and no ID pin. `sprd,typec` node present (mode 2, EIC irq 10) with `ext-ldo-sw = 0`. Charger node: `chg-cv-gpios` = EIC 4, `chg-ovi-gpios` = EIC 6; battery `charger-det-gpios` = EIC 0 (same VBUS detect).

## Battery and charger (from `stock.dts`)

| | |
|---|---|
| Charge end voltage | 4395 mV (`chg-end-vol` 0x112b); full-check 4390 mV; safety limit 4480 mV |
| Recharge voltage | 4320 mV |
| JEITA CC/CV table | CV 4400 mV in all zones, CC 0/100/500/max/700/0 mA |
| Nominal capacity (`cnom`) | 1030 mAh (temperature table 1020 mAh at 0 C, 1010 at 10 C ...) |
| Internal resistance (`rint`) | 550 mOhm |
| Charge current CDP/DCP/SDP | 450 mA (unknown adapter 420 mA); adapter current limits CDP 1500, DCP 3000, SDP 500 mA |
| Charge end current | 60 mA; timeout 21 600 s; recharge timeout 5 400 s |
| Battery NTC (`temp-support`) | 0: no battery temperature input (like the DW100U) |
| Low battery | alarm 5 % / 3450 mV, soft UVLO 3050 mV |
| Fuel gauge | `sprd,sc2721-fgu`, `fgu-mode 0`, `rsense` 200, OCV table 4374 mV -> 3400 mV (21 points) |

Same end voltage and recharge voltage as the DW100U silver (4395 / 4320 mV), but a larger cell (1030 vs 940 mAh) and a lower `rint` (550 vs 590 mOhm). The fast-charger node (`sc2721-fchg`) is disabled.

## Kernel config differences

`stock.config` is 4.4.83 like the others and differs from them in about 12 symbols.

Against DM63: MT108 only: `HEARTRATE_BD1820`, `HEARTRATE_HRS3300`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `TOUCHSCREEN_TLSC6X`. DM63 only: `BACKLIGHT_GENERIC`, `HEARTRATE_HRS3918`, `LEDS_SPRD_BLTC_RGB`, `TOUCHSCREEN_CTS0XX_E10_GS17`.

Against DW100U silver: MT108 only: `HEALTH_GOODIX`, `HEARTRATE_HRS3300` (now added, ID support only), `INPUT_FALL_OFF`, `SUNWAVE_FINGERPRINT`, `TOUCHSCREEN_TLSC6X`. DW100U only: `GOODIX_TEE_FINGERPRINT`, `HEARTRATE_HRS3918`, `INPUT_UINPUT`, `LEDS_SPRD_BLTC_RGB`, `BACKLIGHT_GENERIC`, `TOUCHSCREEN_CTS0XX_E10_GS17`.

Touch Kconfig symbols in `stock.config`: `TOUCHSCREEN_CST9217`, `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`, `TOUCHSCREEN_BETTERLIFE_COB_JR`, `TOUCHSCREEN_TLSC6X` (the same set as the other devices apart from `CTS0XX_E10_GS17`). The MT108 DT only instantiates `cst0xx_ts` and `btl_ts` (`btl,btl_ts`); which Kconfig symbol builds the driver for those two compatibles in the stock tree was not checked.

Missing from this source and enabled in `stock.config`: `HEALTH_GOODIX`, `HEARTRATE_BD1820`, `HEARTRATE_HRS3300`, `INPUT_FALL_OFF`, `INPUT_FINGERPRINT`, `SUNWAVE_FINGERPRINT`, `INPUT_QMAX981` (a different Kconfig name from `qma6981.c`; check), `INPUT_SENSORS_BD1XXX`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `SENSORS_COMPASS_AF6837`, `TOUCHSCREEN_BETTERLIFE_COB_JR`, `TOUCHSCREEN_CTS0XX_E10_GS17`, `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`. Not all are needed, see the peripheral table.

## Applicability of our drivers and fragments

| Item | Applies? |
|---|---|
| `cst0xx_ts` (`dm63_touch.fragment`, `TOUCHSCREEN_CST0XX`) | **yes**: i2c-3 0x38, same compatible and DT properties (`gpios`, `vdd_name`, `TP_MAX_X/Y`). The 360x360 limit is new; the DM63 value should be compared against the driver's coordinate handling |
| `hyn_ts` / CST9217 (`dw100u_touch.fragment`) | **no**: the DT has no `hynitron,hyn_ts` node |
| `hrs3xxx` (`watch_hrs.fragment`) | **yes, ID only**: the DT has the `tyhx,hrs3xxx` node (i2c-2 0x44, power gpio 90); the stock config selects `HRS3300`, whose ID (0x21) the driver now accepts (`mt108.fragment`); no measurement is implemented for either chip. A second HR part (`bd1xxx_hr`, i2c-2 0x38) is also described |
| `sprd_aod_idle` (`CONFIG_SPRD_AOD_IDLE`, `dm63_aod.fragment` / `dw100u_aod.fragment`) | does not apply: the panel is an ST77916 (stock U-Boot log), and the driver issues CO5300 idle-mode commands; disabled in `mt108.fragment` |
| SC2721 CV override (`cv_override_mv`) | **yes**, generic to the SC2721 charger; the stock end voltage is 4395 mV, so any override must stay below it. It is off by default |
| otg role | **not applicable**: peripheral only, no ID gpio, no OTG DT property |
| `watch_hardening` / `watch_release` fragments | generic, apply |

## Gap list

1. Confirm hardware identity on a unit or from `uboot.bin` / `system` (not extracted): `lcd_id`, actual touch IC (CST0xx at 0x38 or Betterlife at 0x2d), heart-rate chip (HRS3300 / HRS3918 / BD1820), accelerometer (SC7A22 vs QMAX981).
2. Heart rate: HRS3300 ID support is done (no measurement); port the BD1xxx driver only if step 1 shows that chip. The userspace HAL likely differs from the DM63/DW100U one.
3. `och1970` magnetic sensor: basic driver in tree (raw X/Y/Z input device, sysfs enable/delay; no thresholds or key mapping). Optional for a first boot.
4. Touch: bring up `cst0xx_ts` with `TP_MAX 360x360`; if the unit carries the Betterlife IC (`btl_ts` (`btl,btl_ts`) at 0x2d, same gpios), a driver is missing.
5. Display: no driver work expected (panels are DT data). Check that the 360x360 jd9855 / st77916 / gc9b71 init data work with the ADF driver. AOD idle needs panel-specific commands.
6. Use the MT108 `stock.dts` as the first DT (single DTB, matches the bootloader). Do not reuse DM63 / DW100U battery data: it charges to 4395 mV, 1030 mAh.
7. Rebuild `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`, `sprdwl_ng.ko` for the new kernel; check that the ramdisk (1.3 MB, from this boot image, not unpacked) loads them with `insmod`.
8. Optional features with no counterpart yet: `INPUT_FALL_OFF` (wrist-off), Goodix `HEALTH_GOODIX`, fingerprint (`SUNWAVE`): none have a DT node, likely not needed.
9. Verified boot state, SELinux and root path are unknown (no unit connected). `mt108.7z` (1.3 GB) was not extracted.

## Bring-up order

After the DM63 (it shares `cst0xx_ts`): boot with the stock DT first, then confirm `lcd_id` from the boot log.

## Config fragment

`arch/arm64/configs/mt108.fragment`: `TOUCHSCREEN_CST0XX`, `hrs3xxx` (HRS3918 and HRS3300 IDs), `sc7a22`, `QMA6981_I2C` (for `qmax981`); `TOUCHSCREEN_CST9217` (no `hyn_ts` node) and `SPRD_AOD_IDLE` (ST77916 panel) are explicitly off. Merge it after any other watch fragment. The DT compatibles (`cst0xx,cst0xx_ts`, `QST,qmax981`, `sprd,sc7a22`, `tyhx,hrs3xxx`) all match the tree drivers; `btl,btl_ts` has no driver. `watch_otg.fragment` is not included. Build-checked only, not run on a unit.
