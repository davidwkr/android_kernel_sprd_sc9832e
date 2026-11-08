# Target devices

Specs for porting this kernel to three UNISOC SL8541E smartwatches. One folder per device:

| Folder | Device | Spec |
|---|---|---|
| `dw100u-silver/` | DW100Ultra, silver unit, firmware V11H | [SPEC.md](dw100u-silver/SPEC.md) |
| `dw100u-v13m/` | DW100Ultra, other unit, firmware V13M | [SPEC.md](dw100u-v13m/SPEC.md) |
| `dm63/` | DM63 | [SPEC.md](dm63/SPEC.md) |
| `mt107/` | MT107 (from the stock boot dump only, no unit examined) | [SPEC.md](mt107/SPEC.md) |
| `mt108/` | MT108 (from the stock boot dump only, no unit examined) | [SPEC.md](mt108/SPEC.md) |
| `dm82/` | DM82 (from the stock boot dump only, no unit examined) | [SPEC.md](dm82/SPEC.md) |
| `k18/` | K18, build `K18_V10A_HM_H20_EN_ZX_HSC_FC4.4V_20260115` (from the stock boot dump only, no unit examined; separate model, closest to the MT107 but with its own DT: never use the MT107 DT) | [SPEC.md](k18/SPEC.md) |

Each folder has:
- `stock.config`: the stock kernel's `/proc/config.gz` (or the IKCONFIG embedded in the stock boot image), Linux 4.4.83.
- `stock.dts`: the stock device tree, decompiled from the DT table appended to the stock boot image.

Use `stock.dts` as the device tree for the first boots. It's the vendor's, and it matches the bootloader, panel, battery and pin setup on each unit.

### In-tree device trees

`arch/arm64/boot/dts/sprd/sp9832e-1h10-<folder>.dts` exists for all seven folders (`dm63`, `dw100u-silver`, `dw100u-v13m`, `mt107`, `mt108`, `dm82`, `k18`) and is built by `make dtbs`. Each file is a self-contained, readable copy of that device's `stock.dts` (no dtsi split): raw phandle numbers in well known properties whose target is a valid provider for them (checked per binding, e.g. `gpio-controller` for `*-gpios`, `#clock-cells` for `clocks`, a regulator for `*-supply`; anything uncertain stays a raw number) (`clocks`, `interrupt-parent`, `*-supply`, `pinctrl-N`, `*-gpios`, `dmas`, `sprd,syscon-*`, ...) became `&label` references, everything else is unchanged. The explicit `phandle` / `linux,phandle` properties are kept on purpose, so the compiled DTB reproduces the stock phandle numbers (cells that were not converted still hold the raw number). With the in-tree `scripts/dtc`, each file compiles to a DTB that is byte-identical to the stock DTB (the kernel build adds a `/__symbols__` node because of `-@`).

- The first boot still uses the stock DT table that is already in the boot image (`sprd_bootimg.py pack` without `--dt`). The in-tree DTS are for review, for later edits, and for a `--dt` image once a change is wanted; they are not a boot requirement.
- Regenerate with `python3 -I tools/dts_label.py devices/<dev>/stock.dts OUT.dts`.
- Prove equivalence with `python3 -I tools/dtcompare.py --dtc ../out_dts/scripts/dtc/dtc STOCK NEW`. STOCK and NEW can each be a boot image, a DTB or a DTS. It parses the FDT binary and compares the header fields, `/memreserve/` entries, node names, property names and raw value bytes (so phandle numbers must match exactly, nothing is normalised); DTS input is compiled first. It prints `BYTE-IDENTICAL`, `SAME STRUCTURE IN ORDER`, `SAME CONTENT, ORDER DIFFERS` or the list of differences; `--strict-order` also fails on order differences. Errors exit 2. Checked: the six DTS against the DTBs of the stock boot images (DW100U V13M against its extracted stock DTB, since no V13M boot image is kept) all report `BYTE-IDENTICAL`.
- The DTS carry no per-unit data (no MAC, serial or IMEI properties). Re-check before adding a tree from a new dump.

Related: [`../TODO.md`](../TODO.md) (work plan), [`../KERNEL_TWEAKS.md`](../KERNEL_TWEAKS.md) (battery, flash-write and GNSS changes wanted once the kernel boots).

## Common platform (all three)

