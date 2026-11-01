# SC2721 charger register map vs `sc2720_charger.c`

Scope: KERNEL_4.14_PLAN.md section 7.2, steps 1 and 2 (charger driver and CV override). Compile-only,
nothing here has been run on hardware.

Sources of truth (4.4 tree, `android_kernel_sprd_sc9832e`), abbreviated below:

- `C`  = `drivers/power/sprd_2721_charge.c`
- `H`  = `drivers/power/sprd_2721_charge.h`
- `G`  = `include/linux/mfd/sprd/sc2721_glb.h`
- `G0` = `include/linux/mfd/sprd/sc2720_glb.h` (only to confirm what the sc2720 driver matches)
- `E`  = `drivers/misc/sprd_otp/pmic_efuse.c`

4.14 driver: `drivers/power/supply/sc2720_charger.c` (abbreviated `D`, line numbers before this change).
The charger registers are two consecutive words, `CFG0 = base + 0x0` and `CFG1 = base + 0x4`. In the 4.4 SC2721
headers they are `CHGR_CTRL0` / `CHGR_CTRL1` at `CTL_BASE_ANA_GLB (0x0C00, G:16) + 0x02C0 / 0x02C4`
(G:194-195), i.e. PMIC address `0x0EC0`. The DT `reg` of the SC2721 charger node must therefore be
`0xec0` (not changed here, DTS is out of scope).

## 1. Constants used by `sc2720_charger.c`

Result column: OK = same value proven for SC2721, DIFF = mismatch, NP = not proven (not used for SC2721).

