# DW100Ultra silver (firmware V11H)

The unit used for development (connected, rooted with Magisk). Common platform: [`../README.md`](../README.md).

## Identity

| | |
|---|---|
| Firmware | V11H (TYWLD 4.4 V battery variant) |
| Stock kernel | 4.4.83, `#1 SMP PREEMPT Thu Dec 18 11:14:27 CST 2025`, built by `power@power-X99` |
| Root | Magisk (patched boot ramdisk), Magisk modules active |
| RAM | 2.74 GB visible (`MemTotal 2869116 kB`; the DT `memory` node says 2 GB at `0x80000000`, U-Boot adjusts it) |
| eMMC | 29.1 GiB (`HBG4a2`) |

## Display

- `lcd_id=ID5300204`: CO5300 AMOLED, 2.04", 368x448 (`lcd_size=448x368` on the cmdline), density 200. Panel node `lcd5300204@5300204` in `stock.dts`.
- The DT carries 23 panel entries (several CO5300 sizes, other controllers). U-Boot selects one by `lcd_id`.

## Peripherals (bound on the running unit)

| Bus | Address | DT node / compatible | Driver | In this source? |
|---|---|---|---|---|
| i2c-0 | 0x20 | `sensor_main` | camera sensor | yes (UNISOC camera) |
| i2c-1 | 0x12 | `qmax981_acc` / `QST,qmax981` | unbound | no (not needed) |
| i2c-1 | 0x18 | `sc7a22_acc` / `sprd,sc7a22` | `sc7a22` (accelerometer, step counter, `sc7a22_input_key`, `sc7a22_input_step`) | yes (`SENSORS_SC7A22`) |
| i2c-2 | 0x0d | `och1970` / `magnetic,och1970` | unbound | no (not needed) |
| i2c-2 | 0x44 | `hrs3xxx` / `tyhx,hrs3xxx` | `hrs3xxx` (input device `heartrate`) | **no: port needed** |
| i2c-3 | 0x2d | `bl_ts` / `btl,btl_ts` | unbound | no (not needed) |
| i2c-3 | 0x38 | `cst0xx_ts` / `cst0xx,cst0xx_ts` | unbound | not needed on this unit |
| i2c-3 | 0x5a | `hynitron` / `hynitron,hyn_ts` | **`hyn_ts` (touch)** | yes (`drivers/input/touchscreen/hynitron_ts`, check the Makefile hookup) |
| spi | – | `goodix_fp_ree` / `goodix,fingerprintree` | Goodix fingerprint (`gf-keys`) | no |

Input devices: `heartrate`, `accelerometer`, `sc7a22_input_key`, `sc7a22_input_step`, `gf-keys`, `sprd-gpio-keys`, `hyn_ts`, headset jack.

Sensors HAL list: `hrs3918 HeartrateSensor` (TYHX), plus placeholder light/proximity.

## Keys

`gpio-keys`: **Volume Down Key** (`KEY_F9`, code 0x43, GPIO 0x2e), **Power Key**, **Function Key** (code 0x57, GPIO 0x2c). No rotary crown.

## Battery and charger (from `stock.dts`)

| | |
|---|---|
| Charge end voltage | 4395 mV (`chg-end-vol` 0x112b) |
| Recharge voltage | 4320 mV |
| Nominal capacity (`cnom`) | 940 mAh |
| Internal resistance (`rint`) | 590 mOhm |
| Charge current DCP/SDP/CDP | 650 mA (unknown adapter 630 mA) |
| Battery NTC (`temp-support`) | 0: no battery temperature input |

**Do not use the V13M device tree on this unit.** It charges to 4430 mV.

## Modules

- Ramdisk `/lib/modules`: `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`, `sprdwl_ng.ko`.
- Magisk modules on `/data` (not kernel modules): `supl_any_network` (GNSS HAL patch), `dirty_expire` (writeback sysctls), `wearit_aod`.

## Kernel config notes

Enabled in `stock.config` but missing from this source: `GOODIX_TEE_FINGERPRINT`, `HEARTRATE_BD1820`, `HEARTRATE_HRS3918`, `INPUT_FINGERPRINT`, `INPUT_QMAX981`, `INPUT_SENSORS_BD1XXX`, `INPUT_SENSORS_HRS3XXX`, `SENSORS_COMPASS_AF6837`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `TOUCHSCREEN_BETTERLIFE_COB_JR`, `TOUCHSCREEN_CTS0XX_E10_GS17`, `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`.

Required for a working watch: Hynitron touch (present) and HRS3xxx heart rate (port).

## Bring-up order

This unit goes first: its touch driver is already in the tree.
