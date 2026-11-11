# HMD Key: official Android 14 Go on SC9832E (reference)

The HMD Key (codename `comet`, model TA-1690, 2025) runs Android 14 (Go) on the same SoC and PMIC as
the watches. Its firmware is the only known Android 14 vendor for this platform, and it shows how
Unisoc runs sharkle on a 5.15 GKI kernel. Inspected read-only on 2026-11-11; nothing from it was
executed or flashed.

## Source

| | |
|---|---|
| File | `HMDSW_cmt_109B-0-00WW-B01_user_devicekit.zip` (1.76 GiB, build 109B, 2025-09-20) |
| sha256 | `05dfc49f73f3b0b50503d79c07f7c19b6f352533c8f689c86d660f98401bb15b` |
| Mirror | `mirrors-hmd-1.lolinet.com/firmware/hmd/2025/comet/official/100WW/` (third party, no published checksum; an older build `cmt_1060` is in the same folder) |
| Kernel source | not published by HMD (no repo in `HMD-OSS-Archive`); GPL request possible |

Treat the archive as untrusted: it bundles Windows flash tools.

## Layout

`devicekit.zip` holds `romAll.zip` plus `rom.json`. `romAll.zip` holds per-partition images:

- Boot chain (signed with HMD keys, do not flash to a watch): `u-boot-spl-16k-sign.bin`, `sml-sign.bin`,
  `tos-sign.bin` (Trusty), `teecfg-sign.bin`, `lk-sign.bin` (LK, not U-Boot), `fdl1-sign.bin`,
  `lk-fdl2-sign.bin`, `vbmeta-sign.img` plus `vbmeta_{system,system_ext,vendor,product,odm}.img`.
- Kernel: `boot.img` (64 MiB, header v4), `init_boot.img` (8 MiB), `vendor_boot.img` (100 MiB, header v4),
  `dtbo.img` (8 MiB).
- `super.img` (sparse, 2.4 GiB, dynamic partitions, A/B groups `group_unisoc_a/b`): `system`, `system_ext`,
  `product`, `vendor`, `odm`, `system_dlkm`, `vendor_dlkm`.
- Modem and coprocessors: `SC9600_sharkle_pubcp_Smart_Phone_modem.bin`, `SharkLE_LTEA_DSP.bin`,
  `SHARKLE2_DM_DSP.bin`, `sharkle_cm4.bin`, `PM_sharkle_cm4_v2.bin`, `gnssmodem.bin`, `gnssbdmodem.bin`,
  plus the phone's NV items.
- Board config `sp9832e_1h10_go.xml`; `android-info.txt`: `board=sp9832e_1h10_go`.

## Findings

| | HMD Key | Watches (DM63, DW100U) |
|---|---|---|
| Board | `sp9832e_1h10_go` | `sp9832e-1h10` family |
| SoC / PMIC | SC9832E (`sprd,sc9832e`), SC2721 | same |
| Kernel | GKI `5.15.149-android13-8-g2f1c4b62898d-ab2340248` (clang 14.0.7, 2025-06-12) | 4.4.83 stock, our 4.14.336 |
| Display | DRM (`sprd-drm.ko`) | ADF (stock 8.1 vendor) |
| GSP | `sprd-gsp.ko`, `sprd,gsp-lite_r2p0-sharkle` | same block (ported to 4.14 in `GSP_414_PORT.md`) |
| GPU | `sprd,mali-midgard` (T820 MP1) | same |
| Bootloader | LK, boot header v4 with `vendor_boot` / `init_boot` | U-Boot 2015, boot header v0–v2 |
| Partitions | `super`, EROFS: `vendor_a` 125 MiB, `odm_a` 51 MiB, `vendor_dlkm_a` 3 MiB | `vendor` 400 MiB ext4, no `super`, no `odm` |

- `vendor_boot` cmdline: `console=ttyS1,115200n8 bootconfig buildvariant=user`.
- `vendor_boot` ramdisk (LZ4) first-stage modules: `aw32257_charger.ko`, `sprd_cache_print.ko`,
  `sprd-drm.ko`, `sprd-gsp.ko`, `sprd-udc.ko`, `sprd_uid.ko`; first-stage ramdisk with
  `ueventd.sp9832e_1h10_go.rc` and the vendor/odm SELinux contexts.
- DTB (one blob in `vendor_boot`, model "Spreadtrum SC9832E SoC") uses mainline-style SC27xx bindings:
  `sprd,sc2721` (+ `-bltc`, `-typec`, `-wdt`), `sprd,sc27xx-{adc,charger,fgu,bc1p2,poweroff,7sreset}`,
  `sprd,sc2731-{regulator,rtc,efuse,eic,syscon,vibrator,flash}`.
- The remaining vendor modules and the HAL list were not read yet: the vendor images are EROFS and
  `erofs-utils` was not installed.

## Route it suggests (not started)

Android 14 on the watches with Google's GKI 5.15 (`android13-5.15`, public source), HMD's SPRD vendor
modules (same KMI), HMD's vendor and odm (176 MiB, fits the watch's 400 MiB vendor partition, odm
folded in or on another partition) and a watch device tree. Unlike a GSI on the 8.1 vendor, the
vendor is built for Android 14 on this SoC.

Hard parts:
1. Boot: the watch U-Boot cannot load `vendor_boot` / `init_boot`; repack kernel + combined ramdisks
   + DTB into a header the watch U-Boot accepts. Keep the watch's own signed boot chain; HMD's is signed
   with HMD keys.
2. Watch drivers as GKI 5.15 modules: CO5300 panel on DRM, touch (`hyn_ts`, `cst0xx`), sensors, Goodix
   fingerprint, `timed_output` vibrator if the HAL needs it. Our 4.14 ports (including the DRM CO5300
   panels) are the starting point.
3. Trusty: HMD's KeyMint/gatekeeper HALs must match the watch's TEE (`tos`) trusted apps, or keystore
   fails.
4. Partitions: no `super` on the watch, so vendor/odm go to fixed partitions (fstab and verity changes).
5. Modem, WCN and GNSS firmware and NV calibration must stay the watch's own.

## Related public kernel sources (HMD GPL)

- `HMD-OSS-Archive/android_kernel_nokia_c1`, `_c2`: Linux 4.4.147 for SC9832E (sharkle, `sp9832e-1h10` DTs);
  newer than the watches' 4.4.83, worth diffing for vendor fixes.
- `HMD-OSS-Archive/android_kernel_nokia_c10`, `_c20`, `_c21`, `_c30`: SPRD Linux 4.14.193 (sharkl3, with an
  EOL sharkle defconfig); a later 4.14 vendor base than ours.
