# DW100U silver boot image candidate, kernel 4.14 with ADF

**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** The image is not in the repo (images are never
committed); it is kept in `~/.cache/dw100u-silver-414-candidate/dw100u-silver_414_adf_candidate.img`. This is a bring-up image, built the same way as the K18 one: see
`../k18/BOOT_414.md` for the commands, the contents and what to expect, which apply here except as noted.

Built from `kernel-4.14` commit `f11b0b75d04b`, kernel release 4.14.336+, config `sprd_sharkle_defconfig` +
`watch_414.fragment` + `watch_414_adf.fragment`, DTB `sp9832e-1h10-dw100u-silver-adf.dtb` in a new SPRD DT table with the stock header and entry
fields (board id `0x2668`, rev 1), on a copy of the stock DW100U silver boot image; in its ramdisk the 4.14 builds of sprdwl_ng.ko, sprdbt_tty.ko and sprd_fm.ko
(`devices/WCN_414_PORT.md`, debug info stripped) replace the 4.4.83 ones.

sha256 `3ac239d6041a94a15b6501c5aa4f2847dce532791d59dd3ad01808e6fd903b45`

Differences from the K18 notes:
- GPU: the r21p0 Mali driver is built in (`sp9832e-watch-gpu.dtsi`, stock DVFS points with vddcore scaling). The
  stock ramdisk still `insmod`s its 4.4.83 `mali.ko`, which fails (wrong kernel) without affecting the built-in
  driver. Whether the stock userspace blob runs on it is what this boot can show (`/dev/mali0`, SurfaceFlinger).
- Panels: all stock /lcds nodes (23, the silver unit reports ID5300204). Check `found lcd... panel success!` in `dmesg` for the unit's `lcd_id`.
- AVB footer kept with a stale `boot` hash: unlocked (orange) devices only.

- Wi-Fi, Bluetooth and FM: the three modules load from the stock insmod lines; deep sleep uses the stock
  `cluster_pd` idle state (`sp9832e-watch-idle.dtsi`); AOD is built in (`/sys/kernel/aod_idle`).

Charging under 4.14 must be attended. If the watch does not boot, reflash the stock `boot` partition from the backup.
