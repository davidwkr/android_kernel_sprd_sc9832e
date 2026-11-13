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

## DM63 camera ioctl ABI (stock vs ours)

Static comparison, stock 4.4.83 `boot.img` against our `kwt-v14` camera tree (DCAM_IF_R4P0, ISP_R6P11, CPP_LITE_R3P0).
Status: PARTIAL (CPP capability and a few helpers still open, see below). Result so far: sizes and field offsets of
every DCAM/ISP ioctl match except `sprd_img_parm` (968 vs 1008), `sprd_img_read_op` (192 vs 196) and CPP
`scale_capability` (28 vs 44). The first two are already translated in `dcam_core.c`; the ABI is not the corruption.

| Ioctl / struct | Stock | Ours | Result |
|---|---|---|---|
| `sprd_img_parm` (SET_FRM_ID_BASE, SET_CROP, SET_OUTPUT_SIZE, SET_FRAME_ADDR, PATH_FRM_DECI, SET_PATH_SKIP_NUM) | 968 | 1008 | differs, translated |
| `sprd_img_parm` via SET_SHRINK, PDAF_CONTROL, EBD_CONTROL (cmd sizes 4/20/12, handlers read the struct) | 968 | 1008 | MISMATCH, NOT translated |
| `sprd_img_read_op` (read()) | 192 | 196 | differs, translated |
| `sprd_img_frm_info` (GET_FRM_BUFFER) | 136 + 16 rsvd | 156 | zoom_ratio@108 added, translated |
| `sprd_img_path_capability` (GET_PATH_CAP) | 180 | 184 | support_4in1@8 added, translated |
| DCAM small cmds (mode, skip, sensor size/trim/if/max, flash, 3dnr, vcm, fmt, ...) | same | same | match |
| `SET_STATIS_BUF` isp_statis_buf_input | 88 / compat 68 | 88 / 68 | match, all 15 offsets |
| `ISP_IO_CFG_PARAM`, `STATIS_IO_CFG_PARAM` isp_io_param | 24 / compat 20 | 24 / 20 | match |
| `ISP_IO_CAPABILITY` | 16 / compat 12 | 16 / 12 | match |
| `ISP_IO_RAW_CAP` isp_raw_proc_info | 72 / compat 48 | 72 / 48 | match, all offsets |
| isp_dev_block_addr (fetch/2D LSC TRANSADDR) | 56 / compat 28 | 56 / 28 | match, offsets match |
| isp_dev_fetch_info (FETCH RAW_BLOCK) | 128 / compat 96 | 128 / 96 | match, offsets match |
| ISP blocks (blc 24, bpc 204, cfa 200, edge 448, gamma 3088, ynr 528, ...) 45 structs | same | same | match |
| CPP START_ROT / START_SCALE / START_DMA | 72 / 240 / 104 | 72 / 240 / 104 | match |
| CPP `SCALE_CAPABILITY` (cmd 30) | 28 | 44 | MISMATCH, not translated |

`sprd_img_parm` legacy layout (stock 968): offsets 0..851 identical to ours (channel_id, frame_addr@176, vir@464,
fd_array@752, buffer_count@848 checked in the stock `set_frame_addr`/`set_crop`/`set_output_size` handlers). After
that stock has img_statis_info 80 bytes (44 common, frame_id, dac_info), jpegls_length, ebd_ctrl, reserved[4]. Ours
inserts monoboottime/is_last_frm/time_diff/zoom_ratio (statis_info 104 bytes, 8-byte aligned at 856) and
is_high_fps/high_fps_skip_num. The compat code copies each piece to the right place; stock `set_output_size` reads
legacy offset 964 = `reserved[3]`, which our code also reads (`dcam_ioctrl.c:354`).

Native and compat both use the same cmd value only when sizes agree; stock native sizes equal ours for everything
except the cmds above. 32-bit sizes were computed with `gcc -m32` on our headers.

Not in the stock table (ours has them, a 32-bit HAL built for stock never sends them): PATH_PAUSE/RESUME (12, 13),
cmds 21, 25, 26, 30, 43, 49, 50, 54, 56..62, 64, 65. Our `sprd_img_k_ioctl` returns 0 for unknown cmds.

CPP `SCALE_CAPABILITY`: the HAL sends `0xc01c6d1e` (28 bytes), ours expects `0xc02c6d1e` (44). Our switch is on the
full cmd, so the call hits `default` (logs "fail to get valid cpp cmd", returns 0) and the HAL reads an untouched
struct. No memory write. Not a corruption cause, but scaling capability is never reported.

