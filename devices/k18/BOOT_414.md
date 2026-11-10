# K18 boot image candidate, kernel 4.14 with ADF

**UNTESTED ON HARDWARE. Do not flash without the owner's explicit OK.** The image is not in the repo (images are never
committed); it is kept in `~/.cache/k18-414-candidate/k18_414_adf_candidate.img`. This is a bring-up image: it is
expected to boot the kernel and Android's early stages, not to give a usable watch (see "What to expect").

Built from `kernel-4.14` commit `83b99cae55df`, kernel release 4.14.336+, config `sprd_sharkle_defconfig` +
`watch_414.fragment` + `watch_414_adf.fragment` (ADF display, `DRM_SPRD` off), DTB `sp9832e-1h10-k18-adf.dtb`.

sha256 `257ca63853447a94f514c1941ad964dd3a7e663bf581718568368478de0deb5d`

## Contents

- Kernel: 4.14 `Image` (16 551 944 B). Everything is built in, there are no modules.
- DT table: a new SPRD table holding `sp9832e-1h10-k18-adf.dtb`, with the stock header and entry fields (board id
  `0x2668`, rev 1), so the bootloader selects it the same way as the stock one.
- Ramdisk: the stock K18 ramdisk, unchanged.
- Cmdline: stock (`console=ttyS1,115200n8 buildvariant=user`); the bootloader adds `lcd_id=`, `lcd_base=` and the
  rest as for stock.
- AVB footer: kept; the `boot` hash descriptor is stale, so this only boots on an unlocked (orange) device, as the
  4.4 candidates.

## Commands

From the 4.4 tree (`android_kernel_sprd_sc9832e`, tools on `security-backports`), on a copy of the stock image:

```
# 4.14 tree
devices/merge_watch_414.sh O   # or merge_config.sh with both fragments as below
scripts/kconfig/merge_config.sh -m -O O O/.config arch/arm64/configs/watch_414.fragment \
    arch/arm64/configs/watch_414_adf.fragment && make O=O olddefconfig
make O=O ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image sprd/sp9832e-1h10-k18-adf.dtb

# 4.4 tree
python3 -I tools/sprd_bootimg.py unpack stock-copy/boot.img DIR
python3 -I tools/sprd_bootimg.py dt-table DIR/dt O/arch/arm64/boot/dts/sprd/sp9832e-1h10-k18-adf.dtb dt414
python3 -I tools/sprd_bootimg.py pack DIR k18_414_adf_candidate.img --kernel O/arch/arm64/boot/Image --dt dt414
python3 -I tools/sprd_bootimg.py info k18_414_adf_candidate.img
```

`dt-table` was checked by feeding the stock DTB back in: it reproduces the stock table byte for byte.

## What to expect

Not expected to work on this image:
- GPU: there is no Mali driver for 4.14 yet. The stock ramdisk `insmod`s its 4.4.83 `mali.ko`, `sprdwl_ng.ko`,
  `sprdbt_tty.ko` and `sprd_fm.ko`; all four fail to load (wrong kernel), which init logs and continues past.
  Without the GPU the stock SurfaceFlinger cannot start, so no Android UI and no boot animation are expected.
- Wi-Fi, Bluetooth, FM (same reason), deep sleep, camera.

What the first boot can show:
1. Serial console (ttyS1, 115200) or `adb`: does the kernel reach init, and does `adbd` come up over USB?
2. eMMC: are the partitions found and does `/data` mount?
3. Display: in `dmesg`, does `sprd-panel-if` find `lcd3311148` (`found lcd3311148 panel success!`), do the dispc,
   dsi and dphy probe, does `/dev/adf0` exist? Whether the panel shows anything depends on how far the stock
   userspace gets without the GPU.
4. Sensors and touch: probe messages for sc7a22, och1970, hrs3xxx and the touch controller; the touch wake and
   panel notifier events on screen on/off.
5. Thermal and watchdog: `/sys/class/thermal`, `/dev/watchdog0` present (the watchdog is not started by the kernel).

Charging under 4.14 must be attended (see `DTS_414.md` and `SC2721_CHARGER_MAP.md`): the charger and gauge have
only been compiled. If the watch does not boot, reflash the stock `boot` partition from the backup.
