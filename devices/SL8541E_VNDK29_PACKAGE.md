# SL8541E_VNDK29: Android 10 vendor and boot image for the watches (reference)

A third-party package with an Android 10 (VNDK 29) vendor and a matching boot image for SL8541E
watches. Inspected read-only on 2026-11-11; nothing from it was executed or flashed.

## Source

| | |
|---|---|
| File | `SL8541E_VNDK29.zip` (397 MB): `SL8541E_VNDK29/boot.img`, `SL8541E_VNDK29/vendor.img` |
| sha256 | `3ef05898dc03d5e77b8cf3eaa342e39f0a76098b01c4495ed29fc38966aca9f1` |
| Origin | received through a messenger; community build, no kernel source; treat as untrusted |

## boot.img

- SPRD boot image, page 2048, one DTB in the dt table, cmdline `buildvariant=userdebug`.
- Kernel `Linux version 4.4.147 ... gcc 4.9.x ... #88 SMP PREEMPT Fri Jul 17 04:47:41 CST 2026`
  (`userdebug`). Same SPRD 4.4.147 level as HMD's Nokia C1/C2 SC9832E trees; the watches' stock is 4.4.83.
- Ramdisk (gzip): Android 10 static `init` with `/first_stage_ramdisk`, `selinux_setup` and
  `first_stage_mount`; `fstab.sl8541e_1h10` / `fstab.sp9832e_1h10` mount `/system` and `/vendor`
  (`ro,barrier=1`, `wait,first_stage_mount`). This is the two-stage system-as-root boot the watches lack.
- DTB: model "Spreadtrum SL8541e Board", no `android,fstab` node (first-stage fstab is in the ramdisk),
  panels `lcd_co5300_oled178_mipi`, `lcd_co5300_oled1952_mipi`, `lcd_co5300_oled204_mipi`,
  `lcd_ch13613_oled204_mipi`, `lcd_ch13613_oled213_mipi`, touch `hynitron,hyn_ts` and `cst0xx,cst0xx_ts`
  (the DW100U family). Bootargs end in `androidboot.selinux=permissive`.

- **The kernel is pre-rooted:** embedded config (`extract-ikconfig`) has `CONFIG_KSU=y` and
  `CONFIG_KSU_LSM_SECURITY_HOOKS=y` (KernelSU; KernelSU strings in the image), plus
  `SECURITY_SELINUX_DEVELOP`, `KALLSYMS_ALL`, `DEBUG_FS`. Never use this kernel on a managed watch;
  report its circulation to the security team. The same `boot.img` also circulates on its own
  (sha256 `f34fa572f3f6696e46076090f61b43b666e272a371512c5c59e8ea51743954af`).
