# Vendor patch for a GSI (system-as-root)

With system-as-root the first-stage ramdisk only mounts `/system` and `/vendor`. The stock Android 8.1 ramdisk files (`init.sl8541e_1h10.rc`, `init.common.rc`, `init.ram.rc`, `init.storage.rc`, `fstab.sl8541e_1h10`, `ueventd.sl8541e_1h10.rc`, `/lib/modules`) are not there any more, so a GSI boots without the watch's hardware setup. The patch moves what is device specific into `/vendor`, where a GSI's init looks:

| File (per device, in `devices/<dm63\|dw100u-silver>/vendor_gsi/`) | Installed as | Read by |
|---|---|---|
| `etc/init/hw/init.sl8541e_1h10.rc` | `/vendor/etc/init/hw/init.sl8541e_1h10.rc` | the GSI `/system/etc/init/hw/init.rc`: `import /vendor/etc/init/hw/init.${ro.hardware}.rc` (`ro.hardware=sl8541e_1h10`) |
| `etc/fstab.sl8541e_1h10` | `/vendor/etc/fstab.sl8541e_1h10` | `mount_all` in the rc above |
| `ueventd.rc` | `/vendor/ueventd.rc` | ueventd of Android 10+ (`/system/etc/ueventd.rc`, then `/vendor/ueventd.rc`, then `/odm/ueventd.rc`) |

All files are `root:root 0644`. SELinux labels: `vendor_configs_file` for the two `etc` files; for `/vendor/ueventd.rc` and the modules the label is looked up at install time (see the script). Source of every file: the stock ramdisks in the untouched dumps (DM63 and DW100U-silver ramdisk; read only, the script and the files here never execute anything from them).

The two ramdisks differ only in `init.sl8541e_1h10.rc` and `init.common.rc` (the `ueventd*.rc`, `fstab`, `init.ram.rc`, `init.storage.rc`, the empty `init.sl8541e_1h10.usb.rc` and the four `.ko` names are the same; the `.ko` bytes differ), so the two directories differ in the rc only:

| | DM63 | DW100U-silver |
|---|---|---|
| `start ylog` in post-fs-data | commented out | active |
| `/sys/devices/platform/gpio-keys/hsc_disabled_keys` chown 0664 | no | yes |
| `chmod 666 /dev/gh3x2x /dev/och1970 /dev/hall` | no | yes |
| `/data/vendor_de/0/fpdata` mkdirs (0770 system) | no | yes |

(The AOSP `init.rc` differs too, DM63 copies `device_policies.xml`/`device_owner_2.xml` and sets `chmod 6755 /system/xbin/su`; it belongs to the stock system layer and is not carried.)

## What is not carried and why

| Stock file | Decision |
|---|---|
| `init.rc`, `init.environ.rc`, `init.usb.rc`, `init.usb.configfs.rc`, `init.zygote32.rc`, `init.zygote64_32.rc` | provided by the GSI (`/system/etc/init/hw/`) |
| `init.sl8541e_1h10.usb.rc` | 1 byte (empty) |
| `init.recovery.sl8541e_1h10.rc` | not imported at boot (recovery, `cali`, `factorytest` triggers only; gnss_download for factory calibration) |
| `ueventd.rc` | AOSP 8.1 generic file, identical on both watches, no device line; the GSI has its own. Only the `subsystem adf` and `subsystem graphics` blocks are copied (see below) |
| `sbin/ueventd`, `sbin/charger`, `sbin/watchdogd`, `init` | binaries of the stock ramdisk, the GSI brings its own (`charger` is a symlink to `/sbin/charger`: charger mode needs a GSI/vendor healthd, out of scope) |
| `lib/modules/*.ko` | see "Kernel modules" |

## Change list against stock

IDs match the header comment of the rc. "Common" is the `init.common.rc` part of the merged file.

