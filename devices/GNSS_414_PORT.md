# GNSS / WCN fail-path port to 4.14

Source: 4.4 tree (`android_kernel_sprd_sc9832e`), GNSS sections of its `KERNEL_TWEAKS.md`.
Target: this branch (`kernel-4.14-gnss`). Compile-only port, nothing here has run on a device.

4.14 layout (the 4.4 `sprdwcn_bsp/sc23xx` driver became `sprdwcn`, same SC2342_INTEG flow):

| 4.4 | 4.14 |
|---|---|
| `drivers/misc/sprdwcn_bsp/sc23xx/platform/gnss/gnss_common_ctl.c` | `drivers/misc/sprdwcn/platform/gnss/gnss_common_ctl.c` |
| `.../sc23xx/boot/wcn_integrate.c`, `wcn_integrate_boot.c`, `wcn_integrate_dev.h` | `drivers/misc/sprdwcn/boot/` (same names) |
| `drivers/staging/sprd/modem/sipc/sbuf.c` | `drivers/soc/sprd/modem/sipc/sbuf.c` |
| `.../sipc/stty.c`, `stty_gnss.c` | do not exist |

Order below is the order the port is committed in (c29f325 and cc53423 depend on a15acda, 2cf0ee6 on cc53423).

## Verdicts

| 4.4 commit | 4.4 change | 4.14 equivalent | Verdict | Why |
|---|---|---|---|---|
| 7addf01635fc | add read-only `gnss_engine_state`; split `gnss_cp_status_read()` out of `gnss_status_get()` (checks the DDR read result) | `gnss_common_ctl.c`: `gnss_status_get()`, attribute list `gnss_common_ctl_attrs[]` | PORT | no attribute in 4.14; `gnss_status_get()` ignores the read error (value initialised to 0 so it reports "on", not "unknown") |
| c63032761465 | `error` for the GOING states when boot status is ERR_BOOT; rate limit the vmap failure print | `gnss_engine_state_show()` (new); `boot/wcn_integrate.c:wcn_read_data_from_phy_addr()` | PORT | `wcn_read_data_from_phy_addr()` has the same unconditional `WCN_ERR` and is hit by every sysfs read. Doc-only hunks (KERNEL_TWEAKS.md) are covered by this file |
| a325df9b0359 | qualify the `off` meaning (comment), rate limit the other two map error prints | `wcn_mem_ram_vmap()` (`malloc err`, `vmap err`) | PORT | same prints exist unchanged. 4.4 `WCN_HEADER_ERR` does not exist; 4.14 adds `WCN_ERR_RATELIMITED()` in `include/wcn_dbg.h` with the same ` error: ` prefix |
| a15acdab4f8a | `wcn_unwind_failed_start()`: release core/sys domain/vddcon after a failed GNSS start | `boot/wcn_integrate_boot.c:start_integrate_wcn_truely()` | PORT | same structure: `wcn_power_wq()` takes vddcon + sys domain, the three GNSS failure returns skip the release, `stop_integrate_wcn_truely()` then refuses with -EINVAL (not opened). `power_wq` delayed work, `wcn_proc_native_stop`, `wcn_power_enable_*`, `wcn_sys_soft_*`, `wcn_sys_deep_sleep_en` all exist |
| c29f325e7e78 | back to POWEROFF after a failed start, stop mapping for -EINVAL, `error` for POWEROFF + ERR_BOOT, clear stale ERR_BOOT on new start, dump hunk | `gnss_common_ctl.c:gnss_power_on()`, `gnss_engine_state_show()`, `gnss_dump_mem_ctrl_co()` | PORT (partial) | `gnss_power_on()` leaves POWERON_GOING / POWEROFF_GOING on failure, same bug. The "last start failed" shortcut and the ERR_BOOT-to-POWERON force in `gnss_dump_mem_ctrl_co()` do not exist in 4.14, so those hunks are dropped (nothing to remove; a POWEROFF core is not dumped because the dump condition needs POWERON_GOING/POWERON). The stop path in 4.14 has no `gnss_set_boot_status(OK)`, not added |
| cc53423f781e | start retry rate limit (module params `gnss_retry_min_s/burst/max_s`), keep ASSERT set by the dump path | `gnss_power_on()` | PORT | 4.14 has no limit; a retry holds `marlin_lock` for up to ~20 s. Same code location, params added with the same names, defaults and mode |
| 2cf0ee6276e0 | `wcn_power_enable_vddcon()` returns on regulator failure; `vddcon_taken`/`sys_domain_taken` in `struct wcn_device`; clamp retry params to 24 h; boottime clock | `boot/wcn_integrate.c:wcn_power_enable_vddcon()`, `wcn_integrate_boot.c:wcn_power_wq()`, `wcn_integrate_dev.h`, `gnss_power_on()` | PORT | all four spots are identical in shape. `ktime_get_boot_ns()` is present in 4.14 (`timekeeping.h`) |
| ae2d7951c6ff | sbuf: extend the GNSS rx hold 20 ms on a partial read | `soc/sprd/modem/sipc/sbuf.c:sbuf_read()` | PRESENT | 4.14 is stock: it has no `rx_release_on_read` / `rx_wl_lock` (our 4.4 tweak, which this commit refines) and already does `__pm_wakeup_event(rx_wake_lock, 20 ms)` on every read while `rx_wakelock_state` is set. That is the behaviour the commit says it restores "like stock" |
| e93bcf4a5d73 | sbuf: only extend while a notification is pending | same | PRESENT | the 20 ms extension in `sbuf_read()` is gated on `rx_wakelock_state`, which `sbuf_thread()` sets on `SMSG_EVENT_SBUF_WRPTR` and `sbuf_read()` clears |
| 51ce70030252 | `tty_schedule_flip()` to `tty_flip_buffer_push()` in stty/stty_gnss | none | N/A | neither file exists and no `tty_schedule_flip()` caller exists under `drivers/soc/sprd` or `drivers/misc/sprdwcn` |

