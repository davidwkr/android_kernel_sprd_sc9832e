# Target devices

Specs for porting this kernel to three UNISOC SL8541E smartwatches. One folder per device:

| Folder | Device | Spec |
|---|---|---|
| `dw100u-silver/` | DW100Ultra, silver unit, firmware V11H | [SPEC.md](dw100u-silver/SPEC.md) |
| `dw100u-v13m/` | DW100Ultra, other unit, firmware V13M | [SPEC.md](dw100u-v13m/SPEC.md) |
| `dm63/` | DM63 | [SPEC.md](dm63/SPEC.md) |

Each folder has:
- `stock.config`: the stock kernel's `/proc/config.gz` (or the IKCONFIG embedded in the stock boot image), Linux 4.4.83.
- `stock.dts`: the stock device tree, decompiled from the DT table appended to the stock boot image.

Use `stock.dts` as the device tree for the first boots. It's the vendor's, and it matches the bootloader, panel, battery and pin setup on each unit.

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

### Partitions (same on all three)

`boot cache fbootlogo gpsbd gpsgl l_deltanv l_fixnv1 l_fixnv2 l_gdsp l_ldsp l_modem l_runtimenv1 l_runtimenv2 logo logo_2..6 misc miscdata persist pm_sys prodnv recovery sml sml_bak system trustos trustos_bak uboot uboot_bak uboot_log userdata vbmeta vbmeta_bak vendor wcnmodem`

Verified boot state `orange` (bootloader unlocked) on the units checked. Unbrick path: UNISOC download mode with `sfd_tool` and a full partition dump.

## Never commit

These are device-specific. Keep them out of this repo, which is a public fork:
- partition dumps, NV/calibration partitions (`l_fixnv*`, `prodnv`, `miscdata`);
- anything from a watch's `/data`;
- serial numbers, IMEIs, MAC addresses, logs with locations.
