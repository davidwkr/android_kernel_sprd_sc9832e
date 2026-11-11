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
| C11 | device rc | `service srtd /vendor/xbin/srtd` (class main, root, `socket srtserver` 0777) | removed | `/vendor/xbin/srtd` does not exist on either watch (checked on both: no binary, no socket), so it is dead stock configuration |
| C5 | Common, `on post-fs-data` | `mkdir /etc/wifi 0770 wifi wifi` | removed | `/etc` is `/system/etc`, read-only |
| C6 | Common, `on post-fs` | `insmod /lib/modules/mali.ko` | `#mali-module: insmod /vendor/lib/modules/mali.ko` (comment) | the ramdisk path is gone. See "Kernel modules" |
| C7 | device rc, `on post-fs-data` | `insmod /lib/modules/sprdbt_tty.ko`, `insmod /lib/modules/sprd_fm.ko` | same with `/vendor/lib/modules/` | module location |
| C7 | device rc, `on post-fs-data` | (none) | `insmod /vendor/lib/modules/sprdwl_ng.ko` | no rc loads `sprdwl_ng.ko` in the stock ramdisk although the ramdisk carries it; the Wi-Fi HAL loads it (see "Kernel modules"). Pre-loading it from the rc is fine: the HAL checks `/proc/modules` first |
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
| `prodnv` ext4 | `/productinfo` | stock | kept at `/productinfo`: the stock vendor depends on the path (`/vendor/etc/init/charge.rc`, the `/productinfo` paths in the rc: WCN `wcn`, `sensor_calibration_data`). The GSI root is the read-only system image and has no such directory, so the mount would fail; the directory is added to the image instead, see "Root directories a GSI lacks" |
| `misc` emmc | `/misc` | `defaults defaults` | raw partition: fs_mgr skips it in `mount_all`, the line stays for recovery/uncrypt |

## Root directories a GSI lacks

A GSI is system-as-root: `/` is the read-only system image. Everything the merged rc files and the fstab name at the root of the filesystem has to exist there or be created by init in a tmpfs. Absolute root-level paths used (both rc files and the fstab): `/sys /proc /dev /data /cache /storage /mnt /system /vendor /productinfo /res /bin /misc /devices`, compared with the root of the downloaded GSI (`acct apex bin bt_firmware bugreports cache config d data data_mirror debug_ramdisk dev efs etc init init.environ.rc linkerconfig metadata mnt odm odm_dlkm oem persist postinstall proc product sbin sdcard sec_storage second_stage_resources storage sys system system_dlkm system_ext vendor vendor_dlkm firmware`):

| Path | In the GSI root | Used for | Handling |
|---|---|---|---|
| `/productinfo` | **missing** | `prodnv` mount point; `chown`/`chmod`/`mkdir` in the rc, WCN and sensor calibration, stock `/vendor/etc/init/charge.rc` | `scripts/gsi_add_mountpoints.sh` adds it (below) |
| `/res` | missing | `symlink /system/res /res` in the rc (`on fs`) | not a mount point or a data directory: the rc line fails on the read-only root (init logs it, the boot goes on). Nothing in the vendor files reads `/res` (it served the stock charger images, and charger mode is out of scope). Not added |
| `/bin` | present | `symlink /system/bin /bin` in the rc | the symlink fails with EEXIST, harmless |
| `/misc`, `/devices` | n/a | the `misc` fstab entry (raw `emmc`, skipped by `mount_all`) and the sdcard/usb `/devices/...` sysfs patterns | not paths in the root |
| `/data /cache /storage /mnt /dev /proc /sys /system /vendor` | present | mount points and tmpfs | none |

Also in the stock ramdisk root: `/lib` (the `.ko` files, see "Kernel modules": they go to `/vendor/lib/modules`), `/sbin` (present in the GSI), `/oem` (present), `/charger`, `/default.prop`, `/etc`, `/sdcard`, `/bugreports` (present in the GSI as symlinks or directories). The stock `/vendor` rc files could not be checked here (no dump of the stock vendor partition in this tree): of them only `/vendor/etc/init/charge.rc` is known to use `/productinfo`. To check on a stock watch: `adb shell 'grep -rhoE "(^|[ =:\"])/[a-z_]+" /vendor/etc/init | sort -u'` (read-only), and compare with the list above.

### `scripts/gsi_add_mountpoints.sh`

```
scripts/gsi_add_mountpoints.sh --dir /NAME --label u:object_r:TYPE:s0 [--dir /NAME --label ...] --output OUT.img IN.img
```

Two use cases:

| Directory | GSIs | Why |
|---|---|---|
| `/productinfo` | every GSI | `prodnv` mount point, see above |
| `/second_stage_resources` | Android 10 and 11 GSIs only | the Android 14 first-stage init of our ramdisk mounts a tmpfs at `/second_stage_resources` and, in `SwitchRoot("/system")`, creates that directory under the new root and moves the mount there; a failure is fatal. It is in the GSI root from Android 12; Android 10 and 11 GSIs lack it and `system` is read-only, so it is added with this script |

Examples (labels are placeholders, see below):

```
scripts/gsi_add_mountpoints.sh --dir /productinfo --label u:object_r:PRODUCTINFO_TYPE:s0 --output OUT.img IN.img
scripts/gsi_add_mountpoints.sh --dir /productinfo --label u:object_r:PRODUCTINFO_TYPE:s0 \
  --dir /second_stage_resources --label u:object_r:ROOT_TYPE:s0 --output OUT.img IN.img
```