| ID | Where (stock) | Stock | Patched | Why |
|---|---|---|---|---|
| C1 | `init.sl8541e_1h10.rc` line 1 | `import /init.common.rc` | content inlined (order: ram, storage, common, device) | one file, no `/init.*.rc` path left that only existed in the ramdisk. Inlining keeps the stock order (common actions before the device actions); to my knowledge an Android 9+ init parses imports after the importing file, which would reverse it with separate files |
| C1 | `init.common.rc` lines 1-3 | `import /init.${ro.hardware}.usb.rc`, `import init.ram.rc`, `import init.storage.rc` | removed; `init.ram.rc` and `init.storage.rc` inlined, the usb file is empty | same, and `/init.*.rc` does not exist in the GSI root |
| C2 | Common, `on early-init` | `mkdir /vendor/etc/seccomp_policy 0777 media system` | removed | `/vendor` is a read-only mount at this point, the mkdir cannot work; the policy dir belongs in the vendor image (stock had the same failure) |
| C3 | Common, `on fs` | `ubiattach 0 ubipac` | removed | raw-NAND (UBI) only; these watches are eMMC (`by-name` on `20600000.sdio`). Not every init version still knows the command |
| C4 | Common, `on fs` | `mount_all /fstab.${ro.hardware}` | `mount_all /vendor/etc/fstab.${ro.hardware}` | the ramdisk `/fstab.*` is gone. Plain `mount_all` (no `--early`/`--late`): all entries mount in `fs`, as in stock |
| C10 | Common, `on fs` | `# exec /sbin/resize2fs -ef /fstab.${ro.hardware}`, `#start path_wrapper` | removed | comments about things that do not exist in a GSI |
| C5 | Common, `on post-fs-data` | `mkdir /etc/wifi 0770 wifi wifi` | removed | `/etc` is `/system/etc`, read-only |
| C6 | Common, `on post-fs` | `insmod /lib/modules/mali.ko` | `#mali-module: insmod /vendor/lib/modules/mali.ko` (comment) | the ramdisk path is gone. See "Kernel modules" |
| C7 | device rc, `on post-fs-data` | `insmod /lib/modules/sprdbt_tty.ko`, `insmod /lib/modules/sprd_fm.ko` | same with `/vendor/lib/modules/` | module location |
| C7 | device rc, `on post-fs-data` | (none) | `insmod /vendor/lib/modules/sprdwl_ng.ko` | no rc loads `sprdwl_ng.ko` in the stock ramdisk although the ramdisk carries it; something in the stock userspace loads it by its `/lib/modules` path, which a GSI does not have. A second insmod by a stock HAL fails with EEXIST and is harmless |
| C8 | Common | `service path_wrapper /sbin/path_wrapper` (critical, seclabel `path_wrapper`) | removed | `/sbin/path_wrapper` is not in the ramdisk, it was never started (`#start path_wrapper`), and a GSI policy has no `path_wrapper` domain |
| C8 | Common | `service count /system/xbin/count` (class main, root, socket 0777) | removed | binary of the stock `/system`, not in a GSI; as `class main` it would only log an exec failure |
| C8 | Common | `dhcpcd_wlan0`, `dhcpcd_p2p`, `dhcpcd_ipv6`, `iprenew_wlan0`, `iprenew_p2p`, `dhcpcd_bnep0`, `iprenew_bnep0`, `dhcpcd_bt-pan`, `iprenew_bt-pan` | removed | `/system/bin/dhcpcd`: a GSI of Android 9+ has no dhcpcd (the framework runs its own DHCP client), all are `disabled`/`oneshot` |
| C9 | whole file | tabs, trailing blanks, 2+ blank lines | spaces, trimmed, one blank line | formatting only |
| (fstab) | `fstab.sl8541e_1h10` | header comment about the checker on `/system`, two commented `memdisk` lines | header replaced by the notes in the file | no `/system` or `/vendor` entry: the first-stage ramdisk mounts them. Every entry line is byte-identical to stock |
| (ueventd) | `ueventd.sl8541e_1h10.rc` | the file | same lines, same order, after two `subsystem` blocks | renamed to `/vendor/ueventd.rc`; the `subsystem adf` and `subsystem graphics` blocks come from the stock `ueventd.rc` as a safety net (the display is ADF: `/dev/adf*`, `/dev/graphics/fb0`; a newer AOSP `ueventd.rc` may drop them; if kept, the duplicate is harmless) |

Everything else in the rc is stock, line for line (about 780 lines): the sensor, GNSS, PMIC, modem, display, thermal, `reserve_space` and cpuhotplug permissions, the ZRAM/LMK/hwui property tables, the `mknod /dev/input c 5 0` line (fails with EEXIST once ueventd created `/dev/input`, as in stock), the `wpa_supplicant`/`p2p_supplicant`, `aprd`, `systemDebuggerd`, `srtd` and `gpsd` services.

