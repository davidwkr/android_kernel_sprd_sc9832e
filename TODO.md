# TODO: kernel for DW100Ultra and DM63 watches

Goal: build this tree (UNISOC SC9832E / SL8541E, Linux 4.4.147, from LineageOS 17.1) for the DW100Ultra (two hardware variants) and the DM63 smartwatches, which ship Android 8.1 on a vendor 4.4.83 kernel.

State as of 2026-10-30: everything below marked done is merged into `security-backports` and builds; **nothing has been flashed or run on a watch yet**. Current status and handover: [`STATUS.md`](STATUS.md).

Hand-over documents:
- [`STATUS.md`](STATUS.md): what is done, branches, build commands, what is open.
- [`devices/README.md`](devices/README.md): common platform, boot image format, partitions.
- [`devices/dw100u-silver/SPEC.md`](devices/dw100u-silver/SPEC.md), [`devices/dw100u-v13m/SPEC.md`](devices/dw100u-v13m/SPEC.md), [`devices/dm63/SPEC.md`](devices/dm63/SPEC.md): per-device specs, with the stock kernel config (`stock.config`) and device tree (`stock.dts`) next to each.
- [`KERNEL_TWEAKS.md`](KERNEL_TWEAKS.md): battery, flash-write and GNSS changes, with the measurement method.
- [`SECURITY_BACKPORTS.md`](SECURITY_BACKPORTS.md): missing security fixes (Mali GPU, 4.4.147 → 4.4.302 stable, CIP, UNISOC bulletins) and hardening.

## Target devices

All three are the same board family: `model = "Spreadtrum SL8541e Board"`, `compatible = "sprd,sp9832e-1h10", "sprd,sc9832e"`, platform `sp9832e`, board `sl8541e_1h10`, Android 8.1, stock kernel 4.4.83 (GCC 4.9).

| Device | Firmware | Stock kernel build | Device tree |
|---|---|---|---|
| DW100Ultra silver | V11H | Dec 18 2025, `power@power-X99` | 137 KB DT table, 1 DTB |
| DW100Ultra (other) | V13M | Jun 17 2026, `powersys@X99-7` | 137 KB DT table, 1 DTB |
| DM63 | DM63B V1.3 | Jan 20 2026, `power@power-X99` | 272 KB DT table, 1 DTB |

Boot image (all three):
- `ANDROID!` header v0, page size 2048, kernel at `0x8000`, ramdisk at `0x05400000`, tags at `0x100`, no second stage.
- Kernel: uncompressed arm64 `Image` (about 18 MB).
- Cmdline: `console=ttyS1,115200n8 buildvariant=user`.
- The device tree is appended as a UNISOC DT table (magic `SPRD`). Its size is stored in the header field after `page_size` (where header v1 has `header_version`), so it needs UNISOC's `mkbootimg` or an equivalent repacker.

Partitions are the same on all three: `boot`, `recovery`, `vbmeta` (+ `_bak`), `system`, `vendor`, `userdata`, modem/WCN/GNSS partitions. Verified boot is `orange` (bootloader unlocked) on the units checked; the DW100U silver runs Magisk (patched boot ramdisk).

Backups of the stock boot partitions, `/proc/config.gz` of each unit and the extracted DTBs are kept locally, outside this repo. Do not commit them (device-specific data).

## Hardware differences

### Kernel config

- DW100U silver vs V13M: identical except `CONFIG_RTC_START_YEAR` (2025 vs 2026). One kernel serves both DW100U variants.
- DW100U vs DM63: only board peripherals differ.
  - DW100U only: `GOODIX_TEE_FINGERPRINT`, `HEARTRATE_BD1820`, `SENSORS_MSP891`, `SENSORS_OCH1970`, `INPUT_UINPUT`.
  - DM63 only: `HEALTH_GOODIX`, `SUNWAVE_FINGERPRINT`, `INPUT_FALL_OFF`.

### Device tree, DW100U silver vs V13M

