# Kernel 4.14 feasibility plan (SC9832E "sharkle")

Status: feasibility only. Nothing here has run on a watch. Scope of this step: provenance, a first
build, a gap matrix against our 4.4 watch kernel, and the Android implications.

## 1. Provenance and licensing

| Item | Value |
|---|---|
| Source | `https://github.com/strongtz/linux-sprd` (remote `sprd414`) |
| Commit | `d834a127aa46356c63bd5a59bc6d86c66ea371be` ("Initial commit", `sprd414/main`) |
| Version | Linux 4.14.133 (UNISOC downstream, 2019 BSP drop) |
| Local branch | `kernel-4.14` (worktree next to the 4.4 tree) |

Licence check:

- `COPYING` is the standard GPL-2.0 text with the usual syscall note; the top-level `Makefile`
  carries `SPDX-License-Identifier: GPL-2.0`. This is GPL kernel source.
- UNISOC driver directories (`drivers/gpu/drm/sprd`, `drivers/misc/sprdwcn`, `drivers/soc/sprd`,
  `drivers/staging/sprd`, `drivers/trusty`, `sound/soc/sprd`) carry GPL-2.0 headers. A few
  internal `.h` files (for example `drivers/misc/sprdwcn/include/*.h`) have no licence header at
  all; they are part of GPL drivers, but note it if we ever redistribute them separately.
- No firmware or binary blobs: `firmware/` holds only a `Makefile`; the only `*_shipped` files are
  the stock upstream kbuild/dtc/crypto ones. No `.ko`, `.a`, `.so` or ELF files are tracked.
  Touch-controller firmware is therefore not in this tree (our 4.4 tree carries some, such as
  `gt1x_firmware.h`).
- The Mali kbase driver is not in the tree (no `drivers/gpu/arm`); it is out of tree in 4.4 too
  (GPL-2.0 kbase from the vendor).
- Caveat: the import has no history, so per-file authorship/licence provenance cannot be checked
  beyond headers. All files were committed with mode 100644 (see fix list below).
- `drivers/misc/sprdwcn` and `drivers/misc/sprdwcn_temp` are near-duplicates (214 files, 51 differ).

## 2. First build

Config: `sprd_sharkle_defconfig`, `ARCH=arm64`, `CROSS_COMPILE=aarch64-linux-gnu-` (GCC 16.2.0),
out-of-tree build under `~/.cache` (1.7 GB, deleted afterwards).

Result: **`Image` builds** (16 MB, `4.14.133+`, vmlinux text 10.8 MB). **`dtbs` build** (all `sprd/*.dtb`,
including `sl8541e-1h10.dtb`, the sharkle board). `modules` builds nothing: the defconfig has no
`=m` options, everything is built in.

Compatibility commits (5, all small, on `kernel-4.14`):

1. `scripts/dtc: remove redundant YYLOC global declaration` (host link, `-fno-common`)
2. `scripts/dtc: link libyaml via HOSTLOADLIBES_dtc` (host link; this tree does not know `HOSTLDLIBS_*`)
3. `kbuild: disable new GCC 10-16 warnings that break the -Werror build` (array-bounds,
   zero-length-bounds, stringop-*, array-compare, address, restrict, enum-int-mismatch,
   dangling-pointer, use-after-free). `enum-int-mismatch` was a hard error (dm-table prototypes).
4. `kbuild: do not promote remaining compiler warnings to errors` (`-Wno-error`; remaining ones are
   builtin-declaration-mismatch, sizeof-pointer-memaccess and similar in vendor code)
5. `Restore executable bit on scripts with a shebang` (409 files). Not a compiler issue: the import
   lost all exec bits, so `gen_vdso_offsets.sh` could not run, `vdso-offsets.h` came out empty and
   `signal.c` failed with `vdso_offset_sigtramp undeclared`. Anything running tools straight from the
   tree would hit this.

No asm-goto, `-fcommon` (beyond dtc) or implicit-declaration fixes were needed. The tree is well inside
the "about 15 fixes" bound. Not verified: link-time behaviour on hardware (stack protector, KASLR
offsets), `-Wno-error` hides real bugs, so a warning review of vendor code is still owed.

