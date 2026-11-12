# Kernel bring-up lessons: SPRD SC9832E watches on the stock Android 8.1 vendor

What it took to run our own kernels (4.4.302, 4.14.336) under the **unchanged stock userspace** (system, vendor,
secure firmware, bootloader) of the DM63 (and, by design, the DW100U). Written as a checklist for the next kernel
(5.x or later): every item below is something the stock userspace or firmware silently depends on. Status as of
2026-11-12: 4.4 (v11) runs daily on a pilot watch; 4.14 (test 8) boots Android with Trusty, the modem and Wi-Fi up;
the audio, video codec and charging fixes (tests 11 and 12) are built but not yet run.

Rule of thumb: **the stock vendor binaries and the bootloader are the spec.** A newer kernel can be correct by
upstream standards and still break them. When in doubt, read the stock 4.4.83 kernel (strings, kallsyms,
disassembly; `tools/sprd_bootimg.py`) and match its behaviour exactly.

## 1. Boot image and bring-up tooling

- **Image format:** SPRD boot image with a SPRD DT table (one DTB), page 2048, 36700160-byte partition. Keep the
  stock ramdisk; only replace modules and add rc fragments (`tools/sprd_bootimg.py`, `tools/sprd_ramdisk.py`).
- **Kernel modules in the ramdisk** (`/lib/modules`: `mali.ko`, `sprdwl_ng.ko`, `sprdbt_tty.ko`, `sprd_fm.ko`) must
  come from the same build. Stock init `insmod`s them. Out-of-tree vendor modules built for 4.4.83 (`aod_idle.ko`,
  `batt_idle.ko`) refuse other kernels; their features had to go in tree with the **same sysfs ABI**.
- **Bench debug image:** early authenticated adb from the ramdisk (`devices/BENCH_DEBUG.md`). Without it the first
  boots are blind. Bench watches only, never MDM-managed or user watches (it bypasses the MDM adb policy).
- **Command line:** stock `loglevel=1`. `loglevel=7` sends everything synchronously to the 115200-baud console and
  overheated the watch. Bootargs live in the DTB `/chosen` (`sprd_bootimg.py dt-bootargs`).
- **pstore/ramoops** at the stock `pstore@8845a800` region, with console, dmesg and pmsg records. It is the only
  evidence after a crash or a hang.
- **Flashing with root:** `dd` to `by-name/boot`, read back and compare the sha256 after `sync` and a cache drop.
  toybox `dd` rejects `conv=fsync`. Always keep the previous image and the stock image as rollback.

## 2. Things that crashed or hung the first boots

| Symptom | Cause | Lesson |
|---|---|---|
| Bootloop at ~50 s (4.4 v1) | Our Mali r21p0 hardening touched `evict_node` that was only initialised for native allocations | Re-test GPU hardening against the stock `libGLES_mali` (wallpaper, imported dma-bufs) (`MALI_R21P0.md`) |
| Black screen, ~950 HWC errors | Our ADF uapi layer record is 68 bytes; the stock hwcomposer sends 48 | uapi structs are frozen by the stock HAL: accept the old layout (`ADF_STOCK_HWC.md`) |
| Panic on first audio use | Stable 4.4.208 added a `memset()` of the PCM DMA buffer in `hw_params`; arm64 `memset` uses `DC ZVA`, which faults on Device memory (SPRD iram buffers) | Stable updates can break vendor drivers; use `memset_io` on device-mapped buffers (`PSTORE_CRASH.md`) |
| Watch hot (75 °C), load 10 | SC7A22 accelerometer IRQ storm (~1900/s): stock arms the level trigger opposite to the pin and re-arms per event | Copy interrupt semantics from stock, per board revision (`SC7A22_IRQ.md`) |
| Stuck at the logo after any panic | Vendor `systemDebuggerd` turns sysdump on (PMIC bit 0x200); the bootloader then waits in dump mode | Ship `persist.sys.sysdump=off` (`setprop debug.sysdump.enabled false`); kernel default off as a safety net (`SYSDUMP_DEFAULT_OFF.md`) |
| Stuck at the logo after a normal reboot (4.14) | The 4.14 restart handler read-modify-wrote the PMIC status and kept 0x200 | Write the reboot mode exactly as 4.4 `arch_reset` does |
| Oops at the first screen off (4.14) | The ADF glue looked up its regmaps by `sprd,sys-ap-ahb`/`sprd,sys-aon-apb`, which the 4.14 dtsi did not give those syscons; clocks also registered after the ADF probe | Keep the stock DT compatibles that vendor glue looks up; look clocks up at use time when providers can come later |

## 3. Interfaces the stock userspace and firmware depend on

- **Display (ADF):** 48-byte layer records; flips with system-heap buffers need the ion buffer passed to the IOMMU
  map (4.14 bug); the stock hwcomposer polls ADF, not DRM.
- **GPU (Mali r21p0, UK 11.0):** must match the stock user library version. The library calls the SPRD boost ioctl
  29 (`0x4004801d`). Clamp devfreq to the DT `sprd,dfs-range-max` (our build used 680 MHz instead of 600). Mali shares
  one IRQ line for job, MMU and GPU (normal).
