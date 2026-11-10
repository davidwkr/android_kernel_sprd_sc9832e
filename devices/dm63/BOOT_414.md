# DM63 boot image candidate, kernel 4.14 with ADF

**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** The image is not in the repo (images are never
committed); it is kept in `~/.cache/dm63-414-candidate/dm63_414_adf_candidate.img`. This is a bring-up image, built the same way as the K18 one: see
`../k18/BOOT_414.md` for the commands, the contents and what to expect, which apply here except as noted.

Built from `kernel-4.14` commit `5c8381fd2883`, kernel release 4.14.336+, config `sprd_sharkle_defconfig` +
`watch_414.fragment` + `watch_414_adf.fragment`, DTB `sp9832e-1h10-dm63-adf.dtb` in a new SPRD DT table with the stock header and entry
fields (board id `0x2668`, rev 1), on a copy of the stock DM63 boot image with its ramdisk unchanged.

sha256 `62bd8042422a87b8791a361c7bf395a3d881225d123bf810b05187fcaaa5d78a`

Differences from the K18 notes:
- GPU: the r21p0 Mali driver is built in (`sp9832e-watch-gpu.dtsi`, stock DVFS points with vddcore scaling). The
  stock ramdisk still `insmod`s its 4.4.83 `mali.ko`, which fails (wrong kernel) without affecting the built-in
  driver. Whether the stock userspace blob runs on it is what this boot can show (`/dev/mali0`, SurfaceFlinger).
- Panels: all stock /lcds nodes (14, the unit reports ID5300213). Check `found lcd... panel success!` in `dmesg` for the unit's `lcd_id`.
- AVB footer kept with a stale `boot` hash: unlocked (orange) devices only.

Charging under 4.14 must be attended. If the watch does not boot, reflash the stock `boot` partition from the backup.
