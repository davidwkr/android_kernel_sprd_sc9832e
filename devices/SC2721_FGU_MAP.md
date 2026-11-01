# SC2721 fuel gauge: `sc27xx_fuel_gauge.c` (sc2720 variant) vs the 4.4 `sprd_27xx_fgu.c`

Scope: KERNEL_4.14_PLAN.md section 7.2, step 4 (check the sc2720 gauge data against the SC2721 values). Compile-only,
nothing here has been run on hardware.

Sources of truth (4.4 tree, `android_kernel_sprd_sc9832e`), abbreviated below:

- `F`  = `drivers/power/sprd_27xx_fgu.c`
- `FH` = `drivers/power/sprd_27xx_fgu.h`
- `G`  = `include/linux/mfd/sprd/sc2721_glb.h`
- `B`  = board DT of the 4.4 watch products (`arch/arm64/boot/dts/sprd/sp9832e-1h10-*.dts`, battery node)

4.14 driver: `drivers/power/supply/sc27xx_fuel_gauge.c` (abbreviated `D`, line numbers before this change).
In 4.4 the SC2721 is `FGU_2721` and shares every branch with `FGU_2731` and `FGU_2720` (F:140-149 for the
calibration, no other type specific branch except the power-on one, F:1285-1290). `sc2720_info` carries only two
values (the PMIC global module/clock enable registers), everything else in D is common code.

Result column: OK = same value proven, DIFF = mismatch, BOARD = board data in 4.4 (DT `sprd,battery` node), not a
chip property.

## 1. Register offsets, bits (FGU block base 0xa00)

| Item | sc2720 path (D) | SC2721 (source) | Result |
|---|---|---|---|
| FGU base | DT `reg` | `reg = <0xa00>` (4.4 sc2721.dtsi fgu node) | OK, DTS item |
| MODULE_EN0 (FGU_EN BIT(7)) | 0xc08, BIT(7) (D:23, D:25) | ANA_REG_GLB_MODULE_EN0 = 0x0C00 + 0x8 (G:20), BIT_FGU_EN BIT(7) (G:265) | OK |
| RTC clock enable (BIT(6)) | 0xc10, BIT(6) (D:24, D:26) | ANA_REG_GLB_RTC_CLK_EN0 = 0x0C00 + 0x10 (G:22), BIT_RTC_FGU_EN BIT(6) (G:313) | OK |
| VOLTAGE / OCV / POCV / CURRENT | 0x20 / 0x24 / 0x28 / 0x2c (D:36-39) | FH:19-22 | OK |
| LOW_OVERLOAD (voltage low threshold) | 0x34 (D:40) | REG_FGU_LOW_OVER FH:24, 13 bit value | OK |
| HIGH_OVERLOAD | not programmed | 0x30, set to the board `chg_bat_safety_vol` (FH:23, F:1230) | DIFF, BOARD value, not set (see section 4) |
| CLBCNT SETH / SETL | 0x50 / 0x54 (D:41-42) | FH:31-32 | OK |
| CLBCNT DELTH / DELTL | 0x58 / 0x5c (D:43-44) | FH:33-34 | OK |
| CLBCNT VALH / VALL | 0x68 / 0x6c (D:45-46) | FH:37-38 | OK |
| QMAXL (first sampled open circuit current) | 0x74 (D:47), `<< 1` (D:392) | REG_FGU_CLBCNT_QMAXL 0x74, `<< 1` (F:1324) | OK |
| USER_AREA SET / CLEAR / STATUS | 0xa0 / 0xa4 / 0xa8 (D:48-50) | FH:48-50 | OK |
| VOLTAGE_BUF / CURRENT_BUF | 0xd0 / 0xf0 (D:51-52) | FH:54-55 | OK |
| Power-on type in USER_AREA | bits 15:12, first = 0xf, normal = 0x5 (D:63-69) | `BITS_POWERON_TYPE` 0xF000 (FH:56), FIRST 0xF, NORMAL 0x5 (FH:156-157) | OK |
| Saved capacity | USER_AREA bits 11:0, percent | `BITS_RTC_AREA` 0xFFF (FH:57), the UI percent (F:1378) | OK for the percent; 4.4 also keeps the raw 0.1 % SOC in USER_AREA1 (0xc0, F:347-360), which 4.14 does not use |
| Interrupt bits | LOW_OVERLOAD BIT(0), CLBCNT_DELTA BIT(2) (D:60-61) | BIT_VOLT_LOW_INT BIT(0), BIT_CLBCNT_DELTA_INT BIT(2) (FH:95, FH:97) | OK |
| Interrupt clear | writes GENMASK(9, 0) | writes 0xFFFF (F:1227) | OK (no bit above 7 is defined, FH:89-97) |
| WRITE_SELCLB_EN | START BIT(0) (D:463) | BIT_WRITE_SELCLB_EN BIT(0) (FH:61) | OK |
| RELAX_CURT_THRE (0x80) | not written | `cur2adc(relax_current)` written at init (F:1354) | DIFF, BOARD value, not written (section 4) |
| CURT_OFFSET (0x90) | not written | `fgu_cal.cur_offset` written at init (F:1404) | DIFF in form only: the value is 0 on the chip-calibration path (F:233), see section 2 |