`--dir` and `--label` are repeatable; a `--label` applies to the preceding `--dir` and each `--dir` needs exactly one. `NAME` is a single root-level component (`^/[A-Za-z0-9_.-]+$`, not `/`, `.`, `..`), no duplicates. The script refuses, before writing anything, if any of the directories already exists in the image (an Android 12+ GSI already has `/second_stage_resources`). Every directory is root:root 0755 and is verified afterwards.

Works on a copy: `IN.img` is only read, `OUT.img` must not exist (it is removed again on any failure). `IN.img` has to be a raw ext4 image: an Android sparse image (magic `3aff26ed`) is refused, convert it first with `simg2img system.img system.raw.img`. Needs `debugfs` and `e2fsck` (e2fsprogs). An image with `shared_blocks` (deduplicated, read-only) cannot be opened read-write by debugfs and the script stops with that message; so does an image without a free block or inode (each directory takes one inode and one block). The real Android 14 GSI was checked read-only: raw ext4 (no sparse magic), features without `shared_blocks`, 479 free blocks and 555 free inodes, so the `mkdir` fits.

On the copy it runs `debugfs -w`: per directory `mkdir`, `set_inode_field mode 040755`, `uid 0`, `gid 0`, `ea_set security.selinux` (label plus the terminating NUL, as the Android labelling code stores it). Mode and owner are what fs_mgr made in the stock ramdisk root (`mkdir(mount_point, 0755)`, root:root; the stock `init.*.rc` and `fstab` have no other chmod/chown for the mount point). The `chown system system /productinfo` / `chmod 0771` in the rc apply to the mounted `prodnv` root, not to the mount point. Then it verifies with `debugfs stat`, `ea_get` and `ls` (directory, 0755, 0:0, label, empty, for each directory) and `e2fsck -fn` (must exit 0).

`--label` is required, not guessed. For `/second_stage_resources` it is only a mount point covered by a tmpfs: use the GSI's own label for an unlisted root entry, from the GSI's `/system/etc/selinux/plat_file_contexts`: the rule that matches `/` or a root entry such as `/debug_ramdisk`. Read it without mounting: `debugfs -R 'cat /system/etc/selinux/plat_file_contexts' IN.img | grep -E 'debug_ramdisk|^/ '` and take the `u:object_r:TYPE:s0` of that rule. For `/productinfo`, take it from the watch's own vendor file contexts, so the mount point has the label the vendor policy expects for `/productinfo`: `adb shell grep productinfo /vendor/etc/selinux/nonplat_file_contexts` (`/vendor/etc/selinux/plat_file_contexts` is the other file of the stock policy), and use the context of the `/productinfo` entry, `u:object_r:TYPE:s0`. If the entry is `/productinfo(/.*)?`, the directory itself matches it. The GSI image is an input and is not committed. Flash `OUT.img` as `system` (the script does not touch a device). Test: `scripts/tests/gsi_add_mountpoints_test.sh` (synthetic ext4 image: directory, mode, owner, xattr, input untouched, refusals).

## Kernel modules

`init` of a GSI can only load modules from a mounted filesystem, and the stock ones live in the ramdisk (`/lib/modules`, in the boot image), which is not used any more. What loads each one now:

| Module | Used for | How it loads now |
|---|---|---|
| `mali.ko` | GPU (`/dev/mali0`) | 4.14 route: built into the kernel (`CONFIG_MALI_MIDGARD=y`), nothing to load, the `#mali-module:` line stays commented. 4.4 route (the 4.4.302 tree of this branch, `scripts/watch_build.sh`): `mali.ko` is a module, it must go to `/vendor/lib/modules` and the line must be uncommented; `scripts/vendor_gsi_patch.sh --modules-dir` does both when `mali.ko` is in the dir |
| `sprdbt_tty.ko`, `sprd_fm.ko` | Bluetooth tty, FM | modules in every tree here: `insmod /vendor/lib/modules/...` from `post-fs-data` (the rc does it); the script installs them from `--modules-dir` |
| `sprdwl_ng.ko` | WLAN (`CONFIG_SPRDWL_NG=m`) | module, loaded by the Wi-Fi HAL: `libwifi-hal` checks `/proc/modules` first and only then loads the module from the path compiled into the HAL (`WIFI_DRIVER_MODULE_PATH` of the vendor build, not checked on the watch; a stock Android 8.1 one is a `/lib/modules` path, which a GSI does not have). The `insmod` from the rc runs first, so the HAL finds it loaded and skips its own load; if the HAL tries its own path, make sure the module is also there |

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

- Which path the Wi-Fi HAL (`libwifi-hal`, `WIFI_DRIVER_MODULE_PATH`) of the stock vendor uses for `sprdwl_ng.ko` (not checked on a watch).
- Which stock file sets `ro.ramsize` (the ZRAM/LMK/hwui tables key on it) and where the `zram` service (`start zram`) is defined: neither is in the ramdisk; if they were in the ramdisk `default.prop` (a link to `system/etc/prop.default`) a GSI has to provide them.
- `wpa_supplicant` and `p2p_supplicant` have no `interface` lines for the Android 10+ HIDL/AIDL supplicant HAL; a GSI's Wi-Fi needs them (vendor service declarations, not done here).
- The `/sys/devices/system/cpu/cpu* /cpufreq/interactive/*` lines in the stock `ueventd` have a space after `cpu*` and probably never match; left as stock.
