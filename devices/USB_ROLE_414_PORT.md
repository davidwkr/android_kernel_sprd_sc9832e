# USB host/device role switch on 4.14

**Compile-only. Nothing here has run on a watch.**

Ports the userspace role switch and host-mode idle timeout from the 4.4 product tree
(`drivers/usb/musb/musb_sprd.c`, `security-backports` branch, commits `4ce221b95851` through
`1ce2ccd78951`) onto the 4.14 SPRD MUSB glue (`drivers/usb/musb/musb_sprd.c`), behind a new
`CONFIG_USB_MUSB_SPRD_ROLE_SYSFS` (off by default, `drivers/usb/musb/Kconfig`, depends on
`USB_MUSB_SPRD && USB_MUSB_DUAL_ROLE`; both already on in `sprd_sharkle_defconfig`). Opt-in through
`watch_414_otg.fragment` (see Config).

## Sysfs interface (unchanged from 4.4)

Two files with the same names, values and semantics as the 4.4 port and its predecessor, the
out-of-tree `otg_role.ko`:

- `/sys/kernel/otg_role/role`
- `role` on the glue device (`/sys/devices/.../20200000.usb/role`)

Mode 0644 (write root only). Write `host` or `device` (trailing newline fine, anything else
`-EINVAL`); read gives `host (current=1 next=1)`, `device (current=2 next=2)`, `idle (current=0
next=...)`. A helper sets `vbus_active`/`wq_mode` under `glue->lock` and `glue->role_lock` (mutex)
and waits for `glue->work` to finish, so the write returns after the switch:

- `-EOPNOTSUPP`: `host` with no VBUS source known and `host_without_vbus` not set. With
  `host_without_vbus=1` and no `vbus-supply`, the switch now actually goes through (the worker no
  longer requires a regulator it was never told to expect); VBUS must be driven externally.
- `-EBUSY`: a device-mode cable is attached, VBUS is already present, or a switch is already
  running. `host` while already in host mode is **not** `-EBUSY`: it returns 0 and rearms the idle
  timeout (see `host_idle_timeout` below) — this is how a caller keeps host mode alive past the
  timeout without a "device"/"host" bounce.
- `-EIO`: the switch did not verify (controller not in host mode / not runtime-active, or the
  device teardown did not leave the controller suspended).
- `device` when not in host mode is a no-op (returns 0). Removing the glue switches back to
  device mode first.
- An ID-GPIO cable event (`musb_sprd_id_notifier()`) arriving while a sysfs switch is in progress
  is deferred (recorded, not applied) and replayed once the switch finishes, instead of racing the
  switch's own VBUS/`glue->work` handling.

Worst case a write blocks for a minute or more (the work item waits for the controller to
suspend/resume); a normal switch takes about a second.

Module parameters, same as 4.4:

- `host_idle_timeout` (seconds, default 300, 0 = off): back to device mode when no USB device is
  enumerated below the root hub for this long after a successful `host`. Armed on host entry and on
  the last device's removal, cancelled on the first device's add; a failed idle switch is latched
  (no further idle switches until the next explicit `host`/`device` write).
- `host_without_vbus` (bool, default 0): allow `host` although no VBUS source is known. Authorises
  the sysfs gate *and* the worker's own regulator lookup, so this is the genuine supply-less path,
  not just a gate bypass that then fails.

## Mapping onto the 4.14 glue

The 4.14 driver is not the 4.4 one with a few functions renamed: it replaced GPIO cable detection,
the USB-ID irq and the charger-IC VBUS boost with extcon (`glue->edev`/`glue->id_edev`,
`musb_sprd_vbus_notifier()`/`musb_sprd_id_notifier()`) and a VBUS **regulator** (`glue->vbus`,
`devm_regulator_get(dev, "vbus")`). The role/idle-timeout code is adapted to those, not pasted:

| 4.4 mechanism | 4.14 equivalent used here |
|---|---|
| `gpio_otg` / `sprd_extic_otg_power()` (`musb_sprd_set_otg_en()`) | `regulator_enable()`/`regulator_disable(glue->vbus)` (new `musb_sprd_set_otg_en()`, same name and call sites) |
| `gpio_is_valid(gpio_otg) \|\| sprd_extic_otg_supported()` (VBUS-source gate) | `glue->vbus != NULL` |
| `gpio_get_value(gpio_device)` (cable/VBUS present check, used for the `-EBUSY` gate and the post-host re-detection poll) | `extcon_get_state(glue->edev, EXTCON_USB)` |
| `hot_plug_nb` / `CABLE_STATUS_HOST_CONN`/`HOST_DISCONN` (what the role write mimics) | `musb_sprd_id_notifier()` (`EXTCON_USB_HOST`) — the role write sets `vbus_active`/`wq_mode` and queues `glue->work` directly, same as 4.4, bypassing the notifier itself |
| `wake_lock`/`wake_lock_active()`/`wake_unlock()` | `struct wakeup_source glue->wake_lock` (already in the 4.14 glue), `.active`, `__pm_stay_awake()`/`__pm_relax()` |
| vbus irq disabled around the post-"device" VBUS re-poll | dropped: 4.14's `glue->vbus_irq` is set to `-1` in `musb_sprd_probe()` and never assigned a real IRQ (VBUS is extcon-driven), so there is nothing to disable; the poll just rereads the extcon state |
| `!musb->gadget_driver` → `-EAGAIN` (host write would otherwise block the work item forever) | dropped: 4.14's `sprd_musb_work()` already calls `musb_host_start()` itself when entering host mode with no gadget driver bound, so the write cannot get stuck on this |

