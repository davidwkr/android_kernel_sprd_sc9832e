# Newer Android on the watches (DM63, DW100U)

State on 2026-11-11. Nothing here has been booted; every route needs a bench watch, a backup of its boot,
system and vendor partitions, and the owner's OK before any flash. Stock Android 8.1 on our kernels stays the
production track.

## Device facts (read over adb, DW100U and DM63)

- 64-bit userspace (`zygote64_32`), Treble on, first API level 26, no full VNDK (`ro.vndk.version` empty,
  `ro.vendor.vndk.version=1`, only `vndk-sp` in `/system/lib*`): vndklite GSIs.
- Not system-as-root: `/` is the ramdisk; the device files (`fstab.sl8541e_1h10`, `init.sl8541e_1h10.rc` and
  its imports, `ueventd.sl8541e_1h10.rc`) exist only in the stock ramdisk.
- Same partition table on both: vendor `mmcblk0p35` 400 MiB, system 3 GB (DM63) / 4.5 GB (DW100U).
- RAM 1.8 GB (DM63) / 2.8 GB (DW100U); Android 14 Go asks for 2 GB.
- Vendor HALs of note: audio 2.0, Keymaster 3.0, composer 2.1 on ADF, GNSS 1.0 (our patched `gps.default.so`).
- SELinux: both policies define the same 92 classes, `map` granted, `handleunknown deny`.

## Kernel limits

| Kernel | Newest GSI that boots unmodified |
|---|---|
| 4.4 (stock, ours, third-party 4.4.147) | Android 13 at most (10-11 expected to work, 12-13 probably) |
| our 4.14 | Android 14; Android 15+ needs 4.19 (network BPF loader), 16+ needs 5.4 |

Spoofing the kernel version does not help: the BPF loader then picks programs that use helpers 4.14 lacks.

## Routes

| Route | Kernel | Vendor | System-as-root | Status |
|---|---|---|---|---|
| A. GSI on the stock vendor | our 4.14 (`config/gsi-414`, GSI DT variants without the DT fstab) | stock 8.1, vndklite | first-stage ramdisk (`tools/firststage-ramdisk`) + vendor patch (`devices/vendor-gsi-patch`) | in progress |
| B. Android 10-13 on 4.4 | our 4.4 + `watch_newandroid.fragment` (`config/newandroid-44`) | stock 8.1, vndklite | same ramdisk and vendor patch | in progress |
| C. VNDK29 package | its 4.4.147 (KernelSU, no source) or ours with Mali r28p0 | Android 10 VNDK 29 (DW99) | its own first-stage ramdisk | reference only: `SL8541E_VNDK29_PACKAGE.md` |
| D. HMD Key | GKI 5.15 + HMD vendor modules | HMD Android 14 | repack for the watch U-Boot | reference only: `HMD_KEY_A14_REFERENCE.md` |
| E. Android 11 A-only GSI | 4.4 | stock 8.1 | none needed (A-only build) | candidate, not downloaded |

The first-stage `init` for routes A and B comes from the official HMD Key `init_boot.img` generic ramdisk
(static 32-bit ARM, Android 14, sha256 `4434efcb3719827f8b306be762434c12427a6b35c21f7cc6616e4c6c73f493b5`),
not from the third-party package.

## Android 14 GSI chosen

`lineage-21.0-20260614-UNOFFICIAL-arm64_bvN-vndklite.img.gz` (Andy Yan, TrebleDroid-based, security patch
2026-06-01, sha256 `892c58f57e2170f562f4e11ad2ca087b459a8257f72d60ea4b9e9c1654e2604c`). Read-only inspection:
- ships `libaudiohal@2.0` and the `android.hardware.audio@2.0` libraries (AOSP drops 2.0 in Android 12), so the
  stock audio HAL stays usable;
- VNDK apexes v28-v33 only, plus `ld.config.26.txt` and `rw-system.sh` tweaks for vndk 26/27 keyed on
  `persist.sys.vndk` / `ro.vndk.version`: set `persist.sys.vndk=27` on first boot (the watches report no VNDK
  version).

Newer vndklite GSIs: TrebleDroid `android_15.0.0_r9` (`ci-20250117`) and ponces AOSP 15 (2025-01) are the last;
later Android 15/16 builds have no vndklite variant. Android 11 A-only: LineageOS 18.1 `arm64_avS` (2024-01, with
phh superuser).

## Known risks with a GSI on these watches

- Video: Unisoc GSI reports (crDroid Android 12 on T606: reboot on playback with `GSP layer dataspace error`;
  Android 13+ wrong colours) point at the SPRD hwcomposer + GSP; test, try `dis.hwc.gsp`.
- Keymaster 3.0 under keystore2, mobile data with radio 1.x, FBE (storage stays unencrypted).
- GPS must be retested: the framework path changed since 8.1 even with the same HAL.
- Phone UI, not a watch UI.
- Rough odds (judgement): Android 14 GSI on route A boots and mostly works 15-20 %; Android 11 A-only on 4.4
  60-70 %.

## Related public sources

HMD GPL kernels (`HMD-OSS-Archive`): Nokia C1/C2 4.4.147 for SC9832E (sharkle DTs), Nokia C10-C30 SPRD 4.14.193
(sharkl3, EOL sharkle defconfig). Worth diffing against our 4.4 and 4.14 trees.