| Item | sc2720 (D) | SC2721 (source) | Result |
|---|---|---|---|
| CFG0 offset | 0x0 (D:17) | CTRL0 = 0x02C0 from block base 0x0C00 (G:194) | OK (relative) |
| CFG1 offset | 0x4 (D:18) | CTRL1 = 0x02C4 (G:195) | OK |
| CHG_PD | BIT(0) (D:32) | BIT_CHGR_PD BIT(0) (G:2315) | OK |
| CC_MODE (CC_EN) | BIT(12) (D:33) | BIT_CHGR_CC_EN BIT(12) (G:2310) | OK |
| DPM field | GENMASK(14,13) (D:35) | BITS_CHGR_DPM 2 bits at 13 (G:2309) | OK |
| DPM value programmed | 2 if vmax < 4300 mV else 3 (D:203-221) | always 3 (C:953) | DIFF: value 2 never used on SC2721, always 3 |
| ITERM field | GENMASK(1,0)<<1 defined, never written (D:40-41) | 2 bits at 1 (G:2314); written 0 at init (C:957 -> C:919-923) | OK, sc2721 not written by us (see section 2) |
| END_V field | GENMASK(4,3) (D:43) | BITS_CHGR_END_V 2 bits at 3 (G:2313) | OK |
| CV_V field | GENMASK(10,5) (D:46) | BITS_CHGR_CV_V 6 bits at 5 (G:2312) | OK |
| CC_I field | GENMASK(13,10), 4 bits (D:49) | BITS_CHGR_CC_I 0x1F<<10, 5 bits (G:2325); sc2720 header has 4 bits (G0:1601) | DIFF: 5 bits on SC2721 |
| OVP field | GENMASK(1,0), 2 bits (D:38) | BITS_VCHG_OVP_V 0x3F<<0, 6 bits (G:2327); sc2720 header has 2 bits (G0:1602) | DIFF: 6 bits, 100 mV steps from 5600 (C:90, H:21-22) |
| OVP value written | 0x01 ("6500 mV") (D:229-236) | sc2721 level comes from board `ovp_stop` (C:956) | NP: value is board policy, not in the 4.4 sources reviewed; not written for SC2721 |
| CC current min | 300 mA (D:26) | SPRDBAT_CHG_CUR_LEVEL_MIN 300 (H:24, C:123) | OK |
| CC current max | none (clamp absent) | 1300 mA (H:25, C:120-121) | DIFF: SC2721 clamps at 1300 mA |
| CC current step | 50 mA below 800, 100 mA from 800, reg 0x0a at 800 (D:27-30, D:279-287) | 50 mA per code from 300: `(cur-300)/50` (C:127); the 1400+ branch (C:129-130, +0x16) is unreachable after the 1300 clamp | DIFF: linear 50 mA, code = (mA-300)/50 |
| CC current readback | 300 + 50*code below 0x0a, 800 + 100*(code-0x0a) above (D:339-345) | `(CTRL1 >> 10 & 0x1f) * 50 + 300` (C:151-154) | DIFF |
| CCCV step | 75 (x0.1 mV = 7.5 mV) (D:23) | ONE_CCCV_STEP_VOL 75 (H:19) | OK |
| CCCV code max | 0x3f (D:25) | SPRDBAT_CCCV_MAX 0x3F (H:18) | OK |
| CCCV code min | 0 | SPRDBAT_CCCV_MIN 0x00 (H:17) | OK |
| END_V base | 4200 mV (D:20) | `chg_end_vol >= 4200` (C:167) | OK |
| END_V step | 100 mV (D:22) | 4200/4300/4400 for END_V 0/1/2 (C:167-205, model comment C:275-278) | OK |
| END_V max code / ceiling | clamp 4500 mV, code up to 3 (D:21) | codes 0..2 only, 4400 mV ceiling (C:190-196, C:284) | DIFF: 4400 mV ceiling, END_V 3 never used |
| Below-4200 mV handling | code = cal - round((4200-v)/7.5), floor 0, END_V 0 (D:142-151) | same (C:204-210) | OK |
| CV code LSB | written as computed (D:164) | `& ~1` before the write (C:261-262, model C:311) | DIFF: bit 0 of the CV code is cleared on SC2721 |
| Write order | END_V then CV (D:153-170) | stock: END_V(0) first inside `sprdchg_tune_endvol_cccv` (C:165-166), then CV (C:265); override: see section 3 | DIFF (see section 3) |
| Calibration source | nvmem cell `cccv_calib`, mask 0x3f (D:102-112) | efuse bits `BITSINDEX(13,0)`, 6 bits = block 13 bits 5..0 with 16-bit blocks (C:69, C:13; E width from `sprd,block-width = <16>`, sc2721.dtsi:84) , mask 0x003F (C:71) | OK for the 6-bit field; location is a DTS item |
| Calibration validity | none | efuse block 0 bit 0 must be set, else cal = 0x20 (C:62-66) | DIFF: needs a second cell (validity flag) |
| Calibration default | none | 0x20 when invalid (C:65) | DIFF |
| Battery psy name | `sc27xx-fgu` (D:15) | not part of the 4.4 charger | n/a (gauge, step 3) |

SC2721 ones used for the new match data: CFG0/CFG1 layout, PD, CC_EN, DPM field and value 3, END_V/CV_V fields,
CC_I 5-bit field with linear 50 mA code and 300..1300 mA range, CCCV step 75, code max 0x3f, END_V 4200 + 100*n with
n <= 2, `& ~1` on the CV code, calibration 6 bits + validity flag + default 0x20.

## 2. SC2721 functions in the 4.4 driver that `sc2720_charger.c` lacks (not implemented, steps 1-2 only)

