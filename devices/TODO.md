# Open work: DM63 and DW100U kernels (4.4 and 4.14)

State on 2026-11-13. Watches: pilot DM63 on v11 (4.4), bench DM63 on v16 (4.4 camera bisect build), DW100U silver
on v13 with its own Magisk ramdisk (original boot backed up, see "DW100U" below).

## Urgent

- **Camera on 4.4 (DM63).** v11 oopses in `isp_hist_done` when the camera opens; v13 and v14 corrupt memory and
  reboot; v16 shows noise and freezes. Stock 4.4.83 works.
  - Cause 1, confirmed: our `c82b428a5b90` (MAP_KERNEL returns kaddr 0). The HAL passes that address to the ISP.
    Fix ready, not yet tested: `fixes/ion-mapkernel-system-44` `380772c3d18f` returns the address to uids below
    10000 only.
  - Cause 2, confirmed: our `f046da13e52a` rejected the HAL's fetch config without a buffer. Fix ready:
    `fixes/cam-fetch-compat-44` `93c0f2cc2a00`.
  - Still open: the noise/freeze. v17 (v16 + `f046da13e52a` fully reverted) is built and not yet run. Next suspect:
    `35cc3a7093f2` (dispc IOMMU hardening, preview overlay). Stream `dmesg -w` to the host during the test, since a
    hang leaves pstore empty.
  - Do not open the camera on the pilot (v11) until fixed.
- **Report to the security team:** `KERNEL_DS` leaks in WCN GNSS dump / Wi-Fi calibration (fixed in 4.4 and 4.14);
  ISP statistics kernel write from a userspace kaddr (`/dev/sprd_image`, 0660 system:camera, SELinux permissive on the
  stock ROM; fixed on `fixes/isp-statis-kaddr-44` and `fixes/414-isp-statis-kaddr`). Both affect stock firmware.

## DW100U (4.4, v13)

- **Palm cover does not work.** Our `hynitron_cst9217.c` reports no key; stock uses the newer Hynitron vendor driver
  (`BIG_PALM_FLAG`). Reverse engineering of the stock kernel running; then capture `getevent` during a palm cover,
  add the stock palm key (likely `KEY_F4` as on the DM63), review, build, test.
- Brightness: `oled_set_brightness ... lcd brightness setting failed` (4 times). Check that brightness changes work.
- `Hardware spinlock [0] lock failed!` (3 times). Watch.
- Lights HAL cannot open `blue_bl/on_off` and `keyboard-backlight/brightness` (the LEDs exist). Compare with stock.
- Battery temperature is a fixed 20.0 C: the stock DT sets `temp-support = <0>`. Same as stock. Enabling it needs
  proof that the NTC is wired (read the raw `adc_temp`); JEITA is blind on this model until then.
- Still to test: call uplink (the other side hears you), plug cycles and charging, deep sleep on battery,
  raise-to-wake, GPS, AOD.
- Restore path: the original boot (Magisk) is in `~/.cache/dw100u-backup-20261113-0036/boot_orig.img`, sha256
  `a6893df5...`; only copy, keep a second one. Restore with `su -c dd` from the Magisk v13 image, or fastboot. Do not
  restore unless asked.

## 4.4 next release

- v12 (Trusty hardening, saudio unmap, `KERNEL_DS`) is built, not flashed; v13 adds the ISP fix. After the camera
  fixes: build v18 = candidate + `fixes/ion-mapkernel-system-44` + `fixes/cam-fetch-compat-44` (+ whatever the camera
  bisect finds), review, bench test (camera, audio, calls, plug, deep sleep, panic), then the pilot.
- Pilot camera test after v18.

## 4.14 (DM63)

- Tests 11-17 passed: boot, Trusty, modem (camps on LTE), Wi-Fi, screen off/on, speaker audio, VSP encoder (valid
  H.264), USB PHY fix, charger-manager plug cycles (USB and wall charger), JEITA zone handling.
- Open: a real charge from below the recharge voltage; status shows "Charging" when full (stock may show Full);
  Android AC flag on a wall charger unverified; cold-path test.
- Not yet tested on 4.14: calls with an active SIM, mobile data, Bluetooth, GPS, camera (port the 4.4 camera fixes
  first), deep sleep on battery, AOD, palm cover, raise-to-wake, sysrq panic reboot, a multi-day soak.
- Debug print to remove before release: rate-limited `vsp isr sts/raw/mask` in `vsp_isr_func.c`.
- `sprd_vsp` reset mask still lacks the stock bit 8 (not needed so far).

## 4.14 (DW100U)

- DT branch `fixes/414-dw100u-dt` (vddrf125, audio, seth, VSP/JPEG, charger-manager flag and stock recharge rule)
  reviewed; no 4.14 DW100U image built or tested yet.

## Housekeeping

- Push the reviewed local branches when approved (4.4 and 4.14 fix branches, test branches, notes).
- Bench DM63 screen settings: use `svc power stayon false`, not `settings put` (PowerManager does not pick it up).
- Update `KERNEL_BRINGUP_LESSONS.md` with the camera findings once the cause is known.
