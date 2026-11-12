# DW100U silver boot image candidate, kernel 4.14 with ADF

**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** The image is not in the repo (images are never
committed); it is kept in `~/.cache/dw100u-silver-414-candidate/dw100u-silver_414_adf_ramoops_candidate.img` (and `..._BENCHDEBUG.img`, see below). This is a bring-up image, built the same way as the K18 one: see
`../k18/BOOT_414.md` for the commands, the contents and what to expect, which apply here except as noted.

Built from `kernel-4.14` commit `cfe3fd4ff26e` (2026-11-11), kernel release 4.14.336+, config `sprd_sharkle_defconfig` +
`watch_414.fragment` + `watch_414_adf.fragment` + `watch_414_stock81.fragment` + `watch_414_ramoops.fragment`, DTB `sp9832e-1h10-dw100u-silver-adf-ramoops.dtb` in a new SPRD DT table with the stock header and entry
fields (board id `0x2668`, rev 1), on a copy of the stock DW100U silver boot image; in its ramdisk the 4.14 builds of sprdwl_ng.ko, sprdbt_tty.ko and sprd_fm.ko
(`devices/WCN_414_PORT.md`, debug info stripped) replace the 4.4.83 ones, and the stale 4.4.83 `mali.ko` with its `insmod` line is removed
(`tools/sprd_ramdisk.py remove-module`). Build script: `devices/build_414_stock81.sh`.


**Build default (since the DM63 bench overheating):** `devices/build_414_stock81.sh` now builds on the normal
`sp9832e-1h10-dw100u-silver-adf.dtb` with the stock `loglevel=1` and keeps `watch_414_ramoops.fragment` (pstore/ramoops on; the
stock DT's ramoops region is used, so oops/panic records still work, the console record only holds what the console
loglevel lets through). The images are named `dw100u-silver_414_adf_candidate*.img`. `LOGLEVEL7=1` selects the `-ramoops`
DTB (`loglevel=7`, bigger console zone, `devices/RAMOOPS_414.md`) for a short attended debug boot only: loglevel=7 writes every message synchronously to the
115200-baud serial console and contributed to the DM63 overheating. The images below are the earlier `loglevel=7` ones.

**Earlier test images (2026-11-11):** the same build on the `-ramoops` DTB (`loglevel=7`, crash log:
`devices/RAMOOPS_414.md`) and `watch_414_ramoops.fragment`, built by `devices/build_414_stock81.sh`
from the merge of `debug/ramoops-414` into `kernel-4.14` (`TOOLS` = a tree with `tools/ramdisk-benchdebug`):

| Image | sha256 |
|---|---|
| `dw100u-silver_414_adf_ramoops_candidate.img` | `869b74e3611cd27d2e64fd7ae8b19d1996c50c348ad976e1e0a5356a1057c0c9` |
| `dw100u-silver_414_adf_ramoops_candidate_BENCHDEBUG.img` | `e9fa6984bdf6300d53759eed4d9a88393e9a98b9b0c28b52eed212317c6b5f64` |

The BENCHDEBUG image adds `/adb_keys` and `init.benchdebug.rc` to the ramdisk: early authenticated adb for the
bench host, nothing else changed (`devices/BENCH_DEBUG.md`). **Bench watches only: reflash the normal image
before the watch leaves the bench.**

What this build adds over the first candidates (all reviewed, compile-tested only):
- the stock `android,fstab` node in the DT (`sp9832e-watch-firmware.dtsi`): without it 8.1 init mounts neither `/system` nor `/vendor`;
- `watch_414_stock81.fragment`: `xt_qtaguid` (xt_owner off), the in-kernel low memory killer with the OOM notifier, `PM_AUTOSLEEP`,
  PID namespaces, `PM_WAKELOCKS_LIMIT=0`, the `timed_output` vibrator (`VIBRATOR_414_PORT.md`) and the GSP blender (`GSP_414_PORT.md`);
- fixes in code that build enables: qtaguid socket lifetimes, low memory killer vmstat counters and OOM locking;
- ADF software fences with a `.wait` callback (the first post from the hwcomposer would otherwise hit a `BUG_ON`);
- Mali r21p0: fence dependency counted before the callback is added.

Differences from the K18 notes:
- GPU: the r21p0 Mali driver is built in (`sp9832e-watch-gpu.dtsi`, stock DVFS points with vddcore scaling). The
  stale stock `mali.ko` and its `insmod` line are removed from the ramdisk. Whether the stock userspace blob runs on it is what this boot can show (`/dev/mali0`, SurfaceFlinger).
- Panels: all stock /lcds nodes (23, the silver unit reports ID5300204). Check `found lcd... panel success!` in `dmesg` for the unit's `lcd_id`.
- AVB footer kept with a stale `boot` hash: unlocked (orange) devices only.

- Wi-Fi, Bluetooth and FM: the three modules load from the stock insmod lines; deep sleep uses the stock
  `cluster_pd` idle state (`sp9832e-watch-idle.dtsi`); AOD is built in (`/sys/kernel/aod_idle`).
- Camera: the 4.4 camera driver ported for the stock HAL (`devices/CAMERA_414_PORT.md`), main sensor as in
  stock. Check that `/dev/sprd_image`, `sprd_isp`, `sprd_cpp` and `sprd_sensor` exist and that the camera app opens.
- Fingerprint: the Goodix interface driver for the stock fingerprint HAL (`drivers/input/fingerprint/goodix`, node on
  spi2). Check `/dev/goodix_fp` and enrolment in Settings.

- GSP: `/dev/gsp` for the stock hwcomposer; check `dmesg` for `cfg does not fit its buffers` (HAL layout assumptions)
  and whether a GSP hang recovers (AHB soft reset, never used on sharkle before).
- Vibrator: `/sys/class/timed_output/vibrator/enable` (milliseconds) for the stock vibrator HAL.
- Networking: `/proc/net/xt_qtaguid/` present (per-app data usage); low memory killer parameters in
  `/sys/module/lowmemorykiller/parameters/`.

Charging under 4.14 must be attended. If the watch does not boot, reflash the stock `boot` partition from the backup.