Never swap these device trees between the two variants: the battery profiles differ.

| | Silver (V11H) | Other (V13M) |
|---|---|---|
| Charge end voltage | 4.395 V (`chg-end-vol` 0x112b) | 4.43 V (0x114e) |
| Battery capacity (`cnom`) | about 940 mAh | about 1080 mAh |
| Battery NTC (`temp-support`) | off | on (own temperature table) |
| Charge current | 650 mA | 800 mA |
| JEITA / OCV tables | silver battery | V13M battery |
| Buttons | volume-down key + second key (GPIO 0x2c / 0x2e) | one key on another GPIO controller |

Panel (368x448 MIPI), touch, sensor, WCN and memory nodes are the same.

### Peripherals actually bound on the running watches

| Function | DW100U silver | DM63 |
|---|---|---|
| Touch | Hynitron `hyn_ts` @ i2c 3-005a | CST0xx `cst0xx_ts` @ i2c 3-0038 |
| Accelerometer / steps | `sc7a22` | `sc7a22` |
| Heart rate | `hrs3xxx` @ 0x44 | `hrs3xxx` @ 0x44 |
| Crown | – | `rotary@0` (GPIO rotary encoder) |
| Fingerprint | Goodix (`gf-keys`) | none bound |
| Camera sensors | `sensor_main` | `sensor_main`, `sensor_sub` |
| Unbound in DT | qmax981, och1970, btl_ts, cst0xx | af6837, qmax981, btl_ts, hyn_ts |

The V13M DW100U was not connected: its touch is assumed to be the Hynitron one (same DT and config as the silver), not confirmed.

## Gap analysis: device config vs this source

Each device enables about 1,240 options. Options with no `config` entry in this tree:

| Option | DW100U | DM63 | Needed? |
|---|---|---|---|
| `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR`, `TOUCHSCREEN_CTS0XX_E10_GS17` | x | x | **Yes for DM63** (its only touch) |
| `TOUCHSCREEN_BETTERLIFE_COB_JR` | x | x | No (`btl_ts` unbound on both) |
| `INPUT_SENSORS_HRS3XXX`, `HEARTRATE_HRS3918` | x | x | **Yes** (heart rate on both) |
| `INPUT_SENSORS_BD1XXX`, `HEARTRATE_BD1820` | x | x (BD1XXX) | No (not bound) |
| `INPUT_QMAX981` | x | x | No (not bound) |
| `SENSORS_COMPASS_AF6837` | x | x | No (not bound) |
| `SENSORS_MSP891`, `SENSORS_OCH1970` | x | – | No (not bound) |
| `INPUT_FINGERPRINT`, `GOODIX_TEE_FINGERPRINT` | x | – | Check whether the DW100U uses the fingerprint at all |
| `HEALTH_GOODIX`, `SUNWAVE_FINGERPRINT`, `INPUT_FALL_OFF` | – | x | Check (`fall_off` may be wrist detection) |

Present in this tree and used: ADF display (`drivers/video/adf/sprd`, matches stock `sprd-adf`), Hynitron touch (`drivers/input/touchscreen/hynitron_ts`, check the `hynitron,hyn_ts` compatible and Makefile hookup), `SENSORS_SC7A22`, `INPUT_GPIO_ROTARY_ENCODER`, `SPRDWL_NG`, `INTEGRATED_MARLIN2_GE2`, `SPRD_WCN_BT`, `SPRD_WCN_FM`, Mali Midgard (`drivers/gpu/arm/midgard`).

## Plan

### 1. Toolchain

- [x] `aarch64-linux-gnu-gcc` (GCC 16) installed; all six devices build with `scripts/watch_build.sh`.
- [ ] Note: the Android NDK (r27, r29) has no GNU binutils; clang 23 + lld alone is not enough for 4.4 arm64.

### 2. Defconfig