Finding on device trees: `sp9832e-1h10.dts` in this tree is stale. It is not in the DTS Makefile and
does not build on its own (it includes only `sp9832e-mach.dtsi`, so `sdio3`, `uart0`... labels are
missing). The buildable sharkle board is `sl8541e-1h10.dts` (includes `sc9832e.dtsi`,
`sp9832e-mach.dtsi`, `sc9832e-wcn.dtsi`, `sc2721.dtsi`, `sprd-sound-sharkl.dtsi`, an LCD dtsi). Use it
as the template for per-watch DTS.

## 3. Gap matrix

Legend: PRESENT = in 4.14 and probably usable, PORT = driver exists in our 4.4 tree and must be
ported/re-implemented, MISSING = in neither tree or needs new work.

| Subsystem | 4.4 (ours) | 4.14 tree | Verdict |
|---|---|---|---|
| CPU/clk/pinctrl/DMA/I2C/SPI/GPIO/timers, sharkle SoC DT | yes | `sc9832e-clk`, `pinctrl-sprd-sharkle`, `sharkle.dtsi`, etc. | PRESENT |
| Display stack | ADF (`drivers/video/adf/sprd`, `sprd-adf`, `sprd-panel-if`) | DRM/KMS (`drivers/gpu/drm/sprd`: `sprd_dpu`, `sprd_dsi`, `sprd_dphy`, `sprd_panel`, `sharkle/global_*`, `dpu_lite_r1p0/r2p0`) | PRESENT but different model (see below) |
| Panels CO5300 (3 variants), ST77916 (3), ST7801, CH13613, SH8601(2), GC9B71(2), ST7797, `lcd_id` selection | per-panel DT nodes (`init-data`, `power-on-sequence`, `oled-backlight`), selected by `lcd_id=` bootarg and `panel-drivers` list | no panel driver per IC: one `sprd,generic-mipi-panel` driver with `sprd,initial-command` byte streams in `lcd/*.dtsi`; OLED hooks exist (`sprd,reg-lock/unlock`, `default-brightness-level`, `sprd,max-level`); no `lcd_id` bootarg selection | CO5300 DONE (3 variants, `lcd/lcd_co5300_*.dtsi`, `lcd_id=` selection via `sprd,lcd-id`, `sprd,refresh-rate`, stock avdd sequence; see `dm63/PANEL_414.md`); other panels PORT |
| AOD (`sprd_aod_idle`) | custom, in 4.4 | not present | PORT (re-plumb on DRM atomic suspend and the new panel code) |
| WCN (Marlin/SC2342 integrated, BT, FM) | `drivers/misc/sprdwcn_bsp` + `sprd_bt_lpm` | `drivers/misc/sprdwcn` (new generation: SC2342_INTEG, SC2355, UMW265x; `sprdwcn_temp` duplicate) | PRESENT, different generation: compatibles `sprd,integrate_marlin`, `sprd,marlin3` exist; WCN firmware loading and our BT/GNSS wakelock tweaks need re-checking |
| GNSS (GE2, `gnss_common_ctl`, our `gnss_engine_state`, retry limits) | yes | `sprdwcn/platform/gnss` with `sprd,integrate_gnss`, `gnss_dump` | PORT of our GNSS fail-path/power tweaks onto the new driver; basic support PRESENT |
| SIPC / modem | `sprd,sipc-v1`, sblock/swcn_blk/sipx/spool + our peer checks | `drivers/soc/sprd/modem/sipc` and `sipc_v2`, `SPRD_SIPC_*` set in defconfig | PRESENT; PORT our `security/sipc-peer` hardening |
| SC2721 PMIC core, regulators, ADC, EFUSE, RTC, EIC, BLTC/LED, 7sreset | `sprd,sc2721-*` | `sc2721-regulator.c`, `sc2721.dtsi`, `mfd/sprd-sc27xx-spi.c`, `sc27xx-*` drivers | PRESENT (DT compatibles differ: stock uses `sprd,sc2721-*`, 4.14 uses `sprd,sc27xx-*` with `sc2721-*` second) |
| SC2721 charger (`sprd_2721_charge`, chg-timer, fchg, chg-wdt, `batt_idle`, CV tweak) | yes | only `sc2720_charger`, `sc2731_charger`, `sc2730_fast_charger`; **no sc2721 charger** | DONE compile-only: `sprd,sc2721-charger` in `sc2720_charger.c` with CV override, input OVP, adapter current cap (`SC2721_CHARGER_MAP.md`); chg-timer, fchg, ITERM/recharge, OVP/CV interrupts still PORT |
| Fuel gauge (`sprd_27xx_fgu`, `gauge-resync`) | yes | mainline-style `sc27xx_fuel_gauge.c` (sc2720/30/31 data; DT lists `sprd,sc2720-fgu` fallback) | DONE compile-only: `sprd,sc2721-fgu` data, optional battery-detect gpio, CV-hold resync (off by default) (`SC2721_FGU_MAP.md`) |
| USB MUSB, PHY, role | `musb_sprd`, our `USB_MUSB_SPRD_ROLE_SYSFS`, host idle timeout | `musb_sprd.c` (`sprd,sharkle-musb`), `phy-sprd-sharkle.c` (`sprd,sharkle-phy`), `MUSB_DUAL_ROLE` | PRESENT; PORT role sysfs and `host_idle_timeout` |
| GPU Mali Midgard | out-of-tree kbase, UK ABI 11.13 (`drivers/gpu/arm/midgard`, `mali.ko`) | only DT node `sprd,mali-midgard` (`gpu@60000000`); **no kbase source** | PORT kbase (4.4 source against the 4.14 API: DRM/ion/dma-buf/timer API changes); userspace blob compatibility must be kept |
| Camera (DCAM/ISP/CSI, sensors) | `drivers/misc/sprd_camera` | only mipi-csi-phy DT stubs; no camera driver | MISSING in 4.14 (PORT from 4.4 if the watches need it; DM63/DW100U camera use must be confirmed) |
| Touch: CST0xx (DM63), Hynitron `hyn_ts` / CST9217 (DW100U), Goodix | `cst0xx_ts`, `hynitron_ts`, `hynitron_cst9217`, `goodix5`/`goodix_5xxx` | `adaptive-ts` (FocalTech/MStar/dummy), `goodix.c`, `edt-ft5x06`, `chipone`, `stmfts` | CST0xx and CST9217 PORTED (compile only); `hynitron_ts` and Goodix not ported |
| Sensors: sc7a22, qma6981, och1970, hrs3xxx | `drivers/input/misc/*` (our drivers) | only generic IIO accel drivers and `iio/sprd_hub` sensor hub | PORTED (compile only) |
| Fingerprint (Goodix TEE, DW100U) | `goodix_fp`, `gf-keys`, netlink 29 | none | PORT |
| Trusty | `drivers/trusty` | `drivers/trusty` (same layout, plus virtio/ipc/irq) | PRESENT; check against the TEE image the watches ship |
| Watchdog | sprd wdt | `sprd_wdt.c` exists, but `CONFIG_WATCHDOG_CORE` is not set in defconfig, `SPRD_WATCHDOG` off | PRESENT (enable) |
| Thermal | `sprd_*_thm`, sc27xx tsensor | many `sprd_*_thm`, `sc27xx_tsensor_thermal`, cpu cooling | PRESENT (DT thermal zones for the watch SKUs to check) |
| Audio (VBC/codec sc2721) | yes | `sound/soc/sprd` with `vbc-rxpx-codec-sc27xx`, `audiosipc` | PRESENT (DT differs, `sprd-sound-sharkl.dtsi`) |
| ION | yes | `ION`, `ION_SPRD` | PRESENT; our `security/ion-lifetime` fixes to re-check |
| VSP/JPG | `sprd_vsp/jpg` | `sprd_vsp`, `sprd_jpg`, both with sharkle compatibles | PRESENT |
| Security backports (stable, CIP, UNISOC bulletins) | 4.4.302+ | 4.14.336 (all 4.14.y stable releases applied, see `KERNEL_4.14_STABLE.md`); 2019 BSP base | DONE for stable; `android-4.14-stable` audited (`ANDROID_4.14_AUDIT.md`), 14 of the top 15 picks plus an f_accessory sync applied; UNISOC bulletins still to audit |

