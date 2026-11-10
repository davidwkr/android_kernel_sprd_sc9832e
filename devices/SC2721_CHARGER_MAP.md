# SC2721 charger register map vs `sc2720_charger.c`

Scope: KERNEL_4.14_PLAN.md section 7.2, steps 1 and 2 (charger driver and CV override), plus the charge-safety
items (timer, watchdog, ITERM/recharge, OVP/CV interrupts, fast charge) added on `kernel-4.14-charge-safety`,
section 5. Compile-only, nothing here has been run on hardware.

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
| OVP value written | 0x01 ("6500 mV") (D:229-236) | `sprdchg_set_chg_ovp(ovp_stop)` (C:80-101, called from `sprdchg_chip_init()` C:956): clamp 5600..9000 mV (H:21-22), code `(mV - 5600) / 100` into `BITS_VCHG_OVP_V` (CTRL1 bits 5..0, G:2327, CTRL1 = G:195), CC_EN (G:2310) cleared before and set after the write (C:92-100) | OK, ported (`sc2721_set_ovp()`), level from DT `sprd,ovp-microvolt`; DM63 stock `ovp-stop` 6500 mV = code 9 |
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
| Termination current (ITERM) level, written 0 at init | C:919-923, C:957 | not implemented, field is proven (G:2314) but the level policy is board data; the 4.4 driver itself only ever writes the fixed level 0, so there is no mA policy to port. Software equivalent: `cm-fullbatt-current` (charger-manager), see section 5 |
| Recharge enable (BIT_RECHG BIT(11)) set on start, cleared on stop | C:895-917, G:2311 | **implemented**: `sc2720_charger_start_charge()`/`stop_charge()` set/clear `SC2721_CHG_RECHG_EN` for SC2721 (section 5). The recharge level itself is a fixed hardware hysteresis in both 4.4 and 4.14, nothing to program; the software recharge decision is `cm-fullbatt-vchkdrop-volt`, now derived from the stock `rechg-vol` (section 5) |
| Charge-power-down external (BIT_CHGR_PD_EXT BIT(15)) | G:2306 | not used by 4.4 driver either, ignored |
| CV status read (BIT_CHGR_CV_STATUS BIT(4) of CHGR_STATUS 0x02C8) | C:926-933, G:196, G:2345 | not read directly; superseded by the `chg-cv-gpios` interrupt below, which reports the same condition without a register read. "CV reached" for recharge/full-detect purposes is still derived in software (gauge resync), as in 4.4 |
| Input over-voltage (OVP) level, 100 mV steps from 5600 up to 9000 | C:80-101, H:21-22 | implemented, `sc2721_set_ovp()` (section 4 item 1) |
| VCHG over-voltage and CV GPIO interrupts (`chg-ovi-gpios`, `chg-cv-gpios`) | C:960-995, C:1030-1050 | **implemented** (section 5): both are optional PMIC EIC interrupts (stock EIC 6 / 4). OVP latches `POWER_SUPPLY_HEALTH_OVERVOLTAGE`, stops charging immediately from the interrupt (defence in depth; the analog comparator already gates the charge path in hardware) and refuses to resume until the comparator clears. CV is diagnostic only (`info->cv_reached`), nothing is keyed off it |
| BC1.2 detection disable (`DP_DM_BC_ENB` in `CHGR_DET_FGU_CTRL`) | C:884-893 | not implemented, USB charger type comes from the usb_phy in 4.14 |
| Charge timer (separate block, 32768 Hz load value, enable BIT(1), int enable BIT(0)) | `sprd2721_chg_timer.c`:34-82 | not implemented as a PMIC register; superseded by the charger-manager software safety timeout `cm-charging-max` (stock `chg-timeout`, section 5), which stops charging from `check_charging_duration()` independently of the charger driver |
| Fast charge (FCHG1/FCHG2 block, 9/12/20 V) | `sprd_2721_fchg.c`:25-67 | not implemented; `fast-charger@900` is `status = "disabled"` in the stock DT on every board this tree supports (K18, DM63 confirmed), so stock never raises VCHG above 5 V either. Left disabled to match |
| Watchdog, input current limit register | no register in the sources reviewed | does not exist in the 4.4 SC2721 driver, nothing to port. The stock `chg-wdt@80` node is itself `status = "disabled"` (K18, DM63), consistent |
| Gauge resync / "battery hold" polling (`gauge_resync`) | C:505-880 | step 3, deliberately not done here |

## 5. Charge safety (`kernel-4.14-charge-safety` branch)

Scope: KERNEL_4.14_PLAN.md section 7.2 risk item 3 ("no protection for an unattended charge"). Everything in this
section is compile-only, nothing run on hardware; the `cm-*` values are the stock `battery` node converted by the
same method as `devices/k18/DTS_414.md` / `devices/dm63/DTS_414.md` ("Charging").

1. **Charge timer / overall safety timeout**: already wired before this branch via `cm-charging-max` in both
   `sp9832e-1h10-k18.dts` and `sp9832e-1h10-dm63.dts` (21600000 ms = stock `chg-timeout` 21600 s). Verified against
   `charger-manager.c:check_charging_duration()`, which calls `try_charger_enable(cm, false)` once the duration is
   exceeded and the battery has not reached the full-voltage window. No change needed; documented here because it is
   the item the task description calls "the charge timer".