`glue->dr_mode`, `glue->wq_mode`, `glue->vbus_active`, `glue->charging_mode`, `glue->recover_work`
and the `schedule_work(&glue->work)` / `flush_work(&glue->work)` protocol are unchanged field names
and semantics between the two trees, so `__sprd_musb_role_set()`, `sprd_musb_idle_work()` and the
`USB_DEVICE_ADD`/`REMOVE` notifier (`sprd_musb_usb_notify()`) port essentially line for line.

**Host success verification differs from 4.4** because of `glue->power_always_on` (a 4.14-only
`wakeup-source` DT flag with no 4.4 counterpart): when set, `sprd_musb_work()` does *not* take
`glue->wake_lock` in host mode, so checking `wake_lock.active` alone would misreport a successful
switch as failed. The verification is `dr_mode == HOST && (wake_lock.active || power_always_on) &&
!pm_runtime_suspended()`.

No board in this tree gives the musb node a `vbus-supply`, so `glue->vbus` is `NULL` everywhere
(confirmed: `REGULATOR_DUMMY` is not set and no DTS has the property) — the same "no VBUS source on
any board" conclusion the 4.4 review reached for the charger-IC path, just via the new regulator
API. `host` is refused with `-EOPNOTSUPP` unless `host_without_vbus=1`, exactly as in 4.4.

## Carried-over 4.4 review fixes

- `pm_runtime_get_sync()` returns 1 when already active, not an error; only a negative value is.
  4.14's `musb_sprd_resume_child()` had the same pre-existing bug as the 4.4 base (`if (ret)`
  instead of `if (ret < 0)`, no `pm_runtime_put_noidle()` on the real failure path) — fixed here the
  same way 4.4 commit `1ba17ca077f6` fixed it, independent of the role switch itself.
  `musb_sprd_resume_child()` is shared, unconditional code: this fix applies regardless of whether
  `CONFIG_USB_MUSB_SPRD_ROLE_SYSFS` is on or off.
- `recover_work` (host-controller recovery) is blocked by `role_busy` for the duration of a role
  change, both in `sprd_musb_recover()` (the `.recover` op) and in `sprd_musb_recover_work()` itself
  (checked again after its `msleep(300)`), so it cannot restart host mode mid-switch or race the
  rollback path. `musb_sprd_id_notifier()` now honours `role_busy` the same way: a cable event
  during a switch is recorded (`glue->id_pending`/`id_pending_connect`) instead of being applied
  immediately, and is replayed through the shared `musb_sprd_id_handle()` once the switch's `done:`
  path clears `role_busy` — closing a window where a host-shutdown in progress could have its VBUS
  disable race a host-start the event had just queued.
- The VBUS regulator is now only ever enabled/disabled once per transition: `glue->vbus_on` tracks
  whether the role code or `sprd_musb_work()` itself currently holds it enabled, so the sysfs path's
  safety-net disable (`musb_sprd_set_otg_en()`, for the early-exit cases the work item's own
  disable skips) no longer double-disables a regulator the work item already turned off cleanly.
- The already-host check in `__sprd_musb_role_set()` no longer also requires `wq_mode == HOST`
  (the worker always resets `wq_mode` to `UNKNOWN` once it dequeues a request, so that condition
  was practically never true): a `host` write while already in host mode now reliably returns 0 and
  rearms `host_idle_timeout`, instead of falling through to `-EBUSY` and leaving a latched idle
  failure stuck.
- Init order (`sprd_musb_role_init()` after `pm_runtime_enable()`), removal order
  (`sprd_musb_role_exit()` first in `musb_sprd_remove()`, leaves host mode without cable
  re-detection, drains `idle_work`/`recover_work`/`glue->work` before the child device is
  destroyed), and the VBUS-gate wording (applies to the manual sysfs `host` write only; the
  automatic extcon ID path is left alone) all carry over from 4.4 commits `d4f58c33975d`,
  `1ba17ca077f6`, `b0cb34d0b633`, `14fe1d36eed8` and `1ce2ccd78951`. The `role` sysfs attribute is
  published only by `sprd_musb_role_init()` (not also listed in the static `musb_sprd_attrs` group),
  so it cannot be created — and so become writable — before `role_lock`/`idle_work` are initialised,
  and it is created exactly once.

## Known gap (4.14-specific, not in the ported commits)

`musb_sprd_remove()` never unregisters `glue->vbus_nb`/`glue->id_nb` (the extcon notifiers
registered in `musb_sprd_probe()`) on the normal removal path — this is a pre-existing gap in the
4.14 driver, not introduced by this port. `sprd_musb_role_exit()` only stops the role-switch's own
notifier (`sprd_musb_usb_nb`) and work items before draining `glue->work`; a `vbus_nb`/`id_nb`
callback firing during module removal could still queue `glue->work` afterwards. 4.4's equivalent
teardown could disable real GPIO IRQs and unregister a single blocking notifier synchronously
before its own final drain; there is no equivalent synchronous stop for extcon's blocking notifier
chain exposed here beyond `extcon_unregister_notifier()`, which this port does not add since fixing
it is outside the role-switch/idle-timeout scope of this change.

## Config

`CONFIG_USB_MUSB_SPRD_ROLE_SYSFS=y` is in the opt-in `watch_414_otg.fragment`, merged on top of
`watch_414.fragment` only when wanted, as on 4.4: there `watch_otg.fragment` was not merged into any
device build (several device fragments say it is deliberately not enabled), and stock has no such
switch. Even when enabled, `host` is refused on every board unless `host_without_vbus` is set.

## Build

Compiles clean (`sprd_sharkle_defconfig` + `watch_414.fragment` + `watch_414_adf.fragment` + the option on,
`ARCH=arm64`), no new warnings in `drivers/usb/musb/musb_sprd.c` or `Kconfig`.