### fstab

| Entry | Mount point | Flags | Note |
|---|---|---|---|
| `userdata` f2fs, then ext4 | `/data` | stock flags, `wait,encryptable=footer,check` | `encryptable=footer` is legacy full-disk encryption: a GSI keeps it only up to Android 10. If userdata carries a crypto footer from the stock system, a GSI that expects FBE or none cannot mount it without a wipe. For Android 11+ remove `encryptable=footer` (documented in the file header); not changed here, to keep the stock flags |
| `cache` ext4 | `/cache` | stock | |
| usb `auto` vfat, sdcard `auto` vfat | `auto` | stock `voldmanaged=...` | sdcard entry has `encryptable=footer` too (same note) |
| `prodnv` ext4 | `/productinfo` | stock | the mount point is not in the GSI image: fs_mgr creates it, which needs a writable root (the ramdisk `/`). If the root is the read-only system image the mount fails: then mount at `/mnt/vendor/productinfo` instead and change the `/productinfo` paths in the rc |
| `misc` emmc | `/misc` | `defaults defaults` | raw partition: fs_mgr skips it in `mount_all`, the line stays for recovery/uncrypt |

## Kernel modules

`init` of a GSI can only load modules from a mounted filesystem, and the stock ones live in the ramdisk (`/lib/modules`, in the boot image), which is not used any more. What loads each one now:

| Module | Used for | How it loads now |
|---|---|---|
| `mali.ko` | GPU (`/dev/mali0`) | built into the kernel on the 4.14 trees (nothing to load; leave the `#mali-module:` line commented; not checked in this 4.4 tree). The 4.4.302 tree of this branch still builds it out of tree (`scripts/watch_build.sh`): put it in `/vendor/lib/modules` and uncomment the line; `scripts/vendor_gsi_patch.sh --modules-dir` does both when `mali.ko` is in the dir |
| `sprdbt_tty.ko`, `sprd_fm.ko` | Bluetooth tty, FM | modules in every tree here: `insmod /vendor/lib/modules/...` from `post-fs-data` (the rc does it); the script installs them from `--modules-dir` |
| `sprdwl_ng.ko` | WLAN (`CONFIG_SPRDWL_NG=m`) | module: `insmod` line added, same location |

Build all of them against the exact kernel `Image` that boots (vermagic): `watch_build.sh` puts `Image` and the four `.ko` in `OUTDIR/DEVICE/`. The ramdisk `.ko` of the stock kernel do not load on another kernel. The script reads `vermagic=` from each file and prints it; the stock copies cannot be taken from the watch (they are in the boot image, not in a mounted filesystem). A module-less kernel (everything `=y`) needs no `--modules-dir`, and the three `insmod` lines then fail harmlessly: delete them.

SELinux on Android 10+ requires the module file label to be allowed for `init` (`module_load`); the vendor policy of these Android 8.1 builds is not that policy, check `dmesg`/`logcat` for `avc: denied { module_load }` on first boot (DM63 is permissive).

## scripts/vendor_gsi_patch.sh

```
scripts/vendor_gsi_patch.sh --device dm63|dw100u-silver            # dry run (default)
scripts/vendor_gsi_patch.sh --device dw100u-silver --apply [--backup-dir DIR] [--modules-dir DIR] [--serial SN]
```

