# DM63 and DW100U 4.4 candidate boot images

**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** Images are never committed; they are
built by `devices/build_candidates_44.sh` into `~/.cache/{dm63,dw100u-silver}-candidate/`.

## Current images (2026-11-11)

Built from `devices/k18-mali-candidate` commit `885eb904b7c9` (v5: Mali evict_node, ADF stock layer records, SC7A22 interrupt storm, GPU cap/thermal/boost, zram/lz4, stock loglevel=1) (kernel content unchanged from the previous
table; the Mali refusal logging and the sprdwcn `strncpy` bound are in). The DT now has `loglevel=7` instead of
`loglevel=1`; the `_BENCHDEBUG` images also carry early authenticated adb (`BENCH_DEBUG.md`: **bench watches
only, reflash the normal image before a watch leaves the bench**).

| Watch | Image | sha256 |
|---|---|---|
| DM63 | `dm63_v5.img` | `e385d0428eec7caff39a35e16709ee918a327499eb468b84d75975b76048f973` |
| DM63 | `dm63_v5_BENCHDEBUG.img` | `ceaf7298573116b5134e7c501fcf94cb511819f9534860a263bfe621e7e6cb42` |
| DW100U silver | `dw100u-silver_v5.img` | `c88b38f1ba92b0e9f9afcdbefa713359f873ca112b1fdc698da07b97496f5e01` |
| DW100U silver | `dw100u-silver_v5_BENCHDEBUG.img` | `155d258185e20caf497e7e7d5462b9d985a0364c4f963f4a0c3b3d4262455407` |

Build: `SFD=<stock images dir> devices/build_candidates_44.sh`; the BENCHDEBUG images need `BENCH_DEBUG=1` and
`ADB_PUBKEY=<path>/adbkey.pub` too.

## What they contain

- Kernel 4.4.302 plus CIP and the audited Android 4.4-p picks, the security fixes of the vulnerability
  writeup (Mali r21p0 kbase, camera ioctls and lifetimes, ION, Wi-Fi/BT/FM clients, SIPC, sysdump/cp_dump,
  ADF debug sysfs, RNDIS), built with `scripts/watch_build.sh` (stock parity + hardening + device fragments).
- 2026-11-11 additions: `xt_qtaguid` unlock-before-put, low memory killer OOM locking / copied victim name /
  init skipped (`fixes/stock-driver-bugs`); AF6837 compass on the DM63, OCH1970 hall sensor on the DW100U
  (`dm63_sensors`, `dw100u_sensors` fragments); Mali r21p0 fence fixes (dormant: the fence code is not built on
  4.4); Mali r21p0 CVE-2023-4272 (whole-page user-buffer DMA), CVE-2023-26083 (timeline stream needs root),
  `MEM_COMMIT` refused on JIT regions, JIT failure-path rollback (`fixes/mali-r21p0-jit-44`); Mali refusal logging (`dmesg | grep 'mali: refused'` on first boot) and the sprdwcn `strncpy` bound.
- On a copy of each stock boot image: the ramdisk modules `mali.ko`, `sprdwl_ng.ko`, `sprdbt_tty.ko` and
  `sprd_fm.ko` replaced by this build's (vermagic and `module_layout` CRC checked by `tools/sprd_ramdisk.py`),
  `init.watch_tweaks.rc` added (`devices/common/`). The stock DT has only `loglevel=1` changed to `loglevel=7`
  (`tools/sprd_bootimg.py dt-bootargs`); the rest of the ramdisk is unchanged (BENCHDEBUG: plus `/adb_keys` and
  `init.benchdebug.rc`).
- AVB footer kept with a stale `boot` hash: unlocked (orange) devices only.

## First boot checks

1. Boots to the watch face; `uname -r` shows the 4.4.302 build; `dmesg` free of oopses.
2. GPU: the stock GLES blob runs on the r21p0 `mali.ko` (SurfaceFlinger up, UI animates).
3. Wi-Fi, Bluetooth, GPS fix (the A-GPS HAL patches are unchanged: same vendor).
4. Sensors: heart rate (HRS3xxx), accelerometer (SC7A22), plus the DM63 compass and the DW100U hall sensor
   (`/sys/class/input/*/name`).
5. Camera opens; vibrator works; charging attended on the first charge.
6. `cat /proc/net/xt_qtaguid/stats` works (data usage), `/sys/module/lowmemorykiller/parameters/` present.

If the watch does not boot, reflash the stock `boot` partition from the backup.