- **Trusty TEE:** keymaster, gatekeeper, RPMB/storage proxies and the modem image verifier (`kernelbootcp`) all
  use `/dev/trusty-ipc-dev0`. The stock TEE expects the **4.4 virtio IPC wire behaviour** (independent single-buffer
  RX entries). The 4.14 sg variant never got the link online, so no device node, no keystore, a `system_server`
  crash loop, and no modem start. Init then misreports these exits as "killed by signal 1". With the 4.4 wire
  behaviour 4.14 gets `GO_ONLINE` at 0.8 s and the modem starts (4.14 boots 7 and 8).
- **Modem (cproc):** the vendor `modem_control` writes the images to `/proc/cptl/*` and `/proc/<cp>/start`. U-Boot
  fills the `cptl` memory layout (7 segments at 0x89600000). Start depends on Trusty verification. Keep the cproc
  logging at info level, otherwise the start path is invisible.
- **eMMC:** HS400ES at 200 MHz with the stock delay `0x30307f5a`. `card_busy()` must test **DAT0 only**. Newer cores
  send the cache flush with a 30 s timeout, above the host's max busy timeout, so the core polls `card_busy()`; with
  DAT[3:0] the poll ended early and the next write's Auto-CMD23 got no answer (all writes failed on 4.14). The
  regulator driver's voltage choice in a window differs between kernels (2.95 V on 4.4, 2.9 V on 4.14): harmless here,
  but check it.
- **USB and charging:** the battery driver learns about plug and unplug from the usb-charger notifier. Stable
  556e036acaa2 ("usb: musb: fix gadget state on disconnect") overwrote the glue's `REMOVED` with `NOTATTACHED`, so after
  one plug cycle the watch showed "charging" forever. 4.14 has no such framework: retest plug cycles on any new kernel.
- **Touch (cst0xx):** palm cover is a `0xAA` report that stock sends as `KEY_F4`, consumed by the vendor palm feature
  (`palm.press.screen`). The framework's raise-to-wake uses the SC7A22 key device (`KEY_F6`/`F10`/`POWER`).
- **AOD:** `/sys/kernel/aod_idle/*` plus the `aod_dtap` input device, driven by the vendor `aod_idled` and
  `persist.sys.aod_*`. AOD keeps the display controller on, so no deep sleep while it shows (expected).
- **Charging extras:** stock-compatible charge-voltage hold (4.4 `sprd_2721_charge` `cv_override_mv`, 4.14
  `sc2720_charger`; the stock `batt_idle.ko` used `/sys/kernel/batt_idle`). Different paths per kernel, so agents must
  probe.
- **WCN (Wi-Fi/BT/GNSS/FM):** the CP2 handshake must match stock (`WCN_STOCK_FW.md`). GNSS needs the A-GPS config in
  the HAL's runtime copy `/data/gnss/config/config.xml` (`NOKIA-EE=TRUE`), which is not a kernel matter.
- **Video codec (VSP):** the stock H.264 encoder aborts on 4.14 (`H264EncPreInit` failed); still open there.
- **Nodes the stock DT enables:** a board DT ported by hand inherits `status = "disabled"` from the SoC dtsi. On 4.14
  the DM63 DT had no audio (no `digital-codec` link: the SC2721 codec probe failed with -EINVAL, no sound card) and
  no video or JPEG codec (`vsp`, `jpg` and their IOMMUs disabled: the stock H.264 encoder aborted in
  `H264EncPreInit`). List every `status = "ok"/"okay"` node of the stock DT and check each one in the new DT; on the
  first boot check `/proc/asound/cards` (stock names `sprdphone`, `all-i2s`) and `ls /dev/sprd_vsp`.
- **Modem shared memory:** the `sipc-lte` smem pool is a fixed 3.9 MB region laid out by the modem firmware. The 4.14
  modem dtsi defines 14 seth channels where stock has 8; each takes 12 KB of the pool, and the voice audio buffers
  (`saudio`) then failed to allocate. Read `/sys/kernel/debug/sipc/smem` and keep the channel set of stock.
- **Charging policy:** on 4.4 the stock `sprd_battery.c` is both the battery supply Android reads and the charging
  policy (JEITA temperature zones, charge timeout, recharge voltage, adapter currents). A newer kernel spreads this
  over the fuel gauge, the charger driver and charger-manager. On 4.14 charger-manager never probed (it required a
  regulator), so there was no `battery` supply (Android showed AC powered, level 100), no temperature limits and no
  timeout; only the charger's probe-time CV (the battery's 4480 mV) applied. Making charger-manager safe took five
  review rounds: the charger driver must not start a charge on its own, stops must be verified and retried, timers
  must count suspend, probe must force the charge off before anything can defer. Take the zone table, timeouts and
  recharge voltage from the stock battery node, and test charging attended before any unattended use.

## 4. Power and thermal