| | |
|---|---|
| SoC | UNISOC SL8541E (SC9832E family, "SharkLE"), 4x Cortex-A53 (`CPU part 0xd03`), up to 1.4 GHz |
| CPU frequencies | 768, 900, 1100, 1300, 1400 MHz; stock governor `interactive`; UNISOC CPU hotplug governor (`/sys/devices/system/cpu/cpuhotplug`) |
| CPU idle states | `WFI`, `core_pd`, `cluster_pd` |
| GPU | Mali (Midgard), `mali.ko` from the ramdisk |
| DT compatible | `"sprd,sp9832e-1h10", "sprd,sc9832e"`, model `Spreadtrum SL8541e Board` |
| Android | 8.1 (vendor BSP, non-VNDK `/vendor`), `ro.boot.hardware = sl8541e_1h10`, platform `sp9832e` |
| Stock kernel | 4.4.83, GCC 4.9, `CONFIG_HZ=100`, `NO_HZ_IDLE`, `PM_AUTOSLEEP`, `PM_WAKELOCKS` |
| Display | ADF (`sprd-adf`, `dispc0`, `dsi0`, `dphy0`, `panel0`), MIPI DSI AMOLED on a CO5300 controller; the panel is picked by `lcd_id` on the cmdline, and its init sequence and power-on timing are in the DT (`lcds { ... }`) |
| Connectivity | Integrated WCN (Marlin2 + GE2 GNSS): Wi-Fi `sprdwl_ng.ko`, BT `sprdbt_tty.ko`, FM `sprd_fm.ko`, GNSS through the WCN `sbuf` channels |
| Modem | LTE, `ltemode` on the cmdline; modem partitions `l_modem`, `l_ldsp`, `l_gdsp`, NV `l_fixnv*`, `l_runtimenv*` |
| Storage | eMMC; `/data` f2fs (`lazytime, noatime, background_gc=on, discard, flush_merge, extent_cache, mode=adaptive`), `/system` and `/vendor` ext4 read-only, `/cache` ext4 |
| I/O | `cfq` scheduler (`noop` available), read-ahead 128 KB |
| VM | `swappiness=150`, `dirty_background_bytes=4 MB`, `dirty_bytes=50 MB`; Android's `init.rc` sets `dirty_expire_centisecs=200`; ZRAM built in, no swap active |
| Debug | `CONFIG_SPRD_SYSDUMP`, `DEBUG_FS`, `FTRACE`, `KALLSYMS_ALL` enabled |

### Boot image

- `ANDROID!` header version 0, page size 2048.
- Kernel: uncompressed arm64 `Image`, about 18 MB, load offset `0x8000`. Ramdisk at `0x05400000`, tags at `0x100`. No second stage.
- Device tree: a UNISOC DT table (magic `SPRD`, one DTB) appended after the ramdisk. Its size is in the header word after `page_size`. Repack with UNISOC `mkbootimg` (`--dt`) or an equivalent tool.
- Header cmdline: `console=ttyS1,115200n8 buildvariant=user`. U-Boot adds `androidboot.hardware=sl8541e_1h10`, `lcd_id=...`, `lcd_base`, `lcd_size`, `vbat_ocv`, `sysdump_magic`, `modem=shutdown`, `ltemode=...`.
- The ramdisk loads `mali.ko`, `sprdbt_tty.ko`, `sprd_fm.ko` from `/lib/modules` (`insmod` in the init scripts), and `sprdwl_ng.ko` from the same folder (loaded later by the Wi-Fi HAL). These must be rebuilt for a new kernel.

### Repacking the boot image

`tools/sprd_bootimg.py` (python3, stdlib only) inspects, unpacks and rebuilds these images. Run it with `python3 -I` on a copy of the dump, and keep dumps out of the repo.

```
python3 -I tools/sprd_bootimg.py info boot.img
python3 -I tools/sprd_bootimg.py unpack boot.img dir/
python3 -I tools/sprd_bootimg.py pack dir/ new.img --kernel Image [--ramdisk FILE] [--dt FILE]
```

Module swap step: `tools/sprd_ramdisk.py` (python3, stdlib only) works on the `ramdisk` file from `unpack`.

```
python3 -I tools/sprd_ramdisk.py list dir/ramdisk
python3 -I tools/sprd_ramdisk.py replace-modules dir/ramdisk new.gz --kernel Image --module mali.ko --module sprdwl_ng.ko --module sprdbt_tty.ko --module sprd_fm.ko
python3 -I tools/sprd_bootimg.py pack dir/ new.img --kernel Image --ramdisk new.gz
```

Flash-write tweak (`TODO.md` section 8): `add-rc` adds `devices/common/init.watch_tweaks.rc` (`on boot`, `dirty_expire_centisecs` and `dirty_writeback_centisecs` 3000) as a new `/init.watch_tweaks.rc` entry before the cpio trailer and appends `import /init.watch_tweaks.rc` to `/init.rc`. Run it on the output of `replace-modules` or on the stock ramdisk; `pack` as above.

```
python3 -I tools/sprd_ramdisk.py add-rc dir/ramdisk new.gz --fragment devices/common/init.watch_tweaks.rc
python3 -I tools/sprd_ramdisk_test.py
```