### DT binding comparison (stock watch DT vs 4.14 drivers, high level)

- Display: the stock DT is ADF-style (`sprd-adf`, `sprd-panel-if` with `panel-drivers`, per-panel nodes
  with `init-data`, `power-on-sequence`, `display-timings`, `oled-backlight`, selected by `lcd_id=`).
  4.14 expects `sprd,display-processor`, `sprd,dsi-host`, `sprd,dsi-phy`, `sprd,generic-mipi-panel`, a
  `lcds` node with `sprd,dsi-*`, `sprd,initial-command`, `sprd,sleep-in/out-command`,
  `sprd,reset-on/off-sequence`, `sprd,esd-*`. Same hardware blocks, different schema: not a drop-in.
- WCN: stock uses `sprd,integrate_marlin`, `sprd,integrate_gnss`, `sprd,gnss_common_ctl`,
  `sprd,wcn_internal_chip`, `sprd,swcnblk` and `sprd,sipc-v1`. The 4.14 sprdwcn matches
  `sprd,integrate_marlin`/`integrate_gnss`/`marlin3`; property names and the SIPC channel layout still
  need a per-node diff (`tools/dt_driver_check.py` from the 4.4 tree can be reused).
- PMIC: stock `sprd,sc2721-*` (charger, fchg, chg-timer, chg-wdt, fgu, regulator, ...) versus 4.14
  `sprd,sc27xx-*` first with `sc2721-*` as fallback; the charger node has no matching driver in 4.14.
