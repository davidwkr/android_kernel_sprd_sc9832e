# Kernel changes wanted: battery, flash writes, GNSS

For the DW100Ultra and DM63 watches (specs in [`devices/`](devices/README.md)). Apply after the first kernel that boots cleanly on a watch (see [`TODO.md`](TODO.md)). Every change gets a before/after measurement on the same unit (method at the end).

Context: the watches run a tracking app with GPS on all day, so the GNSS session is the main battery case. Measurements below are from the DW100U silver unit on the stock kernel, 2026-10-28/29.

## 1. GNSS: let the CPU sleep while the engine runs

### What keeps the CPU awake today

1. **The GNSS HAL wakelock.** The vendor HAL (`gps.default.so`) took the framework wakelock "GPS" at engine start and held it until the engine stopped: 0 suspends during a GPS session. **Already fixed in userspace** (HAL binary patch, Magisk module `supl_any_network` v5: no "GPS" wakelock any more). Nothing to do in the kernel for this one, but the kernel must still wake reliably for GNSS data (item 3).
2. **The sbuf receive wakelock.** `drivers/staging/sprd/modem/sipc/sbuf.c` calls `wake_lock_timeout(&ring->rx_wake_lock, HZ / 2)` for every receive notification, and `HZ / 50` in the read path (the macro near line 56). The GNSS data channel (`sbuf-4-4-0-rx` on the stock kernel, the same driver family) gets bursts several times per second while the engine runs. Measured: **held 507 s of the first 575 s after boot (about 88 %)**, 448 activations, longest 16 s. With bursts at 1 Hz or more and a 500 ms hold each, the CPU can barely suspend.
3. **Wakeup reliability.** With item 1 gone, the CPU may suspend between bursts. The sbuf/mailbox interrupt must stay a wakeup source, so GNSS data still wakes the CPU and is read in time.

### Changes

- [ ] **Per-channel sbuf rx wakelock timeout.** Keep `HZ / 2` for channels that need it (modem, Wi-Fi), and use a short hold for the GNSS channel: start with `HZ / 50` (20 ms, `CONFIG_HZ=100` means 2 jiffies), enough for the HAL's reader thread to run. Implement as a per-ring timeout (the driver already has `need_wake_lock` and `sbuf_set_no_need_wake_lock(dst, channel, bufnum)`), selected by dst/channel or set by the GNSS driver at init. Identify the GNSS dst/channel in this tree first: stock names are `sbuf-<dst>-<channel>-<bufid>`, and the busy one is `4-4-0`.
- [ ] Confirm the receive interrupt (mailbox / SIPC) is wake-enabled (`enable_irq_wake`), so suspended → data arrives → resume works. Test with the HAL v5 patch: fixes must keep arriving every second with the CPU suspending between them.
- [ ] Measure: `/d/wakeup_sources` (sbuf-4-4-0-rx `total_time`), `/d/suspend_stats`, battery current on battery power, fix rate (1 Hz) with GPS on for 10 minutes.

### GNSS driver items (`drivers/misc/sprdwcn_bsp/sc2342/gnss/`)

- [ ] Check what `wcn_power_ctl.c` does when the engine stops: the GNSS core (in the WCN subsystem) should power down or go to its sleep state, not idle at full power. The HAL config has `SLEEP-ENABLE=TRUE`, `SLEEP-TIMER=300`, `STOP-TIMER=1`; check the kernel side honours the chip's sleep request.
- [ ] Debug output: `gnss_dbg.c`. Keep it off by default (no per-message printk).
- [ ] Keep the upstream fix "GNSS calibration failure no longer blocks WiFi" (commit `61193d4f`) and the WCN firmware-path fixes; the stock kernel loads GNSS firmware from the `gpsgl` / `gpsbd` partitions.
- [ ] Nice to have: expose the GNSS engine power state in sysfs (on / sleep / off), for diagnostics without root logcat.
- [ ] Not kernel work, for reference: the HAL patches (A-GPS on any network, keep TLS, no session wakelock) live in the `ge2_libgps` repo as a Magisk module. Restarting the GNSS HAL service crashes `system_server` on Android 8.1; never do that from kernel-side recovery logic either.

## 2. Flash (NAND / eMMC) write reduction

### Baseline (DW100U, tracking with GPS at 1 Hz, stock kernel)

About **1.04 MB/min** written to `/data` (about 1.5 GB/day):

| Writer | Share |
|---|---|
| `system_server`: `notification_log.db` (the tracking app updates its notification on every fix) | 41 % |
| Tracking app, its own database and logs | 21 % |
| GNSS HAL, `FlashCfg.cg` chip state (`fwrite` + `fflush` every 1–2 s, no `fsync`) | 5–9 % |
| Other apps, Magisk | about 15 % |
| f2fs overhead | about 9 % |

The two app items are being fixed in the app. The kernel part: make sure unsynced writes are batched, and don't add writes of its own.

### Changes

