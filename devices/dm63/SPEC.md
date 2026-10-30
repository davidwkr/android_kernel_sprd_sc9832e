# DM63

Connected, with the vendor's built-in `su` (no Magisk). SELinux is **Permissive** on this unit. Common platform: [`../README.md`](../README.md).

## Identity

| | |
|---|---|
| Firmware | `DM63B_UT_64_X39_V1.3_20260120` |
| Stock kernel | 4.4.83, `#1 SMP PREEMPT Tue Jan 20 23:26:35 CST 2026`, built by `power@power-X99` |
| RAM | 1.75 GB visible (`MemTotal 1839996 kB`) |
| eMMC | 14.7 GiB (`Q313MB`) |
| Root | vendor `su` (`su 0 sh -c ...`), no Magisk. `/system` was modified before: it carries `/system/etc/init/batt_idle.rc`, `aod_idle.rc`, `dirty_expire.rc` and `/system/lib/modules/{aod_idle,batt_idle}.ko`. |

## Display

- `lcd_id=ID5300213`: CO5300 AMOLED, 2.13", 410x502 (`lcd_size=502x412` on the cmdline), density 200. Panel node `lcd5300213@5300213` in `stock.dts`.
- The DT carries 14 panel entries. U-Boot selects one by `lcd_id`.

## Peripherals (bound on the running unit)

| Bus | Address | DT node / compatible | Driver | In this source? |
|---|---|---|---|---|
| i2c-0 | 0x20 | `sensor-main` | camera sensor | yes |
| i2c-0 | 0x21 | `sensor-sub` | camera sensor | yes |
| i2c-1 | 0x44 | `hrs3xxx` / `tyhx,hrs3xxx` | `hrs3xxx` (input `heartrate`) | **no: port needed** |
| i2c-2 | 0x0c | `af6837` | unbound | no (not needed) |
| i2c-2 | 0x12 | `qmax981` / `QST,qmax981` | unbound | no (not needed) |
| i2c-2 | 0x18 | `sc7a22` / `sprd,sc7a22` | `sc7a22` (accelerometer, steps) | yes |
| i2c-3 | 0x2d | `btl_ts` | unbound | no (not needed) |
| i2c-3 | 0x38 | `cst0xx_ts` / `cst0xx,cst0xx_ts` | **`cst0xx_ts` (touch)** | **no: port needed** |
| i2c-3 | 0x5a | `hyn_ts` | unbound | not needed on this unit |
| GPIO | – | `rotary@0` | rotary crown (input `rotary@0`) | yes (`INPUT_GPIO_ROTARY_ENCODER`) |

Note: the i2c bus numbers of the sensors differ from the DW100U (heart rate on i2c-1, accelerometer on i2c-2).

Input devices: `cst0xx_ts`, `rotary@0`, `heartrate`, `accelerometer`, `sc7a22_input_key`, `sc7a22_input_step`, `sprd-gpio-keys`, headset jack, `aod_dtap` (from the custom `aod_idle` module).

Sensors HAL list: `hrs3918 HeartrateSensor` (TYHX), Game Rotation Vector and Gravity (AOSP virtual), placeholder light/proximity.

## Keys

`gpio-keys`: **Volume Down Key**, **Power Key**. Rotary crown via `rotary@0`.

## Battery and charger (from `stock.dts`)

| | |
|---|---|
| Charge end voltage | 4480 mV (`chg-end-vol` 0x1180) |
| Recharge voltage | 4415 mV |
| Nominal capacity (`cnom`) | 925 mAh |
| Internal resistance (`rint`) | 360 mOhm |
| Charge current DCP/SDP | 450 mA |
| Battery NTC (`temp-support`) | on |

The custom `batt_idle.ko` overrides the SC2721 charger CV voltage at runtime (`/sys/kernel/batt_idle/{cv_mv,enable}`; stock CV 4480 mV). Rebuild it for the new kernel, or move the feature into the charger driver.

## Modules

| Module | Location | Loaded by |
|---|---|---|
| `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko` | ramdisk `/lib/modules` | ramdisk init (`insmod`) |
| `sprdwl_ng.ko` | ramdisk `/lib/modules` | Wi-Fi HAL |
| `batt_idle.ko` | `/system/lib/modules` | `/system/etc/init/batt_idle.rc` |
| `aod_idle.ko` | `/system/lib/modules` | `/system/etc/init/aod_idle.rc` (also `aod_idled` service, `/system/bin/aod_idled.sh`) |

`aod_idle.ko` is ported in tree as `CONFIG_SPRD_AOD_IDLE` (`sprd_aod_idle`, `arch/arm64/configs/dm63_aod.fragment`; see `KERNEL_TWEAKS.md`). With `=m` the module is `sprd_aod_idle.ko`, so `aod_idle.rc` must `insmod` that name (or build `=y` and drop the insmod). `batt_idle.ko` is still out of tree. Without them the DM63 loses the charger CV override and the AOD double-tap / idle behaviour.

## Kernel config notes

Enabled in `stock.config` but missing from this source: `HEALTH_GOODIX`, `HEARTRATE_HRS3918`, `INPUT_FALL_OFF`, `INPUT_FINGERPRINT`, `INPUT_QMAX981`, `INPUT_SENSORS_BD1XXX`, `INPUT_SENSORS_HRS3XXX`, `SENSORS_COMPASS_AF6837`, `SUNWAVE_FINGERPRINT`, `TOUCHSCREEN_BETTERLIFE_COB_JR`, `TOUCHSCREEN_CTS0XX_E10_GS17`, `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`.

Required for a working watch: **CST0xx touch (port)** and HRS3xxx heart rate (port). Check `INPUT_FALL_OFF` (likely wrist-off detection).

## Bring-up order

Last: after the CST0xx touch port, since it's the only input besides the crown and the keys.
