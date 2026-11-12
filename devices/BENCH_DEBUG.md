# Bench debug aids for the DM63 and DW100U silver test images

**BENCHDEBUG images are for bench watches only. Reflash the normal image before a watch leaves the bench.**
They accept adb from one host key without any prompt, with no `/data` needed. Nothing here is flashed or tested
yet: **UNTESTED ON HARDWARE.**

Two aids, in different images:

| Aid | Normal test image | `*_BENCHDEBUG.img` |
|---|---|---|
| `loglevel=7` in the DT bootargs (instead of `loglevel=1`) | yes | yes |
| Crash log kept in RAM (4.14 only: `-ramoops` DTB, `watch_414_ramoops.fragment`; the stock 4.4 has it already) | yes | yes |
| `/adb_keys` and `init.benchdebug.rc` in the ramdisk: early authenticated adb | no | yes |

## 1. loglevel=7

The stock DT has `loglevel=1`, which keeps almost every message off the consoles (UART, pstore). The test images
carry `loglevel=7`; the rest of the bootargs and the DT are unchanged. On the 4.4 images the stock DT is edited
by `tools/sprd_bootimg.py dt-bootargs` (a proper FDT edit with `fdtput`, refused unless the string occurs exactly
once, layout, ids and padding kept); on 4.14 the `-ramoops` DTBs set it (`devices/RAMOOPS_414.md`). The
bootloader may still override the bootargs: check `/proc/cmdline` of a booting kernel.

## 2. Early authenticated adb (BENCHDEBUG images)

What is added to a copy of the stock ramdisk (`tools/sprd_ramdisk.py add-file` / `add-rc --name`):

- `/adb_keys`, mode 0644 root:root: the bench host's adb **public** key (`~/.android/adbkey.pub`). adbd
  loads authorized keys from `/adb_keys` and `/data/misc/adb/adb_keys` [I: from memory of the AOSP O-MR1 adbd;
  the source was not reachable when this was written. The stock ramdisk's `init.rc` has `restorecon /adb_keys`,
  which fits]. The file is checked by `tools/sprd_adbkey_check.py` (name `adbkey.pub`, one `QAAAA... comment`
  line) and its content is never printed or committed.
- `/init.benchdebug.rc` (imported from `/init.rc` like `init.watch_tweaks.rc`): on `early-boot` it sets
  `sys.usb.controller` and `sys.usb.config adb`. The gadget, the FunctionFS adb function and
  `sys.usb.mode=normal` come from the stock `init.usb.rc` (`on init`); `on boot` sets `sys.usb.configfs=1`,
  and the stock configfs rules then start adbd and bind the UDC. `early-boot` runs after
  `load_persist_props_action`, so the value also wins over a stored `persist.sys.usb.config`, and it does not
  wait for `/data`: init starts adbd without it. The stock `init.sl8541e_1h10.usb.rc` is empty.
  [I: `musb-hdrc.0.auto` as UDC name is inferred from the fstab path `20200000.usb/musb-hdrc.0.auto`; nothing
  in the ramdisk sets `sys.usb.controller`. If `adb devices` stays empty but the kernel log shows the gadget
  bound to nothing, read `/sys/class/udc/` of a running stock system and correct the rc.]
- Unchanged: `ro.adb.secure`, `ro.debuggable`, SELinux. adbd runs as `shell` and only accepts a host holding
  the matching private key. Anyone with that private key and USB access to the watch has a shell, so keep the
  key on the bench host only. Once the Android framework is up, the USB settings may change `sys.usb.config`
  (e.g. to `mtp`) and drop adb until the next reboot.

Use from the bench host:

    adb kill-server; adb start-server
    adb wait-for-device; adb devices          # the serial, state "device" (no authorization prompt)
    adb shell dmesg | tail -n 100
    adb shell logcat -d -b all | tail -n 200

## 3. Reading /sys/fs/pstore after a failed boot

The stock DT reserves a ramoops region (`0x8845a800`, 256 KiB: record, console and pmsg zones of 32 KiB;
`devices/RAMOOPS_414.md`), and the stock 4.4 kernel has the pstore options on, so both kernels read and write
it. After a hang or panic, reset warm (watchdog or the key combo; a power cycle may empty RAM), boot an image
with working adb and:

    adb shell 'ls /sys/fs/pstore'
    adb shell 'cat /sys/fs/pstore/console-ramoops*'   # the tail of the failed boot's console
    adb shell 'cat /sys/fs/pstore/dmesg-ramoops-*'    # oops/panic records, if any
    adb pull /sys/fs/pstore ./pstore-copy             # copy before deleting anything

The console record is the reliable one (written as messages are printed); with `loglevel=7` it holds the full
boot tail. Whether the bootloader keeps the region across the reset is not proven; an empty pstore after the
first failed boot means it did not. Pulled logs can hold kernel addresses: do not paste them into shared
trackers unreviewed.

## Building

    SFD=<dir with dm63/boot.img and dw100u-silver/boot.bin> ADB_PUBKEY=$HOME/.android/adbkey.pub BENCH_DEBUG=1 \
        <the branch's build script>

`BENCH_DEBUG=1` is required for the BENCHDEBUG images and makes the script refuse a missing, unreadable or
malformed `ADB_PUBKEY` (or one not named `adbkey.pub`). Without it only the normal images are written. Images
are never committed.