- USB: `sprd,sharkle-musb` and `sprd,sharkle-phy` exist in 4.14; the stock names have to be checked
  against them (4.4 uses the same strings for MUSB).
- GPU: `sprd,mali-midgard` is the same name, but the DT consumer is absent without kbase;
  `sprd,mali-power-model` and the DVFS/clock syscon names need the kbase platform glue.

## 4. What a 4.14 kernel enables

- cgroup BPF (`CONFIG_CGROUP_BPF=y`, `BPF_SYSCALL=y` already set in `sprd_sharkle_defconfig`),
  `NETFILTER_XT_MATCH_BPF`, PSI, `SCHEDUTIL`, F2FS encryption and binder devices are present. 4.4
  lacks cgroup BPF, which Android netd's traffic accounting and firewalling need from Android 9 on.
- Android 11 and up need eBPF programs loaded by netd/bpfloader (cgroup skb/sock, tracepoint-based
  traffic stats), and Android 12/13 additionally rely on features such as `CONFIG_BPF_JIT`,
  `CONFIG_BPF_JIT_ALWAYS_ON`, `CONFIG_NETFILTER_XT_MATCH_BPF`, `CGROUP_BPF`, and the 4.14 Android
  common-kernel patch set (ANDROID_PARANOID_NETWORK paths, incremental-fs for Android 11 APK
  streaming, and similar). These come from the AOSP `android-4.14-stable` branch (`aosp` remote), not
  from this vendor tree alone. Check the exact minimum kernel per release in the Android kernel
  lifecycle documentation before committing to a target: 4.14 is at the low end for Android 12/13
  and, to my knowledge, no longer accepted as an upgrade kernel for Android 14, so 4.14 is a one or two
  release step, not a long-term answer. (Not verified here: no network access in this step.)
- GKI does not apply: GKI starts at 5.4, and this SoC's vendor modules and DT layout are not GKI-ready.

## 5. Main risks

1. Display is a rewrite of the DT layer (ADF to DRM), including 10+ OLED/LCD panels, brightness tables
   and `lcd_id` selection. Panel init sequences must be converted without hardware to test against.
2. Mali kbase must be ported from 4.4 to 4.14 internals; the userspace GLES blob and kbase UK ABI
   (11.13) must stay compatible.
3. Charging path: no sc2721 charger in 4.14; battery safety (CV, timers, watchdog) must be revalidated
   on hardware, which we cannot do now.
4. The tree is a 2019 BSP snapshot with no history. 4.14.y stable is now applied up to 4.14.336
   (53 of 203 releases had conflicts, resolved by hand); none of it is boot-tested.