- Refuses: `/init.rc` missing, not a regular file or a hard link; `/init.watch_tweaks.rc` present with other content, or imported but missing (half applied); a fragment that is empty, comment-only, over 4096 bytes, not newline-terminated or not printable ASCII; the same non-zero-tail and ambiguous-name checks as `replace-modules`. A second run on its own output writes an unchanged copy ("already applied").
- Every other entry (order, modes, mtimes, inode numbers) stays byte-identical; only `/init.rc` grows and one entry is added (mode, uid, gid and mtime copied from `/init.rc`, next free inode number). The output is re-parsed and compared entry by entry before it is written.
- Order: the import is the last line of `/init.rc`, so the new `on boot` action is queued after the stock `on boot` blocks and its writes win. The DW100U stock `init.rc` already does `import /init.environ.rc` and similar root imports, and writes `/proc/sys/vm/*` from `on boot` (so the SELinux rule for `init` exists). Stock also sets `dirty_expire_centisecs 200` in `init.rc` `on boot`, `dirty_background_ratio 5`, and in `init.sl8541e_1h10.rc` `dirty_background_bytes 4194304` / `dirty_bytes 52428800`: the 4 MiB background threshold still starts writeback before the 30 s timer when more than 4 MiB is dirty. `dirty_writeback_centisecs` is not set by stock. Checked on one local DW100U dump only (by `list`-style parsing of a temp copy, nothing committed); the other models are not checked, and nothing is verified on a booted device.
- Later overrides: in the 14 `.rc` files of the DW100U ramdisk the only `dirty_expire_centisecs` / `dirty_writeback_centisecs` write is the stock `init.rc` `on boot` one (200); no `on property:` trigger rewrites them. The system partition `.rc` files were not available, so `/system/etc/init` and vendor scripts are unchecked. Android 8.1 parses `/init.rc` and its imports before `/system/etc/init`, so a `.rc` copy in `/system/etc/init` runs its `on boot` after ours and wins. The Magisk module `dirty_expire` runs on Magisk's own schedule (its `post-fs-data` / `service.sh` scripts, or an injected `.rc`), not in init's parse order, so it can run before or after ours. Remove the userspace copies when the boot image carries this: whichever runs last wins.

- It replaces only `lib/modules/<basename>` entries; every other cpio entry, its header fields, the order and the trailer stay byte-identical. It refuses a module with no counterpart, a hard link, and a ramdisk with non-zero data after the compressed member or the cpio trailer (a concatenated archive could override the swap). Decompressed size is capped at 256 MiB.
- Vermagic is compared like the kernel's `same_magic()` with `CONFIG_MODVERSIONS`: the whole string must match exactly. The expected string is the stock module's vermagic with only the release token replaced by the release in the `--kernel` Image banner; each differing field is reported. `--symvers Module.symvers` also compares the `module_layout` CRC in `__versions`. Without `--kernel` the release is not checked.
- The stock ramdisks are gzip (no lz4). The silver Magisk-patched one recompresses to the identical stream (zlib level 9); the others were made with GNU gzip and recompress to different bytes with the same decompressed cpio. The module set is loaded by `init.common.rc` (`mali.ko`) and the `init.sl8541e_1h10.rc` (`sprdbt_tty.ko`, then `sprd_fm.ko`); `sprdwl_ng.ko` has no `insmod` line (Wi-Fi HAL).
- Strip debug info from the built modules with the toolchain's strip (`aarch64-linux-gnu-strip --strip-debug`, not `--strip-all`, which removes the symbols and relocations `insmod` needs) or build with `INSTALL_MOD_STRIP=1`. Do it before any module signing. Unstripped, the four modules are about 22 MB and the ramdisk grows from 1.4 MB to 8 MB compressed.