- [ ] **Writeback batching.** Kernel defaults are already `dirty_writeback_interval = 500` and `dirty_expire_interval = 3000` (`mm/page-writeback.c`). Android's `init.rc` (in the boot ramdisk) overrides it: `write /proc/sys/vm/dirty_expire_centisecs 200`. Since we rebuild the boot image anyway, change the ramdisk `init.rc` to:
  ```
  write /proc/sys/vm/dirty_expire_centisecs 3000
  write /proc/sys/vm/dirty_writeback_centisecs 3000
  ```
  Effect: unsynced writes (`FlashCfg.cg`, logs, app files) go out in batches about every 30 s instead of every 2–5 s, and the flusher wakes the CPU less often. Data that is `fsync`ed (SQLite commits) is unaffected. Risk: a hard power cut or kernel panic loses up to about 60 s of unsynced data; a normal shutdown, including the low-battery one, syncs. Currently applied from userspace on both watches (Magisk module `dirty_expire` on the DW100U, `/system/etc/init/dirty_expire.rc` on the DM63); move it into the boot image and remove those.
- [ ] **f2fs online discard.** `/data` is mounted with `discard` (from the vendor fstab in the ramdisk). Online discard issues a trim for every freed block; Android also runs `fstrim` during idle maintenance. Test removing `discard` from the fstab and relying on idle `fstrim`: fewer small discard commands during use.
- [ ] **f2fs background GC.** `background_gc=on`. Check the GC thread's sleep times (`/sys/fs/f2fs/<dev>/gc_min_sleep_time`, `gc_max_sleep_time`, `gc_no_gc_sleep_time`) and raise them if GC runs often while the watch is in use: GC moves valid blocks, which is extra writes. Measure first (`/sys/kernel/debug/f2fs/status`).
- [ ] **No kernel writes to flash.** Check that `pstore`/`ramoops`, `last_kmsg` and UNISOC `sysdump` (`CONFIG_SPRD_SYSDUMP`) only write on a crash, not periodically. Keep kernel logging at `loglevel=1` (stock).
- [ ] Keep `lazytime` and `noatime` on `/data` (stock): no change.
- [ ] I/O scheduler: `cfq` is the stock default. `noop` or `deadline` costs less CPU on eMMC; no effect on the amount written. Low priority.

## 3. Battery, general

- [ ] **Suspend failures.** Stock counters since boot: DW100U `success 1209, fail 832, failed_freeze 285`; DM63 `success 1456, fail 559, failed_freeze 177`. Far too many failures: each one is a full suspend attempt wasted. Find the cause: `/d/suspend_stats` (`last_failed_dev`, `last_failed_errno`, `last_failed_step`), `/sys/kernel/wakeup_reasons/last_resume_reason`, and the dmesg around a failure (`Freezing of tasks failed`, `PM: Device ... failed to suspend`). Fix the driver or task that refuses.
- [ ] **Wakeup sources audit.** Rank `/d/wakeup_sources` by `total_time` and `wakeup_count` with GPS on and off. Known candidates: `sbuf-*-rx` (item 1), `sdiohal_rx_wakelock`, Wi-Fi (`sprdwl_ng`), the Bluetooth `rx_wakelock` (`marlin_bt_lpm.c`, `HZ*5` per receive), the FM wakelock (`HZ*1`). Shorten the holds that only cover "wait until userspace reads" to 20–50 ms, as for sbuf.
- [ ] **CPU frequency.** Stock governor `interactive` (768–1400 MHz) plus the UNISOC hotplug governor (`/sys/devices/system/cpu/cpuhotplug`: core limits, up/down thresholds). For a watch with GPS on and screen off: keep min 768 MHz; test lower `hispeed_freq` / higher `go_hispeed_load`, and a hotplug core limit of 1–2 cores while the screen is off. Measure each change; also check `schedutil` availability in 4.4.147 (needs `CONFIG_CPU_FREQ_GOV_SCHEDUTIL`, but it isn't in the stock config).
- [ ] **Tick.** Keep `CONFIG_HZ=100` and `NO_HZ_IDLE` (stock). `NO_HZ_FULL` isn't worth it here.
- [ ] **CPU idle.** States `WFI`, `core_pd`, `cluster_pd` exist. Check how often `cluster_pd` is reached with the screen off (`/sys/devices/system/cpu/cpu*/cpuidle/state*/usage`); if rarely, find what keeps a core busy.
- [ ] **Debug overhead.** `DEBUG_FS`, `FTRACE` and `KALLSYMS_ALL` are on in stock. They cost little at runtime; keep them for now (debugfs is needed for these measurements), revisit for a release build.
- [ ] Display/AOD: the DM63 uses a custom `aod_idle.ko` for always-on display idling; keep its behaviour (rebuild or merge into the panel/ADF driver).

## 4. Charging (battery health, not energy)

- [ ] DM63: the custom `batt_idle.ko` lowers the SC2721 charger CV voltage (stock 4480 mV) for battery longevity. Consider moving it into the charger driver as a sysfs knob, so no out-of-tree module is needed.
- [ ] Per-unit battery profiles live in each unit's DT (see the specs). Never mix device trees between the two DW100U units.

## How to measure (same procedure before and after every change)

Run on battery, not USB (USB power keeps the CPU awake). Push a small script that logs every 10 s to `/data/local/tmp`, unplug, run 15–30 minutes, plug back in and read:

- `/sys/class/power_supply/battery/current_now`, `voltage_now`, `capacity`
- `/d/suspend_stats` (success / fail counts)
- `/d/wakeup_sources` (`total_time` of the top sources, especially `sbuf-*-rx`)
- `/sys/block/mmcblk0/mmcblk0p<data>/stat` field 7 (sectors written) for flash writes
- GPS: fix rate (1 Hz) and satellites, without logging coordinates

Compare runs with the same app state (tracking on, same request interval), screen off, same place.