- Reusable as knowledge only: the first-stage ramdisk layout (system and vendor with
  `first_stage_mount`; rebuild it from a trusted `init`, do not reuse this one) and the config as a
  reference for newer Android on 4.4. Versus our 4.4 candidate it adds only `CGROUP_BPF`,
  `CGROUP_SCHEDTUNE` (newer-Android builds) and `SPRD_SIPC_STTY`, `SPRD_SIPC_GNSS_STTY`,
  `SPRD_AUDIO_SIPC` (its Android 10 vendor's GNSS and audio path), besides KernelSU.

## vendor.img

- Sparse ext4, 400 MiB unpacked (102400 x 4 KiB): exactly the watches' vendor partition size.
- `ro.vendor.build.fingerprint=SPRD/sl8541e/sl8541e:10.0.0/OPM2.171019.012/29215:user/release-keys`,
  `ro.product.vendor.device=dw99`, `ro.product.vendor.name=lineage_dw99`, build date 2026-07-16,
  `ro.board.platform=sp9832e`, `ro.product.first_api_level=29`, `ro.hardware.keystore=sprdtrusty`,
  `ro.vendor.vndk.version=1` (as on stock).
- HALs (vintf manifest): audio 4.0 (+ effect 4.0, sound trigger 2.0), bluetooth 1.0 (+ a2dp offload),
  camera provider 2.4, drm 1.0, gatekeeper 1.0, gnss 1.1, graphics composer 2.1, allocator/mapper 2.0,
  health 2.0, keymaster 3.0, light 2.0, media omx 1.0, memtrack 1.0, power 1.2, radio 1.1, sensors 1.0,
  usb 1.1, vibrator 1.0, wifi 1.3, supplicant 1.1, hostapd; vendor.sprd connmgr, gnss 2.0, power 3.0,
  thermal, log, face, aprd, radio flavor.
- `lib/hw`: `hwcomposer.sp9832e.so` (ADF: `SprdADFWrapper`), `gralloc.sp9832e.so`, `gsp.sp9832e.so`,
  `audio.primary.sp9832e.so`, `gps.default.so`, `keystore.sprdtrusty.so`, `lights`, `sensors`.
- `lib/modules`: `mali.ko` (Midgard r28p0-01rel0), `sprdwl_ng.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`,
  `sprd_vibrator.ko`, all vermagic `4.4.147 SMP preempt mod_unload modversions aarch64`.
- `etc/fstab.sl8541e_1h10`: `/data` f2fs or ext4 `formattable` without encryption flags, `/cache`,
  `prodnv` at `/mnt/vendor`, `misc`, USB and SD vold entries; zram swap 60 %.

## Fit against the DM63 and DW100U (checked 2026-11-11, read-only over adb)

- Same partition table on both watches: vendor `mmcblk0p35` = 400 MiB on both, system 3 GB (DM63) and
  4.5 GB (DW100U); the Android 14 GSI (2.4 GiB unpacked) fits both.
- Fitted panels are in the DTB: DM63 `lcd_id=ID5300213` (CO5300 2.13"), DW100U `ID5300204`
  (CO5300 2.04", per `dw100u-silver/SPEC.md`). DW100U stock panels missing from the DTB:
  `lcd_co5300_oled_213_mipi`, `lcd_ch13613_oled206_mipi`, `lcd_icna3311_oled148/214_mipi`,
  `lcd_jd9855_oled_cto20_mipi`, `lcd_st7801_oled206_mipi` (other hardware revisions would get no display).
- Stock DT nodes missing: `tyhx,hrs3xxx` (heart rate, both), `goodix,fingerprintree` (DW100U),
  `compass,af6837` (DM63). The DTB adds `heartrate,bd1xxx`.
- Stock vendor services missing in this vendor: fingerprint HAL (`biometrics.fingerprint@2.1`, `goodix.rc`),
  `gnss_download.rc`, `vendor.sprd.hardware.gnss@1.0`, `ifaa`, `power@2.0`, `zram.rc`, `engineermode`,
  `storageproxyd_androidone`; it brings `gnss@2.0`, `power@3.0`, `radio@1.4-sprd-compat`, `thermal@2.0`,
  `wcn.rc`, `engpc`, microtrust storageproxyd.
- GNSS: `gps.default.so` differs from both watches' stock HAL (sha256), only a 32-bit HAL is shipped
  (stock patched one is 64-bit), its own `etc/config.xml` / `supl.xml` and `gpsd`. The A-GPS patches
  (SUPL on any network, TLS, wakelock, IMSI log) would have to be redone on it.
- Unknown until tested: radio HAL vs the watches' modem firmware, charger and sensor behaviour of its kernel.

## What it means

- With this boot ramdisk and vendor, "ab" GSIs (Android 10-14) boot as designed: system-as-root through
  the first-stage ramdisk, and the GSI's `com.android.vndk.v29` matches the vendor (no vndklite
  workarounds). Audio 4.0 is still supported by Android 14 (`libaudiohal@4.0`).
- Our kernels do not drop in: the vendor's Mali userspace is r28p0 (our 4.4 and 4.14 ports carry
  r21p0 for the stock 8.1 blob) and its modules are built for 4.4.147. Using it with our kernel means a
  Mali r28p0 kbase port and rebuilt modules, or bench-testing with its own kernel.
- The DTB is DW99 / DW100U family; the DM63 needs its own DT.
- Not for field watches as is: unknown origin, no kernel source (GPL request needed), `userdebug`
  kernel, SELinux permissive in the bootargs. Needs the security team's review.