- Layout: header page, kernel, ramdisk, DT table (each padded to 2048), zero fill, then the AVB vbmeta blob at the next 4096 boundary and the 64-byte AVB footer at the end of the partition (36700160 bytes for `boot`). `dt_size` is the header word after `page_size`, and the id includes the DT table: both follow the vendor (CAF-style) `mkbootimg` v0.
- The header id is SHA-1 over kernel, ramdisk, second (each followed by its size) and the DT table with its size. `info` checks it; `pack` recomputes it.
- Byte-for-byte check: unpack then pack with no substitutions reproduces all 6 stock dumps: DM63, DW100U silver (Magisk-patched ramdisk), DW100U (second stock dump, `sfd_tool/dw100u/boot.img`, older kernel build, unpatched ramdisk), MT107, MT108, DM82.
- AVB: the stock vbmeta blobs are signed (SHA256_RSA2048) with a hash descriptor for partition `boot`. With a changed image `pack` moves the blob to the new image end, rewrites the footer (keeping its version fields) and warns when the descriptor hash no longer matches. A stale hash is the normal state of a modified image on an unlocked (orange) device (the Magisk-patched silver dump already has one and boots), so the footer is kept by default. `--no-avb` drops it; do not use it for the first test.
- Footer position: pass `--partition-size N` with the real size of the target partition. Without it `pack` assumes the stock `boot` size and warns. The partition XMLs of MT107, MT108 and DM82 list `recovery` at 35 MB (36700160 bytes, same as `boot`); the DM63 and DW100U sizes are not recorded here, read them from the device first.
- The built `Image` is about 23 MB against 17 MB stock. On a DM63 copy it uses about 25.7 MB of 35 MB. `pack` refuses a kernel where `kernel_addr + text_offset + max(image_size, file size)` reaches the ramdisk address `0x05400000`. These header addresses may not be the real SPRD U-Boot load addresses, so the check is a sanity limit only.
- Recovery test (details in `TODO.md` section 7). Prerequisites come first and are mandatory: a full backup of `boot`, `recovery`, `vbmeta` and the partition table verified by checksum, the unbrick tool (`sfd_tool`, UNISOC download mode) tested on that model, and an unlocked (orange) bootloader confirmed on the unit. All of these are device access and need the owner's OK. Then read the `recovery` partition size, dump the stock `recovery`, unpack it, and reuse its `vbmeta.bin` (partition `recovery`, recovery key): copy it into the unpacked boot directory and run `pack DIR recovery-test.img --partition-size SIZE --kernel Image`. Do not use `--no-avb` for the first test. Only then flash to `recovery`.
- `unpack` refuses a non-empty directory; `--force` deletes the old managed files first so stale parts never reach `pack`.

### Partitions (same on all three)

`boot cache fbootlogo gpsbd gpsgl l_deltanv l_fixnv1 l_fixnv2 l_gdsp l_ldsp l_modem l_runtimenv1 l_runtimenv2 logo logo_2..6 misc miscdata persist pm_sys prodnv recovery sml sml_bak system trustos trustos_bak uboot uboot_bak uboot_log userdata vbmeta vbmeta_bak vendor wcnmodem`

Verified boot state `orange` (bootloader unlocked) on the units checked. Unbrick path: UNISOC download mode with `sfd_tool` and a full partition dump.

## Never commit

These are device-specific. Keep them out of this repo, which is a public fork:
- partition dumps, NV/calibration partitions (`l_fixnv*`, `prodnv`, `miscdata`);
- anything from a watch's `/data`;
- serial numbers, IMEIs, MAC addresses, logs with locations.

## DW100U Goodix fingerprint driver (provenance)

`drivers/input/fingerprint/goodix/` (`CONFIG_GOODIX_TEE_FINGERPRINT`, enabled for dw100u only through `arch/arm64/configs/dw100u_fp.fragment`) binds the DT node `goodix,fingerprintree` (`goodix_fp_ree@0` on `spi@70c00000`, properties `goodix,gpio_irq`, `goodix,gpio_reset`, optional `goodix,gpio_pwr`).

- Source: `gf_common.c` and `gf_spi_access.c` derive from `drivers/input/fingerprint/gf3626/` of https://github.com/Supernote-Ratta/kernel_a6x_a5x (commit `bdc77c244cd3`, 2022-08-03). Both files carry Goodix's own GPL-2.0-or-later header, kept as is; the repo has no LICENSE metadata, so the per-file header is the basis. The Infinix NOTE5 4.4 tree (`OpenSource-Infinix/NOTE5-X604-Kernel-4.4`, `goodix_fp_tee/gf_spi_tee.c`, same header) was used to cross-check the uapi; it is MediaTek specific and was not ported.
- Upstream `gf_common.h` and `gf_platform.c` have no license header, so they were not copied: the enums, structs and ioctl macros were reproduced from the upstream `gf_common.h` for ABI compatibility (that file carries no license header); the rest of the header and the platform/probe/remove code is new.
- uapi kept: `/dev/goodix_fp` (misc device), input device `gf-keys`, ioctl magic `'g'` numbers 0..17, netlink protocol 29 (returned to the HAL by `GF_IOC_INIT`). Both public trees agree on these. The stock DW100U HAL was not inspected, so a match is likely but not confirmed.
- Changes: refcounted lifetime, devm GPIOs, bounds checked raw SPI ioctl with a 32-bit compat path, netlink registration limited to root/system, undeclared key codes no longer forwarded, sensor-specific SPI probing in `open()` (the GPIO reset stays) and the debug sysfs file dropped; netlink registration uses the socket portid; unknown `'g'` ioctls (including cmd 15, compiled out upstream) return 0 as upstream; the register read/write helpers were dropped with that ioctl.
