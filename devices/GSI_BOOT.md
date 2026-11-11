# Booting an Android 10+ GSI

How a GSI (system-as-root, Android 10 to 14) is meant to boot on the SL8541E watches (DM63, DW100U; `androidboot.hardware=sl8541e_1h10`). **Nothing here has run on a unit.** Tags: **[R]** from the reference third-party package or the stock ramdisks; **[S]** checked against the AOSP android14-release source; **[I]** inferred, not checked against a source tree or a device.

## Boot chain

1. The bootloader loads the kernel, the ramdisk and the SPRD DT table from the boot image. Header, id recipe and DT table layout are the stock ones (`tools/sprd_bootimg.py`).
2. The kernel (4.14, GSI DT variant, see below) unpacks the ramdisk and runs `/init`. The DT `bootargs` (`root=/dev/ram0 rw init=/init androidboot.hardware=sl8541e_1h10`) are already right for a ramdisk; the header cmdline is left as stock.
3. `/init` is the static first-stage init of an Android 14 generic ramdisk. It mounts `/proc`, `/sys`, a tmpfs `/dev`, reads the fstab, mounts `system` and `vendor` (`first_stage_mount`), and switches root to `/system`. The second stage is then `/system/bin/init` from the GSI.
4. The second stage reads the init rc files from the GSI and from `/vendor`.

## The ramdisk

Built by `tools/mk_firststage_ramdisk.py`, packed by `scripts/gsi_bootimg.sh`. Contents: `/init` (0750), `/fstab.sl8541e_1h10` and `/first_stage_ramdisk/fstab.sl8541e_1h10` (same file), and empty mount points (`dev proc sys mnt metadata debug_ramdisk second_stage_resources system vendor first_stage_ramdisk`; each is justified in the tool). It is deterministic and the tool prints a manifest with the sha256 of every entry.

The init binary is an input, not part of this tree. The tool takes `--init` and requires `--init-sha256`, and refuses a non-ELF, a non-ARM-32 (`--arch arm64` for a 64-bit init) or a dynamically linked file by reading its ELF headers. Keep the binary, and its provenance, outside the repo.

```
python3 -I tools/mk_firststage_ramdisk.py --init INIT --init-sha256 HEX \
    --fstab devices/common/gsi/fstab.sl8541e_1h10 -o rd.gz --manifest rd.manifest
scripts/gsi_bootimg.sh Image DTB stock_boot.img out.img --init INIT --init-sha256 HEX
```

### fstab

`devices/common/gsi/fstab.sl8541e_1h10` has two lines, `system` and `vendor`, ext4, `ro,barrier=1`, `wait,first_stage_mount`, on the `by-name` paths of the stock fstab **[R]**. No `avb` flag: these watches never verify vbmeta in the OS (the tool refuses one unless `--allow-avb`). `userdata`, `cache`, `prodnv`, `misc` of the stock fstab are mounted by the second stage from the vendor fstab, not here.

Placement **[S]**: `fs_mgr_fstab.cpp` `GetFstabPath()` tries, for each of the boot config values `fstab_suffix`, `hardware`, `hardware.platform`, the prefixes `/odm/etc/fstab.`, `/vendor/etc/fstab.`, `/system/etc/fstab.`, `/first_stage_ramdisk/system/etc/fstab.`, `/fstab.` and `/first_stage_ramdisk/fstab.`, and takes the first file that exists. With `androidboot.hardware=sl8541e_1h10` the ramdisk files are found; the file is in both ramdisk places. `androidboot.fstab_suffix=<x>` would select `fstab.<x>` instead; it is not used (it needs a cmdline change).

### Device tree: no fstab node

If the DT has `firmware/android/fstab`, first-stage init uses it (`ReadFirstStageFstab()` reads the DT first and falls back to the default fstab only when that fails) **[S]**. The stock DT and the in-tree DTS of this repo have one (`wait,avb`, from `sharkle-board-common.dtsi` and each board file), which is what the 8.1 ramdisk expects. A GSI image needs a DT variant without that node: on the 4.14 tree, `sp9832e-1h10-dm63-adf-gsi.dts` and `sp9832e-1h10-dw100u-silver-adf-gsi.dts` (branch `config/gsi-414`, `devices/GSI_414.md`). Do not pack the stock DT table with a GSI.

## The vendor patch

The stock 8.1 ramdisk carries the device rc files at its root: `init.sl8541e_1h10.rc`, `init.sl8541e_1h10.usb.rc`, `init.storage.rc`, `init.ram.rc`, `init.common.rc`, `init.environ.rc`, `init.recovery.sl8541e_1h10.rc`, plus `fstab.sl8541e_1h10` and `/sbin` binaries **[R]**. The first-stage ramdisk above has none of them, and a GSI's init reads vendor rc files from `/vendor/etc/init` (`hw/init.${ro.hardware}.rc` for the hardware one) **[I]**. So the vendor partition has to be patched: put the device rc files (adapted: 8.1 syntax, `/sbin` paths, `mount_all` of the fstab, services running from the ramdisk) under `/vendor/etc/init/hw/`, and the fstab as `/vendor/etc/fstab.sl8541e_1h10` for `mount_all`. The kernel modules (`mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`, `sprdwl_ng.ko`) are loaded by `insmod` lines from those rc files in stock; they must come from `/vendor/lib/modules` then. The patch is on branch `devices/vendor-gsi-patch` (`devices/VENDOR_GSI_PATCH.md`).

## Untested

- Everything above, on a unit. No image from `gsi_bootimg.sh` has been flashed (the script was only run on a stock dump with a dummy kernel and DTB).
- That the A14 init (a 32-bit static binary on the arm64 kernel, `COMPAT=y`) starts on this kernel.
- The 4.14 GSI DT variants and the vendor rc patch on a unit, SELinux (the GSI policy against the 8.1 vendor), the HALs of the GSI against the vendor image, and `/data` encryption (stock uses `encryptable=footer`).
- Whether the bootloader accepts the image without a valid AVB hash descriptor (the packer warns that it is stale; only an unlocked device is expected to boot it).
