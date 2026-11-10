# K18 boot image candidate

## Current candidate: with the r21p0 Mali driver

`~/.cache/k18-candidate/k18_mali_candidate.img`, sha256
`5d4097b3951007e2fb97e738a20c966835252ad6fac7088ddeef90d873dcbeea`. **UNTESTED ON HARDWARE; do not flash without
the owner's explicit OK.** Built from branch `devices/k18-mali-candidate` (`security-backports` merged with
`drivers/mali-r21p0-cve2`), with the same commands as below. The difference from the older candidate:
`mali.ko` is the in-tree r21p0 kbase (`r21p0-01rel0 (UK version 11.0)`, the version the stock
`libGLES_mali.so` checks for; see `../MALI_R21P0.md`), not the r28p0 external build, which the stock GPU
library would refuse. `scripts/watch_build.sh` now picks the in-tree r21p0 module for boards whose defconfig
selects it (K18, DM63). Kernel 24 239 632 B, ramdisk 1 442 045 B, stock DT table, stale AVB hash (unlocked
devices only), as before.

## Older candidate (r28p0 mali.ko)


**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** The image is not in the repo (images are never committed); it is kept in `~/.cache/k18-candidate/k18_candidate.img`. Flash prerequisites: `../../TODO.md` section 7 and `../README.md` ("Repacking the boot image").

Built from tree commit `b73d4148c5ec` (branch `devices/k18-bootimg`), kernel release 4.4.302+.

sha256 `e48ceb6174bea5621548ccba596796be1867621febed1cd92c97964d65755c52`

## Contents

- Kernel: this tree's `k18_defconfig` build (`Image`, 24 171 424 B).
- Ramdisk: stock K18 ramdisk with the four modules replaced by the new build (vermagic and `Module.symvers` check passed) and `/init.watch_tweaks.rc` added (`add-rc`, same as the other watches).
- DT table: the **stock** one (same as the other watches' first-boot candidates; no `--dt`). `sp9832e-1h10-k18.dts` is not used in this image.
- AVB footer: kept, as for the other watches. The `boot` hash descriptor is stale (expected for a modified image), so this is only valid on an unlocked (orange) device.

## Commands

```
scripts/watch_build.sh k18 OUT
python3 -I tools/sprd_bootimg.py unpack stock-copy/boot.img DIR
python3 -I tools/sprd_ramdisk.py replace-modules DIR/ramdisk rd1.gz --kernel OUT/k18/Image --symvers OUT/obj-k18/Module.symvers --module OUT/k18/mali.ko --module OUT/k18/sprdwl_ng.ko --module OUT/k18/sprdbt_tty.ko --module OUT/k18/sprd_fm.ko
python3 -I tools/sprd_ramdisk.py add-rc rd1.gz rd2.gz --fragment devices/common/init.watch_tweaks.rc
python3 -I tools/sprd_bootimg.py pack DIR k18_candidate.img --kernel OUT/k18/Image --ramdisk rd2.gz
python3 -I tools/sprd_bootimg.py info k18_candidate.img
```

Run the tools on a copy of the stock image, never on the original. No `--partition-size` was given: the image assumes the stock `boot` size (36 700 160 B). For a `recovery` test pass the real size.

## Modules

The stock ramdisk modules (`mali.ko`, `sprd_fm.ko`, `sprdbt_tty.ko`, `sprdwl_ng.ko`) all have vermagic `4.4.83 SMP preempt mod_unload modversions aarch64`. All four must be rebuilt for the new kernel (release 4.4.302+); `watch_build.sh` does so and `replace-modules` confirmed the new vermagic matches.

## Comparison with stock

| Field | Stock | Candidate |
|---|---|---|
| File size | 36 700 160 | 36 700 160 |
| Page size | 2048 | 2048 |
| Kernel size | 18 020 512 | 24 171 424 |
| Kernel image_size / text_offset | 20 066 304 / 0x80000 | 26 177 536 / 0x80000 |
| Kernel addr | 0x00008000 | 0x00008000 |
| Ramdisk size | 1 374 736 | 1 523 853 |
| Ramdisk addr | 0x05400000 | 0x05400000 |
| Second size / addr | 0 / 0x00f00000 | 0 / 0x00f00000 |
| Tags addr | 0x100 | 0x100 |
| DT table | 137 216 B, 1 DTB, id 0x2668 | identical |
| os_version | 0x1004013b (8.1.0, 2019-11) | same |
| Cmdline | `console=ttyS1,115200n8 buildvariant=user` | same |
| Header id check | OK | OK |
| AVB footer | hash ok, vbmeta at 0x12a2000 | hash stale, vbmeta at 0x18a4000 |

Kernel end (0x8000 + 0x80000 + 26 177 536) is below the ramdisk address 0x05400000; `pack` accepted it.

Stock gzip stream is not reproducible by the tool, so the new ramdisk is recompressed (zlib level 9); the cpio content differs only in the four modules, `/init.rc` and the added rc file.