## 2. ADC scaling and calibration

| Item | sc2720 path (D) | SC2721 (source) | Result |
|---|---|---|---|
| Calibration cell | nvmem `fgu_calib`, `& 0x1ff` (D:1193, D:1210) | efuse `BITSINDEX(3, 0, 16)`, 9 bits = block 3 bits 8:0 (F:143); 16 bit blocks (4.4 sc2721.dtsi efuse node) | OK; the 4.14 cell `calib@6` `reg = <0x6 0x2>` is block 3 (bytes 6-7), same location |
| 4.2 V ADC value | `(data & 0x1ff) + 6963 - 4096 - 256` (D:1210) | `(data + 6963) - 4096 - 256` (F:145) | OK |
| 3.6 V point and 0 mA point | not used | read at F:146-151 but dropped by `sprdfgu_cal_from_chip()` (`fgu_nv_3600mv = 0`, `fgu_0_cur_adc = 0`, F:286-290), so one point calibration | OK |
| 1000 mV ADC | `DIV_ROUND_CLOSEST(cal * 10, 42)` (D:1211) | `DIV_ROUND_CLOSEST(fgu_nv_4200mv * 10, 42)` (F:230-231) | OK |
| 1000 mA ADC | `vol_1000mv_adc * 4 * real / spec` (D:1212) | `vol_1000mv_adc * 4 * rsense_real / rsense_spec` (F:234-237) | OK; B has real = spec = 200 on all 1h10 boards, 4.14 `sl8541e-1h10.dts` has 20000 / 20000, ratio 1 in both |
| Voltage offset | none | `vol_offset = 0 + fgu_cal_ajust` (F:232, F:250) | OK in value: `fgu-cal-ajust = <0>` in B on all boards; BOARD item, no 4.14 property |
| Current zero | 8192 (D:71) | `CUR_0ma_IDEA_ADC` 8192 (FH:154), `cur_offset = 0` on this path | OK |
| Voltage ADC to mV | rounded, `adc * 1000 / vol_1000mv_adc` (D:187) | truncated `(adc + vol_offset) * 1000 / vol_1000mv_adc` (F:295-298) | DIFF, rounding only (< 1 mV), not changed |
| Current ADC to mA | rounded (D:175) | truncated `adc * 1000 / cur_1000ma_adc` (F:305-308) | DIFF, rounding only, not changed |
| Coulomb counter scale | 1 mAh = 36 * cur_1000ma_adc * 2 / 10 counts (`SAMPLE_HZ` 2, D:72, D:1183) | `cur * cur_1000ma_adc * 36 * FGU_CUR_SAMPLE_HZ / 10` (F:403-412), `FGU_CUR_SAMPLE_HZ` 2 (FH:155) | OK |
| Counter to capacity | `delta * 10 / (36 * 2)` then ADC to mA, mAh (D:543-545) | `delta / (360 * 2)` then ADC to mA, 0.1 mAh (F:904-906) | OK (same factor 10 apart, the 4.4 unit is 0.1 %) |
| OCV | `vol_mV * 1000 - cur_mA * R_mOhm`, uV (D:622-646) | `vbat_mV - cur_mA * rint / 1000`, mV (F:470-500, rint in mOhm, 0x168 = 360 in dm63 B) | OK, units consistent (mA * mOhm = uV); R is the DT `factory-internal-resistance` in 4.14 |

## 3. Behaviour that differs (not a register value)

| Item | sc2720 path (D) | SC2721 4.4 | Result |
|---|---|---|---|
| Counter read | VALL then VALH once (D:468-488) | read both halves twice and repeat (up to 100 times, 10 us apart) until equal (F:363-376) | DIFF: a carry between the two reads gives a torn value. Implemented for SC2721 |
| Counter write | SETL, SETH, START, no wait (D:446-466) | same, then `udelay(130)` (F:379-387) | DIFF. Implemented for SC2721 |
| Power-on capacity with the AA chip | always POCV | `ANA_REG_GLB_CHIP_ID_LOW` (0x0C00, G:18) == 0xA000 (SC2721 AA): use the instantaneous voltage and current instead of POCV (F:1285-1290, F:1298-1313) | DIFF. Implemented for SC2721 |
| Power-on capacity with a charger present | always POCV | `gpio_vchg_detect` set and `ocv_type == 0` (sc2721.dtsi `ocv-type = <0>`): same instantaneous OCV instead of POCV (F:1298-1313) | DIFF, needs the board charger detect GPIO (B `charger-det-gpios`), not available to the gauge node: left disabled, TODO |
| Disable path | clears bit 6 of 0xc18 and bit 7 of 0xc08 (`SC27XX_CLK_EN0` hard coded, D:1163-1168, D:19-20) | RTC clock enable is 0xc10 (G:22). 0xc18 is `ANA_REG_GLB_SOFT_RST1` (G:24) | DIFF (a bug in the common code, also for sc2720 and sc2730). Fixed for SC2721 only, sc2720 unchanged |
| Low voltage threshold and alarm | DT `voltage-min-design` | board `alm_vol` (0xd7a = 3450 mV in the dm63 B) | BOARD, same role |
| Capacity floors / fixups when not charging | `sc27xx_fgu_low_capacity_calibration()` | `sprdfgu_read_soc()` (F:888-988), different rules | design difference, not touched |
| Gauge resync while the CV override holds | none | in the charger, `sprd_2721_charge.c:505-735` | step 3, see the resync section of this file |

