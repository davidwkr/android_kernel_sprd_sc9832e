# DM63 boot image candidate, kernel 4.14 with ADF

**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** The image is not in the repo (images are never
committed); it is kept in `~/.cache/dm63-414-candidate/dm63_414_adf_candidate.img`. This is a bring-up image, built the same way as the K18 one: see
`../k18/BOOT_414.md` for the commands, the contents and what to expect, which apply here except as noted.

Built from `kernel-4.14` commit `ed4285470c9a` (2026-11-11), kernel release 4.14.336+, config `sprd_sharkle_defconfig` +
`watch_414.fragment` + `watch_414_adf.fragment` + `watch_414_stock81.fragment`, DTB `sp9832e-1h10-dm63-adf.dtb` in a new SPRD DT table with the stock header and entry
fields (board id `0x2668`, rev 1), on a copy of the stock DM63 boot image; in its ramdisk the 4.14 builds of sprdwl_ng.ko, sprdbt_tty.ko and sprd_fm.ko
(`devices/WCN_414_PORT.md`, debug info stripped) replace the 4.4.83 ones, and the stale 4.4.83 `mali.ko` with its `insmod` line is removed
(`tools/sprd_ramdisk.py remove-module`). Build script: `devices/build_414_stock81.sh`.

sha256 `5d49dfa56d8891f7d10dc7679a797617a694766f5c11db6a5a096a75b13adcda`

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
- Panels: all stock /lcds nodes (14, the unit reports ID5300213). Check `found lcd... panel success!` in `dmesg` for the unit's `lcd_id`.
- AVB footer kept with a stale `boot` hash: unlocked (orange) devices only.

- Wi-Fi, Bluetooth and FM: the three modules load from the stock insmod lines; deep sleep uses the stock
  `cluster_pd` idle state (`sp9832e-watch-idle.dtsi`); AOD is built in (`/sys/kernel/aod_idle`).
- Camera: the 4.4 camera driver ported for the stock HAL (`devices/CAMERA_414_PORT.md`), main and sub sensor as in
  stock. Check that `/dev/sprd_image`, `sprd_isp`, `sprd_cpp` and `sprd_sensor` exist and that the camera app opens.

- GSP: `/dev/gsp` for the stock hwcomposer; check `dmesg` for `cfg does not fit its buffers` (HAL layout assumptions)
  and whether a GSP hang recovers (AHB soft reset, never used on sharkle before).
- Vibrator: `/sys/class/timed_output/vibrator/enable` (milliseconds) for the stock vibrator HAL.
- Networking: `/proc/net/xt_qtaguid/` present (per-app data usage); low memory killer parameters in
  `/sys/module/lowmemorykiller/parameters/`.

Charging under 4.14 must be attended. If the watch does not boot, reflash the stock `boot` partition from the backup.
