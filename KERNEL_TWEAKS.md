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

**Status (branch `tweaks/battery-wakeups`, built, not run on hardware yet):**
- `sbuf-4-4-0` is the `spipe_gnss` DT node (`sttygnss0`: dst 4 = `SIPC_ID_GNSS`, channel 4, 1 ring), created by `spipe_probe()`. Done: per-ring `rx_wake_timeout` (default `HZ / 2`) and `rx_release_on_read` (default off), set with `sbuf_set_rx_wakelock_timeout()`; other rings are unchanged. `spipe` sets 100 ms max hold plus release-on-read for `dst == SIPC_ID_GNSS`: once the ring is drained, `sbuf_read()` drops the hold and re-arms 20 ms, so the lock ends about 20 ms after the HAL read; while unread data is left the hold stays. A per-ring spinlock makes the drained check and the release atomic against `sbuf_thread()`, which arms the hold before it wakes the readers. Correction: `wake_lock_timeout()` only ever extends a running timeout, so the earlier assumption that the read path already shortened the `HZ / 2` hold was wrong (it never did). The hold must be long enough for the HAL reader to run: if it expires before the read, unread data sits in the ring and, if the CP only signals on empty -> non-empty, no further notification arrives, so fixes are delayed and eventually lost. A code hook, not a DT property, because the watches boot the vendor dtb. **Required on hardware before shipping:** 1 Hz fixes continuous with the CPU suspending, `sbuf-4-4-0-rx` `total_time` before/after, and no gaps in the fix timestamps.
- Wake IRQ: nothing to fix. The SIPC notification arrives through the SPRD mailbox (`CONFIG_SPRD_MAILBOX=y`), whose inbox/outbox IRQs use `IRQF_NO_SUSPEND` plus `enable_irq_wake()` (`sprd_mailbox.c`). `smsg-4-4` also takes a `HZ / 2` lock per message, but `sbuf_thread` releases it right after handling (`smsg_ch_wake_unlock`). Remaining: confirm on hardware that fixes keep arriving at 1 Hz with the CPU suspending.

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
  **Status (investigated read-only on both watches, both on USB at the time):** `failed_suspend` (366 / 375) is `alarmtimer` returning `-EBUSY` (`last_failed_dev alarmtimer`, errno -16): upstream refuses to suspend when an RTC alarm is due within 2 s and holds a 2 s wakeup event; the `alarmtimer` source shows 350 activations of max 2008 ms on the DW100U. Cause is userspace alarms (AlarmManager) due every few seconds; not a driver bug. `failed_freeze` (285 / 194) is `-EBUSY` from a pending wakeup event during the freeze window (no task refused to freeze; no "Freezing of tasks failed" lines in dmesg, which only holds recent log). The sources with by far the most events: `sbuf-4-4-0-rx` (wakeup_count 1.03 M DW100U, 517 k DM63), then `sbuf-3-4-10-rx` (BT), `smsg-3-8`, `sbuf-5-6-*`. Also self-inflicted: `pm_message_wakelock` (debug thread in `pm-sharkle.c`, 100 ms every 30 s), now off by default (`/d/power/print_thread_enable`; enabling takes effect within `print_thread_interval`, 30 s). USB: `musb-sprd` is active for the whole USB connection (e.g. 31 h on the DM63), so no suspend at all while plugged in. Remaining: re-measure `/d/suspend_stats` deltas on battery after the sbuf changes; `failed_suspend_noirq: 1` on the DM63 is unexplained (single event).
- [ ] **Wakeup sources audit.** Rank `/d/wakeup_sources` by `total_time` and `wakeup_count` with GPS on and off. Known candidates: `sbuf-*-rx` (item 1), `sdiohal_rx_wakelock`, Wi-Fi (`sprdwl_ng`), the Bluetooth `rx_wakelock` (`marlin_bt_lpm.c`, `HZ*5` per receive), the FM wakelock (`HZ*1`). Shorten the holds that only cover "wait until userspace reads" to 20–50 ms, as for sbuf.
  **Status:** BT and FM here are not the `sc2342/bluetooth` and `radio` drivers (`CONFIG_MARLIN2_BT_SDIO` / `CONFIG_RADIO_SC2332` are not set; `marlin_bt_lpm.c` and `fmdrv_main.c` are not compiled). The modules are `sprdbt_tty.ko` (`sipc_modules/bluetooth/driver/tty-sipc`) and `sprd_fm.ko` (`fm_sipc`), outside the kernel image build. Done: BT rx ring (`sbuf-3-4-10-rx`, held 4.7 h on the DW100U) now 200 ms instead of 500 ms, a plain shorter hold (no release-on-read: `tty-sipc` reads inside the notifier, before the hold is armed). `sprdbt_tty.ko` looks the setter up with `symbol_get()`, so it still loads on a kernel without it (it then keeps `HZ / 2`); ship kernel and modules together anyway. Kept: `BT_TX_wakelock` (BT stack handshake via `/proc/bluetooth/sleep/btwrite`), the `lpm.c` `rx_wakelock` (never taken by tty-sipc). FM (`fm_handler`, `HZ*1`): left, since it only runs while the radio is on and the lock also covers command responses; revisit if FM is used. Not yet audited: `sbuf-3-11-0-rx`, `smsg-3-7/3-8` (WCN), `sbuf-5-6-*` (modem), `sprdwl_*`, `charger_wake_lock` (5838 activations, 4.9 h).