## 4. Result and what was added

Chip level mismatches (so a separate `sprd,sc2721-fgu` data set is justified): counter read/write timing, the AA chip power-on
OCV and the disable path registers. All register offsets, the efuse calibration formula, the ADC scaling, the counter scaling
and the interrupt bits are equal between the sc2720 data and the SC2721 values in 4.4.

`sc2721_info` therefore has the same two enable registers as `sc2720_info` (0xc08 / 0xc10, both proven above) plus flags for the three
behaviours above. `sc2720_info` is unchanged.

Left disabled (cannot be proven from the 4.4 chip level sources, all BOARD data in 4.4):

1. `RELAX_CURT_THRE` write. 4.4 value is the per board `relax-current` (50 mA on all 1h10 boards). It sets the hardware relaxation
   detector that only feeds the OCV register, which 4.14 does not read. TODO if the OCV register is ever used.
2. `HIGH_OVERLOAD` (`chg_bat_safety_vol`) and `fgu-cal-ajust` (0 on all boards). No 4.14 DT property. TODO.
3. `CURT_OFFSET` register: not written, the 4.4 value is 0 on the efuse calibration path. If a product ever uses the 4.4 `fgu_cal`
   command line (NV calibration), the offset is non zero and this is wrong. TODO.
4. Charger-present power-on OCV (`gpio_vchg_detect`): booting with a charger connected still uses POCV, which can give a wrong
   initial capacity (non-AA chips). TODO, needs a DT GPIO.
5. Truncation vs rounding of the ADC conversions: sub-millivolt, not changed.

DTS note (out of scope, not changed): `sc2721.dtsi` lists `"sprd,sc27xx-fgu", "sprd,sc2720-fgu", "sprd,sc2721-fgu"`. The OF
core takes the first listed compatible that matches, which is `sprd,sc2720-fgu`, so `sc2721_info` is not selected until the
`sprd,sc2721-fgu` string is listed before it.

## 5. Gauge resync (plan 7.2 step 3)

Implemented in `sc27xx_fuel_gauge.c` for `sprd,sc2721-fgu` only, off by default (`gauge_resync=0`). It is a port of the
4.4 `gauge_resync` in `sprd_2721_charge.c:505-735`; the 4.4 thresholds are kept: poll 60 s (C:536), rest 600 s (range
600..86400), relaxed current 30 mA (1..100), OCV within 60 mV of the hold (10..200), difference 5 points (1..50), gap 1800 s
(floor 1800), OCV capacity not 0 %, not 100 % unless the OCV is at or above the first table entry, CHARGING with the charger
online, rest continuous (a gap above 2 polls restarts it). The thresholds are read-only module parameters
(`sc27xx_fuel_gauge.gauge_resync_*`), range checked.

Mechanism: the charger (`sc2720_charger.c`, SC2721 only) answers `POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE` with the CV in
uV the override holds the battery at, 0 when not holding (the 4.4 `gr_hold_target_mv()` conditions: override on, a CV
requested, effective CV below the request, effective CV representable). The gauge reads it, with STATUS and ONLINE, through
`power_supply_get_by_name("sc2721_charger")`. Chosen over an exported function because the gauge (`=y` or `=m`) then has no
link dependency on the charger module, and the existing gauge already reads the charger this way (`sc27xx_fgu_get_status()`).
"CV reached" is not read from CHGR_STATUS (not implemented in the charger, see SC2721_CHARGER_MAP.md): as in 4.4 it is
implied by the relaxed current and the OCV being at the hold voltage.

Differences to 4.4: the capacity is in percent (4.4 uses 0.1 %), so the 5 point threshold is 5 %. The "raw" capacity is the
pure coulomb counter value (`sc27xx_fgu_get_raw_capacity()`, split out of `sc27xx_fgu_get_capacity()`, which is unchanged),
not `sprdfgu_read_soc()` with its side effects. There is no generation counter for A->B->A changes of the override within
one poll: the hold voltage and charging state are re-read right before the write and must be unchanged. The write is the
existing `sc27xx_fgu_adjust_cap()`, which re-bases `init_cap` / `init_clbcnt` (it does not rewrite the counter registers).
Units: OCV and hold in uV, current in mA (`mA * mOhm = uV`), thresholds converted once (`mV * 1000`). Untested on hardware.