Not in the requested list, noticed while porting: 4.4 `91f9efcfa2aa` ("wcn_power_enable_sys_domain: only power down on disable"). 4.14 has the same `else if` without `enable == false`. It is only reachable when the domain is marked on and nothing is open, which the unwind above no longer leaves behind, so it is not ported here.

## ABI

- `gnss_engine_state`: `/sys/class/misc/gnss_common_ctl/gnss_engine_state`, mode 0444, values `off transition assert error sleep on unknown`, same semantics as 4.4. The path is the same on 4.14 (misc device `gnss_common_ctl`, group registered on its kobject). Existing attributes unchanged.
- Module parameters `gnss_retry_min_s` (30), `gnss_retry_burst` (3), `gnss_retry_max_s` (600), mode 0644. In 4.14 the GNSS control code is linked into the composite `wcn_platform` object, so the sysfs path is `/sys/module/wcn_platform/parameters/<name>` (built in; kernel command line prefix `wcn_platform.`). This is the one path difference to 4.4, to be confirmed on a booted kernel.

## Needs a device

Everything: failed-start unwind (rail and domain counts, `set WCN SYS TOP PD`), retry after calibration failure with Wi-Fi/BT on and off, `sleep` reporting, suspend behaviour. Use the 4.4 hardware test steps 1-9 unchanged.

## Locking (review follow-up, 4.14 only)

The 4.4 versions of c29f325e7e78 and cc53423f781e have two races, which the 4.14 port fixes in a follow-up commit:

- `gnss_power_on()` did the state check, the cooldown check and the move to `POWERON_GOING` with no common lock, so two concurrent writers to `gnss_power_enable` could both pass and the second one would retry `start_marlin()` without seeing the cooldown set by the first failure. 4.14 now serializes the whole transaction with the mutex `gnss_ctl_lock` (held across `start_marlin()`/`stop_marlin()`, which sleep).
- "Keep ASSERT" was a check followed by an assignment, not atomic against `gnss_dump_mem_ctrl_co()` setting `ASSERT`. 4.14 protects every `gnss_status` write with the spinlock `gnss_status_lock` and uses conditional transitions (`gnss_status_move(from, to)`); the dump callback sets `ASSERT` through `gnss_status_set()`. A start that returns success also no longer overwrites an `ASSERT` set meanwhile. The dump callback does not take `gnss_ctl_lock`, because the dump can be triggered from inside `start_marlin()`/`stop_marlin()` and may sleep, so a spinlock only covers the state word.

4.4 TODO (not touched here): the same two races exist in the 4.4 tree.
