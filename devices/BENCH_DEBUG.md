# Bench debug aids for the DM63 and DW100U silver test images

**BENCHDEBUG images are for bench watches only. Reflash the normal image before a watch leaves the bench.**
**They start adb from the ramdisk regardless of an MDM policy that disables adb: use them only on watches released
from MDM or with the MDM administrator's approval.**
They accept adb from one host key without any prompt, with no `/data` needed. Nothing here is flashed or tested
yet: **UNTESTED ON HARDWARE.**

Two aids, in different images:

| Aid | Normal test image | `*_BENCHDEBUG.img` |
|---|---|---|
| `loglevel=7` in the DT bootargs (instead of `loglevel=1`) | only with `LOGLEVEL7=1` | only with `LOGLEVEL7=1` |
| Crash log kept in RAM (4.14 only: `watch_414_ramoops.fragment`, on by default; the stock 4.4 has it already) | yes | yes |
| `/adb_keys` and `init.benchdebug.rc` in the ramdisk: early authenticated adb | no | yes |

## 1. loglevel=7

The stock DT has `loglevel=1`, which keeps almost every message off the consoles (UART, pstore). The test images
carried `loglevel=7` until the DM63 overheated on it: every kernel message goes synchronously to the 115200-baud serial
console and kept all CPUs busy. The build scripts now keep the stock `loglevel=1` and set `loglevel=7` only with `LOGLEVEL7=1`
(`build_candidates_44.sh`; on 4.14 `build_414_stock81.sh` then uses the `-ramoops` DTBs, otherwise the normal ADF DTBs with
the pstore fragment). Use it for a short attended boot only. When set, the rest of the bootargs and the DT are unchanged. On the 4.4 images the stock DT is edited
by `tools/sprd_bootimg.py dt-bootargs` (a proper FDT edit with `fdtput`, refused unless the string occurs exactly
once, layout, ids and padding kept); on 4.14 the `-ramoops` DTBs set it (`devices/RAMOOPS_414.md`). The
bootloader may still override the bootargs: check `/proc/cmdline` of a booting kernel.

## 2. Early authenticated adb (BENCHDEBUG images)

What is added to a copy of the stock ramdisk (`tools/sprd_ramdisk.py add-file` / `add-rc --name`):

- `/adb_keys`, mode 0644 root:root: the bench host's adb **public** key (`~/.android/adbkey.pub`). adbd
  loads authorized keys from `/adb_keys` and `/data/misc/adb/adb_keys` [I: from memory of the AOSP O-MR1 adbd;
  the source was not reachable when this was written. The stock ramdisk's `init.rc` has `restorecon /adb_keys`,
  which fits]. It is an **additional** authorized key: hosts already in `/data/misc/adb/adb_keys` stay
  authorized once `/data` is mounted. The file is checked by `tools/sprd_adbkey_check.py` (name `adbkey.pub`,
  a regular file, not a symlink; one `<base64> comment` line whose strictly decoded base64 is a valid 524-byte
  Android RSAPublicKey: 64 words, `n0inv` and `rr` consistent with the 2048-bit modulus, exponent 3 or 65537)
  and its content is never printed or committed.
- `/init.benchdebug.rc` (imported from `/init.rc` like `init.watch_tweaks.rc`): starts adbd **before `/data`
  is mounted**. Stock order (`init.rc` `on late-init`): `early-fs`, `fs` (`init.common.rc` mounts the fstab,
  userdata included), `post-fs`, `late-fs`, `post-fs-data`, `zygote-start`, `load_persist_props_action`,
  `early-boot`, `boot`. These are queued events, so all `on late-init` commands, the bench rc's included, run
  before the first of them. /system and /vendor come from the first-stage early mount, so adbd and
  `/vendor/default.prop` are available there. The order in the image:
  1. `on init` (stock `init.usb.rc`): mounts configfs, makes gadget `g1` with ids and strings, config `b.1`,
     the `ffs.adb` function and the functionfs mount on `/dev/usb-ffs/adb`; sets `sys.usb.mode=normal`.
  2. `on late-init` (bench rc): `start adbd`, `wait /dev/usb-ffs/adb/ep1 10` (adbd writes the FunctionFS
     descriptors, which creates the endpoint files), then the body of the stock `init.usb.configfs.rc` rule for
     `sys.usb.config=adb` (ids `18d1:4EE7`, relink `ffs.adb` plus `gser.gs0-7` into `b.1`) and
     `write .../UDC ${sys.usb.controller}`. A property based start is not used because a property change is
     queued behind the `late-init` triggers, so even a `sys.usb.config=adb` set here would only act after `fs`.
  3. `fs` mounts `/data` (the stall that motivated this): adb is already up.
  4. `early-boot` (bench rc): `setprop sys.usb.config adb`. This alone does not win over
     `persist.sys.usb.config` (the vendor default is `ptp`): 8.1 init runs the initial property-trigger scan after
     the queued events, and the stock `on property:persist.sys.usb.config=*` (init.usb.rc) then copies `ptp` back.
  5. The bench rc has an action with the same trigger, parsed after init.usb.rc (it is imported at the end of
     `/init.rc`), so it runs last and sets `sys.usb.config adb` again: adb is the final state, also if the stored
     value changes later.
  5. `boot` (stock): `sys.usb.configfs=1`. With `config=adb`, `configfs=1` and `mode=normal` the stock rules
     fire again: `start adbd` (already running) and, as `sys.usb.ffs.ready=1`, the same relink and UDC write.
     In this kernel unlinking a function from a bound gadget unbinds the UDC first
     (`config_usb_cfg_unlink`), so the stock rule rebinds adb cleanly; it does not leave a ptp or empty gadget.
  `sys.usb.controller` is not set by the bench rc: the vendor `default.prop` has
  `sys.usb.controller=musb-hdrc.0.auto` and property defaults are loaded before the first action. If it were
  still empty at `late-init`, the UDC write fails and step 4/5 binds it once the property is there.
  The stock `init.sl8541e_1h10.usb.rc` is empty.
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