2. **Recharge**: two parts.
   - Hardware: `SC2721_CHG_RECHG_EN` (`CHGR_CTRL0` bit 11, `sc2720_charger.c`) is now set whenever charging starts and
     cleared when it stops, SC2721 only, matching the 4.4 `sprdchg_start_charge()`/`sprdchg_stop_charge()`. The
     recharge trip level is a fixed hardware comparator in both trees; there is no register field for it to program.
   - Software: `cm-fullbatt-vchkdrop-volt` was a generic placeholder (60 mV) copied from the `sl8541e-1h10` reference
     board. Replaced with the stock-derived drop `chg-end-vol-check - rechg-vol`: K18 4390 - 4374 = 16 mV, DM63
     4450 - 4415 = 35 mV. This is read by `charger-manager.c:fullbatt_vchk()`/`check_charging_duration()`, which calls
     `try_charger_restart()` once the OCV has dropped this much below the full-battery voltage.
3. **ITERM / charge-done**: left as a software threshold, not a PMIC register (see section 2 row 1). `cm-fullbatt-current`
   (stock `chg-end-cur`, 60 mA on both boards) and `cm-fullbatt-voltage` (stock `chg-end-vol-check`) were already
   mapped before this branch; `is_full_charged()` in `charger-manager.c` uses both together. Not changed here, only
   verified against the stock values and `SC2721_FGU_MAP.md`'s "CV reached" design (gauge resync uses the CV hold
   voltage, not the PMIC ITERM register, which 4.4 never programs beyond level 0 either).
4. **OVP value and OVP/CV interrupts**: the OVP value (`sprd,ovp-microvolt`, ovp-stop) was already ported. This
   branch adds the two PMIC EIC interrupts, `chg-ovi-gpios` and `chg-cv-gpios` (both optional in the driver, so units
   without the property still probe):
   - `sc2721_ovp_irq_handler()`: reads the gpio under `info->lock`, latches `info->ovp_fault`. While a fault is
     latched, `POWER_SUPPLY_PROP_HEALTH` reports `POWER_SUPPLY_HEALTH_OVERVOLTAGE` (even if a failed stop left
     `info->charging` set) and `sc2720_charger_start_charge()` refuses to start (`-EBUSY`); every caller (the USB
     work and `set_status()`) holds `info->lock`, so the check cannot race the handler. The initial gpio state is
     read under the same lock after the irq is requested, and a gpio read error fails probe instead of counting as
     a fault. If the fault asserts while already charging, the handler itself calls
     `sc2720_charger_stop_charge()` immediately, not waiting for `charger-manager`'s next poll
     (`cm-poll-interval` 15 s) — the hardware OVP comparator has already disconnected the input by then, this is
     software catching up to the same state. `charger-manager`'s existing `cm_check_charge_health()` (unchanged)
     then converges `cm->charger_enabled` to match and resumes once health reports `GOOD` again (fault gpio
     deasserted), through its existing "Charging health is recover good" path.
   - `sc2721_cv_irq_handler()`: reads the gpio, stores `info->cv_reached` for diagnostics only. Not wired to any
     charging decision, because the recharge/full-detect logic already in place (item 2, item 3) is the one proven
     against the 4.4 driver's actual behaviour; the CV GPIO reports the same CHGR_STATUS bit the 4.4 driver itself
     never acted on either (`sprdchg_get_cccvstate()` is only read for a debug log, C:926-933).
   - `ovp-restart` (5800 mV, both boards): not a separate register in either tree, the OVP comparator's hysteresis
     is fixed in hardware; nothing to program.
   - Start/stop write `CHG_PD` and `RECHG_EN` in one masked `CHG_CFG0` update, and `info->charging` is cleared
     only after a stop succeeds (`set_status()` returns the error), so a failed write retries on the next
     `STATUS=0` or unplug instead of leaving charging on behind the driver's back.
   - Probe and remove: the USB notifier is registered only after `psy_usb` exists; on a later probe failure and on
     remove it is unregistered, `info->work` is drained with `cancel_work_sync()`, and any charge the work started
     is stopped, before devm frees `info`.
5. **Fast charge**: confirmed `fast-charger@900` (`sprd,sc2721-fchg`) is `status = "disabled"` in the stock DT of
   every board this tree ports (K18, DM63; also checked MT107/DM82/DW100U in passing, same). Stock never raises the
   input above 5 V on these products, so no fast-charge protection code is needed; the 4.14 port must simply not
   enable a `sc2721_fast_charger`-style driver, which it does not.
6. **Watchdog**: the stock `chg-wdt@80` (`sprd,sc2721-chg-wdt`) is `status = "disabled"` on every board checked, and
   section 2 already established the 4.4 driver has no code path that touches it. Nothing to port; `cm-wdt-interval`
   (charger-manager's own `POWER_SUPPLY_PROP_FEED_WATCHDOG` petting, a different, external-watchdog-chip mechanism)
   does not apply here either and is intentionally left unset.
7. Still open, as before this branch: representability edge cases of the CV override, hardware validation of
   everything (no watch has been charged under this kernel).
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

1. Input over-voltage protection: ported. `sc2721_set_ovp()` is the 4.4 `sprdchg_set_chg_ovp()` (C:80-101, field G:2327,
   limits H:21-22, call C:956). The level is board data, read from the DT property `sprd,ovp-microvolt` (uV). When the
   property is absent the register is left as is and a warning is printed once. Stock DM63: `ovp-stop` 0x1964 = 6500 mV.
   Not ported: the VCHG over-voltage interrupt (`chg-ovi-gpios`, C:960-995) and `ovp-restart`.
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