- [x] Partly done: base config is reproducible in tree: `sprd_sharkle_defconfig` + `watch_base.fragment` (only `SPRDWL_NG=m` was missing) + `watch_hardening.fragment` + device fragments, assembled and verified by `scripts/watch_build.sh DEVICE`. The stock-config gap (options in `stock.config` that we lacked) is classified in [`devices/CONFIG_PARITY.md`](devices/CONFIG_PARITY.md); the A items (Trusty, crown, AP watchdog, `SPRD_SIPC_WCN`, `TASK_DELAY_ACCT`, `SC7A22`) are in `watch_stock_parity.fragment` / `watch_sc7a22.fragment`, still untested on a watch. Standalone `dm63`, `dw100u`, `mt107`, `mt108`, `dm82` `_defconfig` files are generated from the fragments (see below). Still open: the `/proc/config.gz` comparison.
- [ ] Start from `sprd_sharkle_defconfig`, then merge the DW100U `/proc/config.gz` (resolve options this tree lacks or renamed; keep 4.4.147 defaults where the old option no longer exists).
- [x] Done: `arch/arm64/configs/{dm63,dw100u,mt107,mt108,dm82,k18}_defconfig`, generated by `scripts/watch_defconfig.sh DEVICE|all` (same assembly as `watch_build.sh` via `scripts/watch_lib.sh`, then `savedefconfig`); `--check` regenerates and fails on drift. Each `make <device>_defconfig` gives a `.config` identical to the one `watch_build.sh` produces. One `dw100u_defconfig` serves silver and V13M: the build assembles identical configs for both, and the only stock difference (`CONFIG_RTC_START_YEAR` 2025 vs 2026) is not set by us. The fragments stay the source of truth: edit them, rerun the script, commit both.
- [ ] Keep Android 8.1 userspace needs: binder (non-VNDK layout), SELinux, the ADF display, `/proc/config.gz`, `CONFIG_MODULES` with the same module layout.
- [ ] Decide on KernelSU (this tree already carries KSU hooks, see the warn-fix commits); the DW100U silver uses Magisk today.

### 3. Device trees