| Function (4.4) | Where | Status |
|---|---|---|
| Termination current (ITERM) level, written 0 at init | C:919-923, C:957 | not implemented, field is proven (G:2314) but the level policy is board data |
| Recharge enable (BIT_RECHG BIT(11)) set on start, cleared on stop | C:895-917, G:2311 | not implemented, start/stop only touch PD |
| Charge-power-down external (BIT_CHGR_PD_EXT BIT(15)) | G:2306 | not used by 4.4 driver either, ignored |
| CV status read (BIT_CHGR_CV_STATUS BIT(4) of CHGR_STATUS 0x02C8) | C:926-933, G:196, G:2345 | not implemented; needed by step 3 ("CV reached") |
| Input over-voltage (OVP) level, 100 mV steps from 5600 up to 9000 | C:80-101, H:21-22 | not implemented for SC2721 (value is board `ovp_stop`) |
| VCHG over-voltage and CV GPIO interrupts (`chg-ovi-gpios`, `chg-cv-gpios`) | C:960-995, C:1030-1050 | not implemented |
| BC1.2 detection disable (`DP_DM_BC_ENB` in `CHGR_DET_FGU_CTRL`) | C:884-893 | not implemented, USB charger type comes from the usb_phy in 4.14 |
| Charge timer (separate block, 32768 Hz load value, enable BIT(1), int enable BIT(0)) | `sprd2721_chg_timer.c`:34-82 | not implemented; separate DT node with its own `reg` and irq |
| Fast charge (FCHG1/FCHG2 block, 9/12/20 V) | `sprd_2721_fchg.c`:25-67 | not implemented; not relevant for 5 V watch input |
| Watchdog, input current limit register | no register in the sources reviewed | does not exist in the 4.4 SC2721 driver, nothing to port |
| Gauge resync / "battery hold" polling (`gauge_resync`) | C:505-880 | step 3, deliberately not done here |
| Charge fault read | C:935-938 | returns 0 in 4.4, nothing to port |

## 3. CV override semantics carried over (from C:226-470)

- Module parameter `cv_override_mv`, range 4000..4400 mV, 0 = off (C:226-240, C:420-423). Out-of-range writes
  return -EINVAL. In 4.14 it is a parameter of the `sc2720_charger` module and only affects SC2721 devices.
- Target = `min(request, override)` where the override is first capped to the battery profile maximum
  (`constant-charge-voltage-max-microvolt`, the 4.4 `chg_end_vol_pure`) (C:246-257).
- Representable CV model: `END_V` base 4200/4300/4400 mV, voltage = `base + (code - cal) * 7.5 mV`, code has bit 0
  cleared (C:283-322). Highest representable CV not above the target is searched downward in 1 mV steps
  to 3700 mV (C:329-346).
- If no representable CV is at or below the target: the lowest CV (`END_V` 0, code 0) is programmed, unless the
  stock request itself is lower still, in which case the stock request is programmed (C:389-405).
- Writes: CV code to 0 first, then `END_V`, then the CV code (C:361-371 and C:442-444), all under one mutex that
  also serialises the parameter write (C:104, C:410-418).
- Override off: stock programming of the request, with the 4.4 arithmetic (C:160-212). The 4.14 port refuses
  (`-EINVAL`, nothing written) the requests where that arithmetic would wrap (above 4400 mV or code out of range)
  instead of writing the wrapped value.

Difference to 4.4 that is intentional: the 4.4 stock path briefly writes END_V 0 with the old code before the new
code; the 4.14 port always follows the monotone order above, including with the override off.

## 4. Left disabled for SC2721 (cannot be proven from the 4.4 sources)

1. Input over-voltage protection: field width (6 bits) and step (100 mV from 5600) are proven, the programmed value
   is board data (`ovp_stop`) not found in the SC2721 sources. Not written. TODO: add a DT property once the
   board value is known.
2. DPM value 2 (sc2720 path for vmax < 4300 mV): SC2721 always programs 3 (C:953).
3. Termination current (ITERM) and recharge bit: not written. TODO (hardware validation first).
4. END_V code 3 / targets above 4400 mV: not representable per 4.4, rejected.
5. Efuse layout in the 4.14 efuse driver: the calibration field is block 13 bits 5..0 and the validity flag is block 0
   bit 0, with 16-bit blocks (E, sc2721.dtsi:83-84). The nvmem cell offsets for the 4.14 efuse driver must be
   checked when the DTS is written; the charger requires nvmem cells `cccv_calib` (6 bits) and `cccv_calib_valid`
   (bit 0) and fails probe without them (no guessed default).
6. Timers, fast charge, CV status, interrupts: see section 2.
7. Everything is untested on hardware.

## Init unit fix

`0dee9e011d3a`: the vendor `hw_init()` passed the battery CV in mV to `sc2720_set_termination_voltage()`, which expects uV, so the first CV programmed was the lowest one (about 3.96 V). Fixed for both SC2720 and SC2721.
