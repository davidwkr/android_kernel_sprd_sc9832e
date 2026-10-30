# TODO: kernel for DW100Ultra and DM63 watches

Goal: build this tree (UNISOC SC9832E / SL8541E, Linux 4.4.147, from LineageOS 17.1) for the DW100Ultra (two hardware variants) and the DM63 smartwatches, which ship Android 8.1 on a vendor 4.4.83 kernel.

State as of 2026-10-29: analysis done, nothing built or flashed yet.

Hand-over documents:
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

- [ ] Install `aarch64-linux-gnu-gcc` (GCC 16, Arch `extra`; the repo's warn-fix commits target GCC 16). Needs sudo in a real terminal. Alternative without sudo: Arm GNU toolchain 14.x in the home directory.
- [ ] Note: the Android NDK (r27, r29) has no GNU binutils; clang 23 + lld alone is not enough for 4.4 arm64.

### 2. Defconfig

- [ ] Start from `sprd_sharkle_defconfig`, then merge the DW100U `/proc/config.gz` (resolve options this tree lacks or renamed; keep 4.4.147 defaults where the old option no longer exists).
- [ ] Create `arch/arm64/configs/dw100u_defconfig` and `dm63_defconfig` (or one shared defconfig plus a DM63 fragment).
- [ ] Keep Android 8.1 userspace needs: binder (non-VNDK layout), SELinux, the ADF display, `/proc/config.gz`, `CONFIG_MODULES` with the same module layout.
- [ ] Decide on KernelSU (this tree already carries KSU hooks, see the warn-fix commits); the DW100U silver uses Magisk today.

### 3. Device trees

- [ ] Use each device's own DTB from its stock boot image (silver, V13M, DM63), not this tree's `sp9832e-1h10-*.dts`, for the first boot. That keeps panel timing, pinmux, battery and keys unchanged.
- [ ] Decompile each DTB and compare it with `sp9832e-1h10-native.dts` / `sc9832e.dtsi` for bindings that changed between the 4.4.83 BSP and 4.4.147 (display, WCN, GNSS, charger, thermal). Fix the device DTS where a driver expects a different binding.
- [ ] Later: commit proper `sp9832e-1h10-dw100u-silver.dts`, `...-dw100u-v13m.dts`, `...-dm63.dts` into `arch/arm64/boot/dts/sprd/`.

### 4. Missing drivers

- [ ] **CST0xx touch (DM63).** Hynitron family; check whether `hynitron_ts` handles the CST0xx protocol and add the `cst0xx,cst0xx_ts` compatible, otherwise port an open-source CST0xx/CST8xx driver. Without this the DM63 has no touch.
- [ ] **HRS3xxx heart rate (both).** Port a TYHX HRS3300/HRS3xxx driver, matching the stock input device name `heartrate` and its sysfs/ioctl interface so the vendor heart-rate HAL keeps working (check the HAL in `/vendor` for what it opens).
- [x] `aod_dtap` input device on the DM63 (from the custom `aod_idle` module): ported in tree as `CONFIG_SPRD_AOD_IDLE` (`drivers/video/adf/sprd/drv/sprd_aod_idle.c`, fragment `dm63_aod.fragment`); builds as `=m` and `=y`. **Not yet tested on hardware** (see `KERNEL_TWEAKS.md`, Display/AOD). Open: confirm generic vs DCS write path on the CO5300, userspace `insmod` name.
- [ ] Fingerprint, fall-off: decide after checking whether anything on the watches uses them.

### 5. Modules

- [ ] Ramdisk modules loaded at boot: `mali.ko`, `sprdwl_ng.ko`, `sprd_fm.ko`, `sprdbt_tty.ko`. Build them from this tree (or build them in) for the new kernel.
- [ ] Custom out-of-tree modules: `aod_idle.ko` (DM63, AOD idle + `aod_dtap`; **ported in tree as `sprd_aod_idle`, `dm63_aod.fragment`, needs hardware test**), `batt_idle.ko` (DM63, SC2721 charger CV override, loaded by `/system/etc/init/batt_idle.rc`), `wearit_aod` (DW100U, Magisk module). Find their sources and rebuild them against this kernel.
- [ ] Check `/vendor/lib/modules` and `/system/lib/modules` on each watch for any other `.ko` loaded at boot.

### 6. Boot image

- [ ] Repack: stock ramdisk (or Magisk-patched ramdisk on the silver) + new `Image` + device DT table, same header fields as stock.
- [ ] Write a repack script that reproduces the stock boot image byte-for-byte from its parts first (proves the packer), then swaps the kernel and modules.

### 7. Test (safe order)

- [ ] Flash the test image to the **recovery** partition and start it with `adb reboot recovery`. If it doesn't boot, a normal reboot uses the untouched `boot` partition.
- [ ] Have a full partition backup and the UNISOC download tool (`sfd_tool`) ready for unbrick.
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

- [ ] GNSS: short per-channel sbuf rx wakelock for the GNSS channel (stock holds 500 ms per burst: about 88 % awake during a GPS session); wake-enabled receive interrupt. Engine stop analysed (core, rail and WCN domain do power down on `gnss_power_enable=0`; no bug on the normal path) and `gnss_engine_state` sysfs added: **hardware test pending** (steps in KERNEL_TWEAKS.md). Open: failed GNSS start leaves the core powered (documented, not fixed).
- [ ] Flash writes: `dirty_expire_centisecs` / `dirty_writeback_centisecs` 3000 in the ramdisk `init.rc` (replaces the userspace workarounds); test without f2fs online `discard`; check f2fs background GC; no periodic kernel writes.
- [ ] Battery: find the suspend failures (about 40 % of attempts fail on stock), audit wakeup sources (BT 5 s, FM 1 s holds), CPU governor and hotplug limits with the screen off, `cluster_pd` residency.
- [~] Charging: DM63 `batt_idle` CV override into the charger driver (done on `tweaks/charge-cv`: `cv_override_mv` module parameter / `sprd,cv-override-microvolt`, off by default; not tested on hardware; gauge resync not ported); per-unit battery profiles stay in each unit's DT.

### 9. Security backports

Plan in [`SECURITY_BACKPORTS.md`](SECURITY_BACKPORTS.md). Order:
- [ ] Mali GPU fixes (r28p0; exploited-in-the-wild CVEs).
- [ ] TCP SACK, `af_unix` GC, `seq_file`, epoll cherry-picks.
- [ ] Stable merge to 4.4.302 (via AOSP `android-4.4-q`).
- [ ] CIP 4.4 post-302 fixes, UNISOC bulletins.
- [ ] Hardening config.

## Open decisions

- [ ] Toolchain: system GCC 16 (needs sudo) or Arm GNU 14.x in the home directory.
- [ ] Test path: recovery partition first (recommended).
- [ ] Main goal of the custom kernel: battery and wakeups, KernelSU, other.
- [ ] Root solution: KernelSU (built into the kernel) or keep Magisk on the ramdisk.
- [ ] Whether the fork on GitHub stays public: don't commit device dumps, configs with serials, or anything from the watches' `/data`.
