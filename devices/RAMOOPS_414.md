# Persistent crash log for the 4.14 bring-up (ramoops/pstore)

The 4.14 kernel has never booted on the DM63 or DW100U silver, and the UART is not always at hand. This
adds an opt-in crash log: the last kernel messages of a failed boot survive a reset in RAM and are read
from `/sys/fs/pstore` once a working kernel (the stock 4.4, or a later 4.14 boot) is up.
**Untested on hardware**: the kernel and both DTBs only build, and the DTBs were checked offline.

## Region: the stock one, nothing new

The stock 4.4 DT already reserves a ramoops region, and the stock kernel is built for it. Evidence:

- Stock DTBs (DT table of `sfd_tool/dm63/boot.img` and `sfd_tool/dw100u-silver/boot.bin`, unpacked
  read-only to a temp dir with `tools/sprd_bootimg.py`, then `dtc -I dtb`), identical on both boards:

      reserved-memory { pstore@8845a800 { reg = <0x0 0x8845a800 0x0 0x40000>; }; ... };
      ramoops { compatible = "ramoops"; memory-region = <&pstore>;
                record-size = <0x8000>; console-size = <0x8000>; pmsg-size = <0x8000>; };

- The 4.4 tree (`security-backports`) has the same nodes in `sp9832e-1h10-dm63.dts` and
  `sp9832e-1h10-dw100u-silver.dts`, and `CONFIG_PSTORE`, `PSTORE_CONSOLE`, `PSTORE_PMSG`, `PSTORE_RAM`
  on in `sprd_sharkle_defconfig` and `dm63_defconfig`.
- The SPRD sysdump (`CONFIG_SPRD_SYSDUMP`, node `sprd-sysdump`) does not reserve a region of its own: it
  uses `memory-region` = the whole `memory@80000000` and `memory-region-re` = the sipc, wcn, gnss and cp
  regions (it dumps them), so there is no separate last_kmsg area to reuse.
- The 4.14 DTs (`sp9832e-1h10-dm63.dts`, `sp9832e-1h10-dw100u.dtsi`) already carry the same region as
  `ramoops@8845a800` (`compatible = "ramoops"` inside `/reserved-memory`, which 4.14's
  `of_platform_default_populate_init()` probes), and the 4.14 defconfig already has the pstore options on.
  So the base boards already reserve and probe it. What the variants add is below.

Address and size are therefore exactly stock: **0x8845a800, 0x40000 (256 KiB)**, so the layout is the
one the bootloader and modem already see. It does not overlap any reserved-memory node of the stock DT:

| neighbour | range |
|---|---|
| gnss-mem | 0x88300000 + 0x15a800 = ends at 0x8845a800 (adjacent) |
| **pstore** | **0x8845a800 - 0x8849a800** |
| cp-modem | starts 0x89600000 (0x1160000 further) |
| wcn-mem, sipc-mem, mpu0-dump | below 0x88300000 |
| audio_dma 0x8d840000, tos/sml/sh 0x94000000+, iq 0x98100000 | above |
| camera 0xbdf64000, framebuffer (fb) 0xbe464000, overlay 0xbf274000 | far above (top of the 2 GiB window) |

The 4.14 boards have no ION carveout in this range either. Whether the bootloader clears or reuses this
RAM across a warm reset is **unknown**: the stock 4.4 relies on it (its ramoops node is the same), but
that was not tested here, and a power cycle or a bootloader that scrubs DRAM will empty it.

## What the -ramoops variants change

`sp9832e-1h10-dm63-adf-ramoops.dtb` and `sp9832e-1h10-dw100u-silver-adf-ramoops.dtb` include the board DT
and `sp9832e-ramoops.dtsi`, which only touches `pstore_reserved` (same address, size and zones) and the
bootargs:

- the zone sizes stay stock (record, console, pmsg 32 KiB each; the rest of the 256 KiB is oops records),
  so the stock 4.4 kernel reads what 4.14 wrote: ramoops finds the zones by offset, and a different
  console size would move them. The console zone is a ring: what is kept is the tail of the boot.