5. `-Wno-error` and warning suppressions mask real defects in vendor code.
6. Our 4.4 watch features (AOD, OTG role, GNSS/SIPC tweaks, four sensors, three touch drivers,
   Goodix FP) all need porting; none exist in 4.14.
7. Bootloader/boot-image compatibility (cmdline `lcd_id=`, DTB selection, `sprd_bootimg.py`) is untested.
8. No hardware access in this step: nothing is boot-tested; the Image has only been compiled.

## 6. Recommended next step

Done (compile only, nothing booted): 4.14.y stable to 4.14.336, the top `android-4.14-stable` fixes, input
drivers, SC2721 charger and gauge, the DM63 board DTS (`sp9832e-1h10-dm63.dts`, `dm63/DTS_414.md`) and the
CO5300 panels on DRM.

Next:
1. First boot of the DM63 on 4.14 (needs the user's OK to flash): console, USB, eMMC, then display. The first
   charge must be attended (see `dm63/DTS_414.md`).
2. Userspace: the Android 8.1 vendor hwcomposer/gralloc use ADF; on 4.14 DRM a DRM-capable composer is needed,
   which ties a 4.14 kernel to a newer Android build (GSI with a DRM HWC) rather than the stock vendor image.
3. Mali kbase port (needs the kbase source tarballs).
4. Board DTS for the other watches (DW100U next), AOD on DRM, WCN/GNSS tweaks, SIPC hardening.

## 7. Driver port status (branch `kernel-4.14-ports`)

Source: the 4.4 product tree, branch `security-backports`. All builds below are compile and link only;
nothing has run on a watch.

### 7.1 Input drivers (ported, builds, untested)

| Driver | Files | Kconfig | Status |
|---|---|---|---|
| CST0xx (DM63) | `touchscreen/cst0xx_ts.c` | `TOUCHSCREEN_CST0XX` | ported, builds |
| Hynitron `hyn_ts` / CST9217 (DW100U) | `touchscreen/hynitron_cst9217.c` | `TOUCHSCREEN_CST9217` | ported, builds |
| SC7A22 accelerometer | `misc/sc7a22.c` | `SENSORS_SC7A22` | ported, builds |
| QMA6981 accelerometer | `misc/qma6981.c/.h` | `INPUT_QMA6981_I2C` | ported, builds |
| OCH1970 magnetometer | `misc/och1970.c` | `SENSORS_OCH1970` | ported, builds |
| HRS3xxx heart rate | `misc/hrs3xxx.c` | `INPUT_SENSORS_HRS3XXX` (+ `HEARTRATE_HRS3918/3300`) | ported, builds |

Notes:
- `hyn_ts` (the rewritten driver) and CST9217 are the same source file in 4.4 (`hynitron_cst9217.c`,
  input name `hyn_ts`, compatible `hynitron,hyn_ts`). The old vendor directory
  `touchscreen/hynitron_ts/` is not built in 4.4, relies on a prebuilt gesture library and was not
  ported.
- API changes needed: none. 4.14.133 still provides everything these files use: `devm_gpiod_get_index`,
  `of_get_named_gpio` + `devm_gpio_request_one`, `input_mt_*`, delayed work/hrtimer, regulator consumer
  API. They use no `setup_timer`, `struct timeval` or `do_gettimeofday`, so no `timer_setup`/`from_timer`
  conversion was required. The 4.4 robustness fixes (probe hardening, devm, late suspend/early resume,
  ack of failed reads, clamping) came over unchanged because the files were copied verbatim.
- Warnings: the default build is clean for all six files. `W=1` still reports `qma6981.c` unused
  `rc`, an always-true `position >= 0` on an unsigned value and unused `qmax981_cvt`; kept to stay
  identical to 4.4.
- Untested: probe on real hardware, DT bindings against the 4.14 DTS (no watch DTS exists yet),
  interrupt trigger behaviour, suspend/resume with the 4.14 PM core, the sensor HAL ABI.

### 7.2 SC2721 charger: steps 1 to 4 done (compile only), see SC2721_FGU_MAP.md

Status: `sprd,sc2721-charger` match data and the CV override are in `sc2720_charger.c`, compile only; the register comparison and the parts left disabled (OVP value, ITERM, recharge, timers, fast charge) are in `SC2721_CHARGER_MAP.md`. Step 3 (gauge resync) and the DTS nodes are still open. The original plan follows.

#### Plan

Why a straight port is not possible: `sprd_2721_charge.c` is only a backend of the legacy Spreadtrum
battery framework. It registers through `sprdbat_register_ext_ops()` and uses `struct
sprdbat_drivier_data` from `sprd_battery.h`, `sprd_charge_helper.h`, the chg-timer/fchg/helper files, and
the fuel gauge calls `sprdfgu_read_soc`, `sprdfgu_read_vbat_ocv`, `sprdfgu_read_batcurrent`,
`sprdfgu_ocv_to_capacity`, `sprdfgu_force_set_soc` from `sprd_27xx_fgu.c`. 4.14 has none of that: its
model is `charger-manager` (already in the `sl8541e-1h10.dts` charger-manager node) plus power_supply
drivers, with `sc27xx_fuel_gauge.c` as the gauge (DT fallback `sprd,sc2720-fgu`). Porting the charger
would mean porting `sprd_battery.c` and `sprd_27xx_fgu.c` and dropping the 4.14 gauge, which is a
rewrite of the fuel gauge path and was out of scope.

4.14-native approach:
1. Charger: extend `drivers/power/supply/sc2720_charger.c` (compatible `sprd,sc2720-charger`; it is the
   closest in-tree driver, with the same CCCV code scheme: `CHG_CV_V` field, efuse `cccv_calib` cell, 75 step
   constants) and add a `sprd,sc2721-charger` compatible with its own match data. Every register
   address, field mask, step size and calibration offset for the SC2721 must be taken from our 4.4
   `sprd_2721_charge.c`/`sprd_2721_charge.h` and `sc2721_glb.h`; do not reuse the sc2720 values unchecked.
   Charge timers/watchdog/fast charge (`sprd2721_chg_timer.c`, `sprd_2721_fchg.c`) map to charger-manager
   feeding plus a small power_supply property set, or a separate `sc2721_fast_charger` modelled on
   `sc2730_fast_charger.c`.
2. CV override: implement inside the charger driver's `POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX`
   set path (the point where the CCCV code is programmed). Keep the 4.4 module parameter
   `cv_override_mv` (range 4000 to 4400, 0 = off), the lock around the END_V and CV_V writes, the
   exact representable-CV model (highest CV not above the target; if unreachable program the lowest
   CV, END_V written first) and the "write CV code to 0 first" monotone step.
3. Gauge resync: do it in `sc27xx_fuel_gauge.c`, not in the charger. The charger exposes
   "override active" and "CV reached" through power_supply properties (or a notifier); the gauge
   driver reads OCV (`sc27xx_fgu_get_vbat_ocv`), current, and after the 4.4 rest/threshold conditions
   calls the existing `sc27xx_fgu_adjust_cap()` (which rewrites the coulomb counter through
   `sc27xx_fgu_set_clbcnt`) with the capacity from its own OCV table. This replaces
   `sprdfgu_force_set_soc` and keeps 4.14 gauge ownership of the counter.
4. Check first: the sc2720 gauge data (`sc2720_info`) against the SC2721 gauge registers and OCV tables
   in the 4.4 `sprd_27xx_fgu.c`; the DTS currently claims `sprd,sc2720-fgu` as a compatible only.
5. Needs hardware validation (CV, timers, thermal, watchdog) before any real battery use.

## 8. Config fragment and build

- `arch/arm64/configs/watch_414.fragment` enables all ported drivers as `=y`.
- `devices/merge_watch_414.sh <build-dir>` runs `sprd_sharkle_defconfig`, merges the fragment with
  `scripts/kconfig/merge_config.sh` and runs `olddefconfig`. Then build with
  `make O=<build-dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs`.
- Result: `Image` and all `sprd/*.dtb` build with the fragment (GCC 16); the six ported objects are
  linked in and no build warning mentions them. The build directory was deleted afterwards.