Method: stock `kernel` from `sprd_bootimg.py unpack` (plain arm64 Image, no kallsyms, vaddr base
`0xffffff8008080000`). The DCAM ioctl table is in rodata at file offset `0x85e2a8` (16-byte entries cmd, fn; fn from
the `R_AARCH64_RELATIVE` table at `0xcb9070`). `__func__` strings in rodata (`0x85b000..0x85f000`) resolve most
functions by adrp+add chains. `__arch_copy_from_user` is `0xffffff800836d680`. Stock functions used:

| Function | Address |
|---|---|
| `sprd_img_k_ioctl` table handlers (`dcamio_*`) | `0xffffff8008465d18..0xffffff800846b96c` |
| `dcamio_set_frame_addr` | `0xffffff8008469d78` |
| `sprd_img_read` | `0xffffff80084692c0` |
| compat ioctl (statis buf, capability, cfg param, raw cap) | `0xffffff800843b798` |
| block_addr / fetch_info compat helpers | `0xffffff800843aae0`, `0xffffff800843adc8` |
| `isp_k_*_block` (45 structs) | `0xffffff800844d7c0..0xffffff8008459234` |
| CPP ioctl | around `0xffffff8008470400` |

Open items (still to check):

- FOUND, not fixed: `dcamio_set_shrink`, `dcamio_pdaf_ctrl`, `dcamio_core_ebd_ctrl` copy `sizeof(struct sprd_img_parm)` =
  1008 from the HAL buffer, which is 968 bytes on this ROM (stock handlers copy 968). The cmd numbers carry sizes 4/20/12,
  so `sprd_img_k_compat_ioctl` does not translate them. regular_desc@44 and pdaf_ctrl@108 are in the common prefix and
  fine. `ebd_ctrl` is at legacy 940 but ours reads 972, i.e. 4 bytes past the HAL buffer end and beyond: mode/image_vc/
  image_dt are garbage, so EBD can be switched on (`path->is_work = 1`) with random values. Candidate for stray DMA.
  Fix idea: route these three cmds through the same legacy-to-native translation. Not yet confirmed on device.
- CPP `scale_capability` layout (28 vs 44) and who calls it.
- Not compared: sensor ioctl structs (`sprd_sensor_k.h`), flash cfg, 3DNR/4in1 (absent on stock), ION ioctl layout.
- Still to check: field meaning inside the ISP block structs (only sizes and the fetch/statis/raw offsets compared).
- Match of sizes does not prove the same field meaning inside the 45 ISP block structs (only sizes compared).
- The memory corruption is more likely an address problem (MAP_KERNEL kaddr, IOMMU mapping), see Urgent.

## DW100U (4.4, v13)

- Palm cover: fixed on `fixes/dw100u-palm-44` `893f5e6a746b` (KEY_F4 on `buf[4] == 0x80` while awake, as stock);
  reviewed; passes on the watch (`dw100u-silver_v13palm_magisk.img`, sha256 `7a9959cc...`). Merge into the candidate.
- AOD does not work: our ADF handles only DPMS ON/OFF, so DOZE turns the panel off and flips fail with
  "dispc is not initialized". Stock maps DOZE_SUSPEND to panel `aod-in` (idle mode) with dispc/DSI suspended and
  `aod-out` on the way back. Port in progress on `fixes/dw100u-aod-44`.
- Pedometer: to fix (not investigated yet; dig later).
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
| v17 | v16 - `f046da13e52a` (camera bounds) | noise, then reboot: kswapd oops in `zs_compact` on a pointer holding camera-like data (DMA/stale write into freed memory) |
| v18a | v17 + camera driver tree as imported (ION/display changes kept) | building |

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
| Palm cover | fail on v13 (no key); pass on v13 + `893f5e6a746b` (KEY_F4 down/up, screen off) |
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
5. Tests above.
6. Palm cover fix built into the Magisk image and flashed with `su -c dd`; palm turns the screen off. Next: plug cycles, deep sleep, raise-to-wake, GPS, AOD, call uplink.

## Housekeeping

- Push the reviewed local branches when approved (4.4 and 4.14 fix branches, test branches, notes).
- Bench DM63 screen settings: use `svc power stayon false`, not `settings put` (PowerManager does not pick it up).
- Update `KERNEL_BRINGUP_LESSONS.md` with the camera findings once the cause is known.