- [ ] **CPU frequency.** Stock governor `interactive` (768–1400 MHz) plus the UNISOC hotplug governor (`/sys/devices/system/cpu/cpuhotplug`: core limits, up/down thresholds). For a watch with GPS on and screen off: keep min 768 MHz; test lower `hispeed_freq` / higher `go_hispeed_load`, and a hotplug core limit of 1–2 cores while the screen is off. Measure each change; also check `schedutil` availability in 4.4.147 (needs `CONFIG_CPU_FREQ_GOV_SCHEDUTIL`, but it isn't in the stock config).
- [ ] **Tick.** Keep `CONFIG_HZ=100` and `NO_HZ_IDLE` (stock). `NO_HZ_FULL` isn't worth it here.
- [ ] **CPU idle.** States `WFI`, `core_pd`, `cluster_pd` exist. Check how often `cluster_pd` is reached with the screen off (`/sys/devices/system/cpu/cpu*/cpuidle/state*/usage`); if rarely, find what keeps a core busy.
- [ ] **Debug overhead.** `DEBUG_FS`, `FTRACE` and `KALLSYMS_ALL` are on in stock. They cost little at runtime; keep them for now (debugfs is needed for these measurements), revisit for a release build.
- [~] Display/AOD: the DM63 uses a custom `aod_idle.ko` for always-on display idling; keep its behaviour (rebuild or merge into the panel/ADF driver).
  - **Done in tree, untested on hardware:** `CONFIG_SPRD_AOD_IDLE` (tristate, default n) = `drivers/video/adf/sprd/drv/sprd_aod_idle.c`, enabled by `arch/arm64/configs/dm63_aod.fragment` (also needs `dm63_touch.fragment`). Same ABI as the stock module: `/sys/kernel/aod_idle/{idle,idle_brightness,dtap,dtap_ms,status,peek}` and input device `aod_dtap` (`KEY_WAKEUP`).
  - **What it does:** `idle=1` sends generic writes `fe 00` then `39` (Idle Mode on) to the CO5300, then DCS `51 <idle_brightness>` (default 8; 0 = keep the framework doze level), then reads power mode `0x0a` (`0x9c` normal, `0xdc` idle). `idle=0` sends `fe 00`, `38`, then resends the `sprd_backlight` level. While idle, an input filter on `cst0xx_ts` swallows all touches; two touch-downs within `dtap_ms` (default 400) emit `KEY_WAKEUP` from `aod_dtap`, and a 600 ms wakeup source keeps the CPU up for the second tap. Commands are refused (`-EIO`) unless the panel and DSI platform devices are runtime active; both are pinned for the transaction and the backlight `update_lock` is held.
  - **Differences from the stock module:** no version check, offsets or kallsyms. `sprd_panel.c` now exports `sprd_panel_get()` and `sprd_panel_oled_backlight()`; `mipi_dsi_api.c` exports `mipi_dsi_gen_write/dcs_read/set_max_return_size/lp_cmd_enable` (all `EXPORT_SYMBOL_GPL`). Module name is `sprd_aod_idle.ko` (stock: `aod_idle.ko`), so `aod_idle.rc` must be adapted for `=m`.
  - **Open:** (1) the stock module chose DCS or generic write from a panel-info flag at offset 0x08; that field does not exist in this tree, whose CO5300 init/sleep/brightness sequences are all generic writes, so generic is used; verify `status` shows `dcs_0x0a=0xdc(idle)` after `idle=1`. If not, switch `send()` to `mipi_dsi_dcs_write`. (2) The AOD app/HAL side (who writes `idle`, `aod_idled.sh`) was not inspected here. (3) DW100U (`hyn_ts`, also CO5300) is not matched; only `cst0xx_ts`.
  - **Test on hardware (DM63, battery, no USB):** `cat /sys/kernel/aod_idle/status` (expect `panel_powered=1`, `dtap=1`); `echo 1 > .../idle`, status shows `dcs_0x0a=0xdc(idle)` and the panel dims to the AOD level; `echo 8 > .../idle_brightness` changes level live; with idle on, double tap quickly: `taps`/`dtaps` increase and `getevent -l` on `aod_dtap` shows `KEY_WAKEUP` down/up, a single tap or taps >400 ms apart do nothing, and no touch reaches Android; `echo 0 > .../idle` restores the normal level and touch; `echo 0 > .../dtap` passes touches through. Check no `dsi`/`panel` errors in `dmesg` across screen off/on with idle on, and that `refused` stays 0 outside suspend. Idle power: follow "How to measure" with screen in AOD for 15-30 min, compare `current_now` with `idle=0` doze and with the screen off.

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