- [ ] Use each device's own DTB from its stock boot image (silver, V13M, DM63), not this tree's `sp9832e-1h10-*.dts`, for the first boot. That keeps panel timing, pinmux, battery and keys unchanged.
- [x] Done: stock DTs vs this tree's drivers compared, see `devices/DT_BINDINGS.md` (`tools/dt_driver_check.py`). No boot-blocking mismatch. `dm63_touch.fragment` now also builds `TOUCHSCREEN_CST9217` (insurance for a DM63 revision with the Hynitron chip; `dm63_defconfig` regenerated). Open there: panel `lcd_id` must exist in each DT, AOD on non-CO5300 panels is unverified.
- [x] Done: `sp9832e-1h10-{dw100u-silver,dw100u-v13m,dm63,mt107,mt108,dm82}.dts` are in `arch/arm64/boot/dts/sprd/` (labelled copies of the stock DTs, built by `make dtbs`, byte-identical to the stock DTBs when compiled with the tree's dtc; see `devices/README.md`, `tools/dtcompare.py`). The first boot still uses the stock DT table from the boot image; the next step is the binding comparison in the item above.

### 4. Missing drivers

- [x] **CST0xx touch (DM63).** Done: `cst0xx_ts` (type B, late PM so a wake tap is not lost), `dm63_touch.fragment`; the DW100U `hyn_ts` driver rewritten (`dw100u_touch.fragment`). Not tested on hardware. Original item: Hynitron family; check whether `hynitron_ts` handles the CST0xx protocol and add the `cst0xx,cst0xx_ts` compatible, otherwise port an open-source CST0xx/CST8xx driver. Without this the DM63 has no touch.
- [~] **HRS3xxx heart rate (both).** Done: input device `heartrate`, sysfs `enable` / `chipinfo` (`watch_hrs.fragment`). It reports no heart rate: the HR/SpO2 algorithm is closed vendor code (next step: GPL source request to the vendor). Original item: Port a TYHX HRS3300/HRS3xxx driver, matching the stock input device name `heartrate` and its sysfs/ioctl interface so the vendor heart-rate HAL keeps working (check the HAL in `/vendor` for what it opens).
- [x] `aod_dtap` input device on the DM63 (from the custom `aod_idle` module): ported in tree as `CONFIG_SPRD_AOD_IDLE` (`drivers/video/adf/sprd/drv/sprd_aod_idle.c`, fragment `dm63_aod.fragment`); builds as `=m` and `=y`. **Not yet tested on hardware** (see `KERNEL_TWEAKS.md`, Display/AOD). Open: confirm generic vs DCS write path on the CO5300, userspace `insmod` name.
- [ ] DW100U AOD in kernel: `sprd_aod_idle` now also matches `hyn_ts` (`dw100u_aod.fragment`); same CO5300 command set per the stock DTs. **Needs hardware test** (steps in `KERNEL_TWEAKS.md`). Userspace to write: `aod_idled.sh` + `.rc` replacing the Magisk `wearit_aod`, plus the overlay `config_powerDecoupleAutoSuspendModeFromDisplay=true` (otherwise no suspend in AOD). Open: double tap across system suspend with `hyn_ts` (one report read on resume).
- [ ] Fingerprint, fall-off: decide after checking whether anything on the watches uses them.

### 5. Modules

- [x] Ramdisk modules loaded at boot: `mali.ko`, `sprdwl_ng.ko`, `sprd_fm.ko`, `sprdbt_tty.ko`: all four build against this tree (commands in STATUS.md). Build them from this tree (or build them in) for the new kernel.
- [ ] Custom out-of-tree modules: `aod_idle.ko` (DM63, AOD idle + `aod_dtap`; **ported in tree as `sprd_aod_idle`, `dm63_aod.fragment`, needs hardware test**), `batt_idle.ko` (DM63, SC2721 charger CV override, loaded by `/system/etc/init/batt_idle.rc`; **replaced by `cv_override_mv` in `sprd_2721_charge`**, see section 8), `wearit_aod` (DW100U, Magisk module). Find their sources and rebuild them against this kernel.
- [ ] Check `/vendor/lib/modules` and `/system/lib/modules` on each watch for any other `.ko` loaded at boot.

### 6. Boot image

- [x] Repack: stock ramdisk (or Magisk-patched ramdisk on the silver) + new `Image` + device DT table, same header fields as stock. `tools/sprd_bootimg.py pack DIR OUT --kernel Image` does this (size check done on a DM63 copy: 25.7 MB used of 35 MB). Not yet booted on hardware.
- [x] Write a repack script that reproduces the stock boot image byte-for-byte from its parts first (proves the packer), then swaps the kernel and modules. Done for kernel, ramdisk and DT table (6 of 6 stock dumps identical). Module swap inside the ramdisk done: `tools/sprd_ramdisk.py replace-modules` (round trip of the decompressed cpio identical on 6 of 6 stock ramdisks; vermagic checked like `same_magic()` against the new `Image`; DM63 dry run packs and `info` is clean). Not booted on hardware. `scripts/watch_build.sh` strips the modules with `--strip-debug`.

### 7. Test (safe order)

- [ ] **Prerequisites, all mandatory and done before any flash** (each is per device model, and each needs the owner's OK because it is device access):
  - full backup of the target partitions (at least `boot`, `recovery`, `vbmeta` and the partition table), verified by checksum (read back or dump twice and compare);
  - the unbrick tool (UNISOC download mode with `sfd_tool`) tested to work on that model, using the backup;
  - bootloader state confirmed unlocked (orange) on that unit;
  - the recovery partition's own AVB needs: read its size first and pack with `--partition-size SIZE` (the AVB footer must sit at its end); dump the stock recovery, unpack it and reuse ITS `vbmeta.bin` (partition `recovery`, recovery key) with our kernel + boot ramdisk + DT table; do not use `--no-avb` for the first test. Details in `devices/README.md`, "Repacking the boot image".
- [~] `scripts/first_boot.sh` (branch `tools/first-boot`, not merged) automates these steps: `check`, `backup` / `backup-full` (optional `--stock-ref` to prove the stock recovery), `build`, `flash` (recovery only, every target checked before `dd`), `verify`, `restore`, `boot-test`, `collect`. Its latest review fixes are uncommitted until the fake-adb test suite (`scripts/first_boot_test/run.sh`) has been run.
- [ ] Only then: flash the test image to the **recovery** partition and start it with `adb reboot recovery`. If it doesn't boot, a normal reboot uses the untouched `boot` partition.
- [ ] DW100U silver first (touch driver already present), then V13M, then DM63 (after the CST0xx port).
- [ ] Checklist per device:
  - display
  - touch
  - crown / keys
  - Wi-Fi
  - Bluetooth
  - mobile data and calls (modem)
  - GNSS fix and A-GPS
  - charging and battery percentage
  - heart rate
  - step counter
  - suspend / resume (`/d/suspend_stats`)
  - AOD
  - reboot loop check
- [ ] Only after a full pass: flash to `boot`. Keep the stock boot image to roll back.

### 8. Kernel changes wanted (after the first booting build)

Details, measurements and code pointers in [`KERNEL_TWEAKS.md`](KERNEL_TWEAKS.md). Summary:

- [x] GNSS: short per-channel sbuf rx wakelock (done: 100 ms max, released about 20 ms after the HAL drained the ring; **hardware test pending**) for the GNSS channel (stock holds 500 ms per burst: about 88 % awake during a GPS session); wake-enabled receive interrupt. Engine stop analysed (core, rail and WCN domain do power down on `gnss_power_enable=0`; no bug on the normal path) and `gnss_engine_state` sysfs added: **hardware test pending** (steps in KERNEL_TWEAKS.md). Failure paths fixed (`fixes/gnss-failpaths`, merged): a failed start unwinds what it acquired, restarts rate limited, `gnss_engine_state` reports `error`; hardware test pending.
- [~] Flash writes: `dirty_expire_centisecs` / `dirty_writeback_centisecs` 3000 via `tools/sprd_ramdisk.py add-rc` (adds `/init.watch_tweaks.rc` from `devices/common/`, imported last from `/init.rc` so it overrides stock `dirty_expire_centisecs 200`; tool and tests done, **not booted on a device**; replaces the userspace workarounds, remove those after the boot test; see `devices/README.md`); test without f2fs online `discard`; check f2fs background GC; no periodic kernel writes.
- [~] Battery (done: BT rx hold 200 ms, PM print thread off by default; suspend failures traced to userspace alarms, see KERNEL_TWEAKS.md section 3): find the suspend failures (about 40 % of attempts fail on stock), audit wakeup sources (BT 5 s, FM 1 s holds), CPU governor and hotplug limits with the screen off, `cluster_pd` residency.
- [~] Charging: DM63 `batt_idle` CV override into the charger driver (merged: `cv_override_mv` module parameter / `sprd,cv-override-microvolt`, off by default, never above min(request, `chg-end-vol`, override); not tested on hardware; fuel-gauge resync ported as `gauge_resync`, off by default, merged; open: SELinux write check); per-unit battery profiles stay in each unit's DT.
- [~] USB host mode: `CONFIG_USB_MUSB_SPRD_ROLE_SYSFS` (`watch_otg.fragment`) adds `/sys/kernel/otg_role/role` (host|device) to the SPRD MUSB glue, replacing the out-of-tree `otg_role.ko` (merged on `drivers/otg-role`, built, **not run on hardware**). Open: the tree has no VBUS source on any board (FAN54015 on a disabled i2c bus, SC2721 ops without `otg_charge_ext`): host needs an externally powered OTG adapter or self-powered hub, `host` is refused unless `musb_sprd.host_without_vbus=1`; host mode blocks suspend (the idle timeout below limits it); stock `sprd_glue` layout (notifier at 0x2c0) not identified. Steps in KERNEL_TWEAKS.md section 5.
  - [x] **Host mode idle timeout** (`host_idle_timeout`, default 300 s, 0 = off; builds, not tested on hardware): return to device mode automatically after a few minutes with no USB device enumerated (boost off, wake lock released). There is no ID pin, so this is the only way to notice the adapter was removed. Cheap; do first.
  - [-] **Suspend in host mode:** reviewed, not implemented. Today a connect during suspend is lost (`musb_suspend()` masks MUSB irqs and ends the session, `musb_runtime_suspend()` clears it in host mode; `musb_resume()` only restores it afterwards via `musb_start()`), and no tree evidence shows a wake route for the MUSB irq (SPI 55): `enable_irq_wake()` on the GIC only sets a flag (`IRQCHIP_SKIP_SET_WAKE`), `sprd-wakeupen-intc.c` matches no sc9832e DT node, and `pm-sharkle.c` only saves/restores the AP intc enables and enables dma/gpio/kpd/ana/timer in the AON intc. Would need: keep the session and connect irq across suspend plus the USB bit in the intc wake mask, proven on hardware (steps in KERNEL_TWEAKS.md section 5). Safe path stands: `host_idle_timeout` returns to device mode and releases the lock.

### 8b. More devices: MT107, MT108, DM82

Specs from the stock boot dumps only (no unit examined): [`devices/mt107/SPEC.md`](devices/mt107/SPEC.md), [`devices/mt108/SPEC.md`](devices/mt108/SPEC.md), [`devices/dm82/SPEC.md`](devices/dm82/SPEC.md). All three: stock 4.4.83, same SL8541E 1H10 board, one DTB each (use it for the first boot), SC2721 charger, WCN/GNSS and cameras already in this tree.

| | MT107 | MT108 | DM82 |
|---|---|---|---|
| Panel (stock U-Boot log) | CO5300, 412x502 | ST77916 TFT LCD, 360x360 | CO5300, 468x466 |
| Touch candidates in DT | `cst0xx_ts`, `hyn_ts`, `btl_ts` | `cst0xx_ts`, `btl_ts` | `cst0xx_ts`, `hyn_ts`, `btl_ts` |
| Heart rate in DT | `hrs3xxx`, `bd1xxx_hr` | `hrs3xxx` (stock config: HRS3300), `bd1xxx_hr` | `hrs3xxx`, `bd1xxx_hr` |
| Battery | 730 mAh, 4395 mV | 1030 mAh, 4395 mV | 1250 mAh, 4480 mV |
| USB cable detect | typec | gpios | gpios |
| `sprd_aod_idle` | applies (CO5300) | **no** (not a CO5300) | applies (CO5300) |

- [ ] Read on each unit (or a U-Boot log with the `lcd_id` line): exact `lcd_id`, which touch chip answers, which heart-rate chip, accelerometer.
- [x] Per-device config fragments (`mt107.fragment`, `mt108.fragment`, `dm82.fragment`): touch (cst0xx + hyn where the DT lists both), HRS, AOD (MT107/DM82), sc7a22 and QMA6981. Compatibles checked against the of_match tables (all match); probe error paths of both touch drivers use devm GPIO/regulator, so a non-fitted chip releases the shared GPIOs. Build-checked only.
- [x] `hrs3xxx`: accepts the HRS3300 chip ID `0x21` too (`HEARTRATE_HRS3300`, `chipinfo` reports `hrs3300`). Probe, standby and ABI only; HRS3300 measurement (ENABLE/PDRIVER setup, data registers) is not implemented and nothing is verified on a unit.
- [ ] Betterlife touch (`btl_ts`, 0x2d): port a driver only if a unit has that chip.
- [x] `och1970` Hall magnetic sensor: new driver `drivers/input/misc/och1970.c` (`SENSORS_OCH1970`, in all three fragments), written from the public OCH1970VAD-H datasheet V1.1: chip-ID check, power-down by default, polled single measurements, input device `och1970` (ABS_X/Y/Z, raw 16 bit, 1.1 uT/LSB), sysfs `enable` / `delay` (min 10 ms). The stock HAL's device name and the open/close key logic (`linux,open` etc., thresholds) are unknown and not implemented; irq pin unused. Build-checked only, not verified on a unit.
- [ ] Fingerprint (Goodix, `goodix,fingerprintree`): branch `drivers/goodix-fp` is empty; needs the vendor GPL source (request draft kept outside this repo).
- [ ] `bd1xxx_hr` heart rate and `msp891` sensors: no driver in this tree; port only if fitted and wanted.
- [x] `sprd_aod_idle`: MT107 / DM82 only (in their fragments); the match table covers `cst0xx_ts` and `hyn_ts`, so either fitted chip works.
- [~] USB role switch on `gpios` cable-detect boards (MT108, DM82): reviewed (KERNEL_TWEAKS.md section 5): same vbus handler and locking as Type-C, no charger misread, idle timeout unchanged; fixed a NULL notifier_call registered for non-typec methods. No VBUS source on any board: `host` is refused (`-EOPNOTSUPP`) unless `musb_sprd.host_without_vbus=1` (powered adapter or hub). Note: the FAN54015 bus is disabled (and its OF string has a space); not changed (needs hardware knowledge). Not run on a unit.
- [ ] Charge CV override: the driver already caps at each unit's `chg-end-vol` (4395 mV on MT107/MT108).
- [ ] MT107 camera: no ACK on i2c-0 (hardware, see the sfd_tool notes); nothing to do in the kernel.
- [x] K18 ([`devices/k18/SPEC.md`](devices/k18/SPEC.md)): spec from the stock boot dump, `k18.fragment` / `k18_defconfig`, labelled DT. Separate model: MT107 board and keys, no crown, no `msp891`, 480x480 touch, 1069 mAh battery; never use the MT107 DT. Build-checked only.
- [ ] K18: read `lcd_id`, touch chip and heart-rate chip on the unit; `bd1xxx_hr` (BD1820) has no driver; enable `SPRD_AOD_IDLE` only once the panel is known.

### 9. Security backports

Plan in [`SECURITY_BACKPORTS.md`](SECURITY_BACKPORTS.md). Order:
- [~] Mali GPU fixes (r28p0; exploited-in-the-wild CVEs): CVE-2022-38181 and CVE-2023-4211 done; CVE-2021-28663/28664 and CVE-2022-36449 need Arm's fixes.
- [x] TCP SACK, `af_unix` GC, `seq_file`, epoll cherry-picks (plus `fget` and three binder fixes).
- [x] Stable merge to 4.4.302 (per-release diffs from the stable tags; f2fs excluded from 4.4.172 on).
- [x] CIP 4.4 post-302 fixes (about 1257), UNISOC bulletins (fixes where the code could be located; table in SECURITY_BACKPORTS.md). Found on the way: USB RNDIS DoS in the stock kernel (fixed, `470aacefb`; reported to the security team, pending).
- [x] Hardening config (`watch_hardening.fragment`; `watch_release.fragment` not ready until SELinux runs enforcing).
- [x] SIPC peer checks (`security/sipc-peer`) and ION buffer lifetime (`security/ion-lifetime`, probable CVE-2023-49672), merged, build-tested only. Hardware test items in SECURITY_BACKPORTS.md.
- [ ] dcam_r6p0 kernel-pointer uapi (not built in our configs), iommuex out-of-range bypass, stale IOVA reuse: vendor issues for the security team, see SECURITY_BACKPORTS.md.

## Open decisions

- [ ] Test path: recovery partition first (recommended).
- [ ] Main goal of the custom kernel: battery and wakeups, KernelSU, other.
- [ ] Root solution: KernelSU (built into the kernel) or keep Magisk on the ramdisk.
- [ ] Whether the fork on GitHub stays public: don't commit device dumps, configs with serials, or anything from the watches' `/data`.