- No `unbuffered`: the default write-combining mapping is kept, as on stock. On arm64 `unbuffered` maps the
  region as Device-nGnRnE, and the pmsg path copies from userspace with wide, possibly unaligned stores
  (`__copy_from_user` in `fs/pstore/ram_core.c`), which fault on Device memory. A write-combined store may
  still sit in a write buffer at an abrupt SoC reset; panics and oopses flush through the normal path.
- `loglevel=1` of the stock bootargs becomes `loglevel=7`. At 1 almost nothing reaches any console, the
  pstore console included, so the log would be empty. The bootloader may still override `bootargs`; check
  `/proc/cmdline` of a booting kernel if the log looks empty.

`arch/arm64/configs/watch_414_ramoops.fragment` pins `PSTORE`, `PSTORE_RAM`, `PSTORE_CONSOLE`,
`PSTORE_PMSG` and leaves `PSTORE_FTRACE` off (no room for it, ftrace is off).

## Build

    O=$(mktemp -d $HOME/.cache/tmp-build/k414ramoops.XXXX)
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$O sprd_sharkle_defconfig
    scripts/kconfig/merge_config.sh -m -O $O $O/.config arch/arm64/configs/watch_414.fragment \
        arch/arm64/configs/watch_414_adf.fragment arch/arm64/configs/watch_414_stock81.fragment \
        arch/arm64/configs/watch_414_ramoops.fragment
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$O olddefconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$O -j16 Image \
        sprd/sp9832e-1h10-dm63-adf-ramoops.dtb sprd/sp9832e-1h10-dw100u-silver-adf-ramoops.dtb

(the same merge as `devices/build_414_stock81.sh` plus the fragment; pack the DTB into the boot image the
way that script does).

## Reading the log after a failed boot

1. Boot the 4.14 -ramoops image; when it hangs or resets, do not power-cycle if you can avoid it: use a
   warm reset (watchdog or the reset key combo).
2. Boot something that has the same region and a working shell: the stock 4.4 image (same address, size
   and zones, so its ramoops finds the 4.14 data) or a 4.14 -ramoops boot that gets far enough.
3. Read it (the data is only there until the next boot that writes the region):

       mount -t pstore pstore /sys/fs/pstore 2>/dev/null   # Android mounts it by itself
       ls /sys/fs/pstore
       cat /sys/fs/pstore/console-ramoops*   # console of the previous boot (the failed boot's tail)
       cat /sys/fs/pstore/dmesg-ramoops-*    # oops/panic records; slots are circular, use the timestamps inside, not the number
       cat /sys/fs/pstore/pmsg-ramoops-*     # userspace pmsg (Android logcat), if any

   Files are removed from pstore (and the buffer freed) by `rm`; copy them off first.

## Risks

- **Unknown persistence.** Nothing here proves the bootloader or CP leaves the region alone on the warm
  reset a hang ends in. The first failed boot is the test: if `/sys/fs/pstore` is empty, the region is
  being cleared (or `loglevel`/`bootargs` kept the log empty), and the fix is a different region
  or the UART.
- **Panic path.** `CONFIG_SPRD_SYSDUMP` handles panics (it calls `machine_restart("panic")` after
  flushing caches), possibly before pstore's panic dump runs. The console zone still holds everything
  printed up to that point, because the pstore console writes as messages are printed, so the console
  record is the reliable one; the `dmesg-ramoops` records may be missing.
- **Early boot.** Nothing is logged before `ramoops` probes (`arch_initcall_sync`); a hang before that
  (earlycon-only stage) needs the UART. A hang later is captured.
- **Mapping alias.** Like the stock kernel, the region stays in the kernel linear map (cached) and is also
  mapped write-combining by ramoops. Nothing else touches it, so this has no effect in practice; marking the
  node `no-map` would remove the alias but moves ramoops to the `ioremap` path, which was not tried.
- **Size.** The region is 256 KiB: fine for a boot tail, small for a full log. Do not enlarge it without
  checking the modem region that starts at 0x89600000.
- Security: the log can contain kernel addresses and, with pmsg, userspace log lines. Keep pulled logs
  out of shared issue trackers without a review.
