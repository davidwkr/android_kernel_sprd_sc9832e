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

- **Palm cover does not work.** Our `hynitron_cst9217.c` reports no key. Stock (static disassembly of the stock
  kernel, `cst3xx_touch_report`): in the 0xd000 report (7 bytes, `buf[6] == 0xAB`, `buf[0] != 0xAB`), `buf[4] ==
  0x80` is a palm cover; while the screen is on it sends `KEY_F4` press, SYN, release, SYN and skips touch processing
  for that report; no debounce. Probe declares `KEY_F4` (and `KEY_WAKEUP`, `BTN_TOOL_FINGER`). Fix: do the same in
  our driver (declare `KEY_F4`, handle `buf[4] == 0x80` while not asleep), confirm with `getevent` during a palm
  cover, review, build, test. Scripts and disassembly: `~/.cache/tmp-build/re.pLOX/`.
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

## Test log

Results on hardware. "Pass" means seen working on the watch; "fail" lists what was seen. Evidence in
`~/.cache/qa-out/` (directories named after the device and build).

### DM63, 4.4 (v11 on the pilot and bench)

| Test | Result |
|---|---|
| Boot, display, touch, launcher | pass |
| Speaker audio, playback and capture | pass (after the `memset_io` fix) |
| Calls, both directions | pass |
| Bluetooth pairing and A2DP | pass |
| Wi-Fi off/on, LTE data | pass |
| GPS fix with A-GPS (runtime `NOKIA-EE=TRUE`) | pass (outdoors, user) |
| Palm cover (`KEY_F4`) | pass (after the cst0xx fix) |
| Raise to wake | pass |
| Deep sleep on battery (`AP_DEEP_SLEEP_CNT` rising) | pass (after the dispc gate fix) |
| sysrq panic reboots by itself, pstore kept | pass (sysdump off) |
| eMMC write and verify | pass |
| USB plug/unplug cycles, status follows | pass (after the musb REMOVED fix) |
| Charge-voltage override (CV hold) | pass |
| Camera (Open Camera) | **fail**: oops in `isp_hist_done` on v11 (pilot) |

### DM63, 4.4 camera bisect (bench)

| Build | Content | Result |
|---|---|---|
| v13 | v12 + ISP statistics fix | preview starts, memory corruption (sync fence list), reboot |
| v14 | v13 - `d7a97c968947` (camera lifetime) | noise on the preview, zsmalloc corruption, reboot |
| v15 | v14 - `c82b428a5b90` (MAP_KERNEL kaddr) | black preview, no crash; fetch config rejected (fd 0) |
| v16 | v15 + fetch compat | noise, then freeze (pstore empty after the forced power off) |
| v17 | v16 - `f046da13e52a` (camera bounds) | built, not run |

### DM63, 4.14 (bench)

| Build | Result |
|---|---|
| test 6 | Trusty online, keymaster connected; Oops at screen off in `dphy_glb_disable` |
| test 7 | Android boots fully; screen off/on fine; modem starts, SIM READY; no cell (`vddrf125` off); no sound card; saudio smem warnings |
| test 8 | modem camps on LTE (`vddrf125` on); 3 h 12 min soak: no reboot, 6 screen cycles, max 50 C; one `vddusb33` unbalanced disable at a USB re-enumeration |
| test 11 | 4 sound cards, saudio clean; speaker path running; VSP probes but the encoder times out |
| tests 13-15 | VSP MM gates, PD_MM_TOP left on, camera power domain reference: still timing out (VSP interrupt fires per frame) |
| test 16 | VSP interrupt handling as 4.4: valid H.264 recording (410x502, 46 frames in 5 s); speaker audio heard by the user |
| test 17 | charger-manager: `battery` supply (type Battery, Li-ion, health Good), USB vs AC, JEITA zone 3, plug cycles on USB and on a wall charger (DCP detected, JEITA re-evaluated, charging restarted); no `vddusb33` warning |

### DW100U silver, 4.4 v13 (Magisk ramdisk)

| Test | Result |
|---|---|
| Boot to Android, kernel 4.4.302 | pass |
| SIM, network registration (UMTS) | pass |
| Wi-Fi | pass (user) |
| Bluetooth | pass (user) |
| Call with speaker audio | pass (user) |
| Microphone (voice recorder) | pass (user) |
| Palm cover | **fail**: no key reported |
| dmesg | no Oops/WARNING/BUG; brightness write failures (4), hardware spinlock failures (3) |
| logcat | no app crashes or ANRs; lights HAL cannot open two LED attributes |
| Thermal | CPU/GPU zones read (about 55 C while charging), board 27.8 C, battery fixed 20.0 C (stock DT) |
| Root for restore | Magisk works (`su -c id` is uid 0) |

## DW100U progress

1. Identified the unit: DW100Ultra silver (DT and kernel identical to the stock silver image), stock 4.4.83 with
   Magisk.
2. Backed up the boot partition (Magisk boot) and verified it three ways.
3. Flashed v13 BENCHDEBUG: booted, but no root (the ROM is a production build; root came from Magisk).
4. Built `dw100u-silver_v13_magisk.img` (v13 kernel and modules in the original Magisk ramdisk, original DT); flashed
   by the user with fastboot; root back.
5. Tests above. Next: palm cover fix, then plug cycles, deep sleep, raise-to-wake, GPS, AOD, call uplink.

## Housekeeping

- Push the reviewed local branches when approved (4.4 and 4.14 fix branches, test branches, notes).
- Bench DM63 screen settings: use `svc power stayon false`, not `settings put` (PowerManager does not pick it up).
- Update `KERNEL_BRINGUP_LESSONS.md` with the camera findings once the cause is known.