- **Deep sleep:** check `AP_DEEP_SLEEP_CNT` in the SPRD sleep debug output, with the screen off on battery. On 4.4
  and 4.14 the dispc resume enabled its AHB gate twice and suspend released it once (non-stock "Re-enable GLB"
  code), so no deep sleep ever (`DEEPSLEEP_DISPC.md`).
- **Thermal:** stock never throttled the GPU; we added the SPRD GPU cooling device (units Hz→MHz, mV) and the
  power-allocator governor. A newer kernel needs the DT cooling node and trips too.
- **zram:** a newer lz4 API capped zram output at one page and gave swap write errors. zram needs a two-page
  output buffer.
- **Regulators:** a newer kernel switches off unused regulators in `regulator_late_cleanup()`; 4.4 never does, so
  anything the bootloader left on stays on there. Marking the stock `sprd,default-on` supplies always-on (eMMC core,
  vdd18, vddrf18, vddldomem...) is not enough: **`vddrf125` is not default-on in stock, but the modem's LTE RF needs
  it**. With it off, 4.14 started the modem and read the SIM but never saw a cell (`NO_NETWORK_FOUND`). Compare the
  `<name>: disabling` lines of the new kernel's boot log against what the modem, WCN and sensors use.

## 5. Security work that changed behaviour

Backports and hardening were where most regressions came from: the Mali refusals, stable 4.4.208 (PCM clear),
4.4.302's musb state fix, the lz4 upgrade. Vendor drivers also carry their own bugs worth a sweep on any tree: error
paths that return with the address limit still at `KERNEL_DS` (WCN GNSS dump and Wi-Fi calibration save, sensor and
touch drivers), and `vm_map_ram()` mappings freed with `iounmap()` (saudio). For every security change, list the stock userspace callers and test them
on hardware. Found and fixed along the way:
- the 4.14 `/proc/sprd_sysdump` stack overflow and its crash triggers;
- Mali CVEs (`MALI_R21P0.md`, `MALI_R28P0_CVE_44.md`);
- lz4 partial-decode over-read.

The stock DM63 ROM runs SELinux permissive; release builds of our kernel drop `SECURITY_SELINUX_DEVELOP`.

## 6. Debugging methods that worked

- **Capture from the first seconds:** the bench adb is up at about 4 s. Pull dmesg early, raise
  `CONFIG_LOG_BUF_SHIFT` (WCN polling wraps 512 KB in about 30 s), and boot with `printk.devkmsg=on`.
- **Tracing:** `trace_event=signal:signal_generate` works, but **Android init turns `tracing_on` off** at boot, so
  re-enable it at runtime. A process init reports as "killed by signal N" can still show `CLD_EXITED`: believe the trace.
- **Do not trust one log line:** WCN log spam evicts the evidence, and logd drops lines (`chatty ... expire`). The
  lights HAL lines were dropped, and that once made a launcher bug look like a backlight bug.
- **Use the hardware's own state:** panel registers through the AOD `peek` (DCS `0x0A` power mode, `0x52`
  brightness), PMIC sleep counters, `/sys/kernel/debug/mmc0/ios`, the clock summary, `/proc/interrupts`.
- **Reproduce deliberately:** `echo c > /proc/sysrq-trigger` for panics, scripted plug and unplug cycles, 300 MB
  write-and-verify on `/data` after eMMC changes, and an on-device logger that survives unplugging (the USB host
  sees nothing while the watch is unplugged).
- **Second opinions:** independent read-only reviews found the double gate enable, the DAT0 busy bug (the decisive
  one for 4.14 eMMC) and the Trusty IPC layout. Review every fix before it goes on a watch.

## 7. Minimum test plan for a new kernel

1. Boot to the launcher, with no GPU, display or HWC errors; temperature under about 60 °C at idle; irq 54 quiet.
2. Audio playback and capture; a call both ways; Bluetooth pairing and A2DP; Wi-Fi off and on; LTE data.
3. GPS fix with A-GPS (runtime `NOKIA-EE=TRUE`).
4. Ten or more unplug and replug cycles from a PC and a wall charger: status must follow every time.
5. Screen off on battery: `AP_DEEP_SLEEP_CNT` rises; wake works; AOD cycles; palm cover; raise to wake.
6. eMMC write and verify, then reboot with no filesystem errors.
7. A `sysrq` panic reboots by itself (sysdump off) and pstore keeps the records; a normal reboot comes back.
8. Keystore works (no `system_server` crash loop); the modem starts (SIPC `dst 5` open); `modem_control` stays
   idle; the modem sees cells (no `NO_NETWORK_FOUND`) and registers with an active SIM.
   `/proc/asound/cards` lists the stock card names; `/dev/sprd_vsp` exists and a video can be recorded. No
   `<supply>: disabling` line for a supply 4.4 keeps on. `sipc/smem` has room left after boot.
10. `/sys/class/power_supply/battery` exists with type Battery and capacity 0..100; `dumpsys battery` shows USB on a
   PC and AC on a wall charger; attended plug and unplug tests show charging stops and resumes per the stock rules.
9. A full day of wear and a night unplugged, compared with stock.
