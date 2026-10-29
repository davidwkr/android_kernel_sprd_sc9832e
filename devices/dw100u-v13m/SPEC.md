# DW100Ultra, other unit (firmware V13M)

Second DW100Ultra hardware variant. Data from a full firmware dump only: the unit itself was not connected, so nothing here was read from the running watch. Common platform: [`../README.md`](../README.md).

## Identity

| | |
|---|---|
| Firmware | V13M (`XV1.3_C4.4VNTC`: 4.4 V battery with NTC) |
| Stock kernel | 4.4.83, `#1 SMP PREEMPT Wed Jun 17 20:22:03 CST 2026`, built by `powersys@X99-7` |
| RAM, eMMC | not read (unit not connected) |

## Difference from the silver unit

**Kernel config:** identical to [`../dw100u-silver/stock.config`](../dw100u-silver/stock.config) except `CONFIG_RTC_START_YEAR` (2026 here, 2025 there). One kernel binary serves both DW100U units.

**Device tree:** only these differ from the silver unit:

| | Silver (V11H) | This unit (V13M) |
|---|---|---|
| Charge end voltage | 4395 mV | **4430 mV** (`chg-end-vol` 0x114e) |
| Recharge voltage | 4320 mV | 4370 mV |
| Nominal capacity (`cnom`) | 940 mAh | **1079 mAh** |
| Internal resistance (`rint`) | 590 mOhm | 359 mOhm |
| Charge current DCP/SDP/CDP | 650 mA | **800 mA** (unknown adapter 780 mA) |
| Battery NTC (`temp-support`) | off | **on** (own `temp-tab-val` table) |
| JEITA current / CC-CV tables | silver battery | own tables (CV 4450 mV) |
| OCV table (`ocv-tab-vol`) | silver battery | own table |
| Keys | Volume Down, Power, Function | **Power, Function** (Function on another GPIO controller) |

Panel list, touch, sensor, WCN, memory and pinmux nodes are the same as on the silver unit.

**Never use the silver device tree on this unit, or this one on the silver unit.** The battery profiles, charge voltages and fuel-gauge tables are different.

## Display

Same panel list as the silver unit (CO5300 AMOLED entries). The `lcd_id` actually in use was not read (unit not connected); expected `ID5300204` (2.04", 368x448).

## Peripherals

Same DT nodes as the silver unit. Which touch controller is fitted (Hynitron `hyn_ts` @0x5a vs CST0xx @0x38) was **not confirmed**. The silver unit uses Hynitron, and the DT and config are identical, so assume Hynitron until checked.

To confirm when the unit is connected:

```sh
for d in /sys/bus/i2c/devices/*-00*; do echo "${d##*/} $(cat $d/name) -> $(basename $(readlink $d/driver) 2>/dev/null)"; done
cat /proc/cmdline | tr ' ' '\n' | grep lcd_
```

## Modules

Ramdisk `/lib/modules`: `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`, `sprdwl_ng.ko` (contents differ from the silver unit's, same sizes; rebuild for the new kernel anyway).

## Bring-up order

After the silver unit: same kernel, this unit's own `stock.dts`.