- **Dry run is the default.** It reads only: root mode, `ro.hardware`, the mounted `/vendor` and its by-name partition, free space, the vendor `nonplat_file_contexts`, the current hash of each target file; then prints the plan with the label of each file. Exit 0, or 3 if there is a blocker (verity, no space). No backup, no write.
- **Root detection:** adbd already root, else the device's Magisk form `su -c CMD` (DW100U; approve the prompt on the watch), else the vendor `su 0 sh -c CMD` (DM63). The order of the probe follows `--device`; a wrong `ro.hardware` (not `sl8541e_1h10`) aborts.
- **`--apply` order:** preflight; **dump of the whole vendor partition** (`dd` of the `.../by-name/vendor` device through `adb exec-out`, to `DIR/vendor-<serial>-<time>.img`); size must equal `blockdev --getsize64`; sha256 of the file must equal the sha256 of a second, independent read of the partition; only then `*.sha256` is written. Any failure removes the partial file and exits before anything on the device is touched. Then remount `/vendor` rw, create `/vendor/etc/init/hw` (and `lib/modules`) if missing, install each file (`cat > file.new` through `adb exec-in`, `chown 0:0`, `chmod`, `chcon`, `mv` over the target; sha256, owner/mode and label are read back; an existing target is first copied to `<target>.pregsi`, once), `sync`, remount ro, print the rollback command.
- **Labels:** the label of each file is taken from the device's own `/vendor/etc/selinux/nonplat_file_contexts` (longest matching regex, later on a tie; a regex that does not match, has another file type than a regular file, or gives something that is not `u:object_r:*:s0` is ignored). Fallback when nothing matches: `vendor_configs_file` for the `etc` files; for `/vendor/ueventd.rc` and modules the label of `/vendor/build.prop`, respectively the parent directory. The chosen label is in the plan; check it with `ls -Z` after the install (the script does).
- **verity:** if `/vendor` is mounted from a `dm-*` device, dm-verity is on and the filesystem cannot be remounted rw. The script reports it as a blocker and stops; it never runs `adb disable-verity` (that rewrites the verity metadata of the other verified partitions too). Options: a vendor image without verity, or `disable-verity` run by hand, knowing it touches more than vendor. A plain read-only block device is handled (`blockdev --setrw` on the vendor device only).
- **Scope:** the only device block path it accepts matches `/dev/block/.../by-name/vendor` (`--vendor-blk` too) and is only read. Every write is a file under `/vendor` (the code refuses other paths); nothing goes to `/data` (no `adb push`), no other partition, no fastboot, no reboot. If the run fails while `/vendor` is rw, an exit trap remounts it ro.
- **Rollback (whole partition):** the script prints `adb -s SN exec-in '<su> dd of=<vendor by-name> bs=1048576' < backup.img` for the mode it detected. Run it while `/vendor` is still ro, reboot at once, then compare with `backup.img.sha256`. Per file: `<target>.pregsi` over `<target>`; files that were new have no `.pregsi` and are deleted.

## Tests

`scripts/tests/vendor_gsi_patch_test.sh`: a stub adb (shell function) over a fake device tree, nothing touches a device. It checks: dry run is the default and writes nothing; Magisk and vendor-su modes; files, modes, owners requested and labels (also from a `nonplat_file_contexts` rule); the dump equals the partition; a truncated dump and a mismatching re-read abort before any remount or write; verity is reported and nothing is written (dry run exit 3); wrong `ro.hardware`, a non-`by-name/vendor` block device, a full `/vendor`, an unknown device are refused; modules and the mali switch; `.pregsi` is kept once; other partitions and `/data` are byte-identical afterwards. `bash -n` and, if installed, `shellcheck` run first (not installed on the machine used; not run).

Not verified on hardware: nothing here was run against a watch. First-boot checks: `getprop ro.hardware`, `logcat -b all | grep -i init`, `dmesg | grep -i avc`, `/dev/mali0`, `/dev/adf0`, Wi-Fi/BT modules (`lsmod`).

## Open points

- Where `sprdwl_ng.ko` is loaded in the stock system (not an rc in the ramdisk; check `/system` and `/vendor` of a stock watch for the loader and its path).
- Which stock file sets `ro.ramsize` (the ZRAM/LMK/hwui tables key on it) and where the `zram` service (`start zram`) is defined: neither is in the ramdisk; if they were in the ramdisk `default.prop` (a link to `system/etc/prop.default`) a GSI has to provide them.
- `wpa_supplicant` and `p2p_supplicant` have no `interface` lines for the Android 10+ HIDL/AIDL supplicant HAL; a GSI's Wi-Fi needs them (vendor service declarations, not done here).
- `srtd` (`/vendor/xbin/srtd`) runs as root with a world-writable socket (`0777`), stock design; kept because it is vendor functionality. Review before shipping.
- The `/sys/devices/system/cpu/cpu* /cpufreq/interactive/*` lines in the stock `ueventd` have a space after `cpu*` and probably never match; left as stock.
