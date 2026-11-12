# sysdump default-off (4.4)

## Symptom (DM63, our 4.4 v8, hardware)

With `/proc/sprd_sysdump` showing `sysdump_status = 1`, `echo c >
/proc/sysrq-trigger` leaves the watch at the boot logo after the reboot (no
USB, needs a forced power-off) and the pstore console record is lost. After
`echo off > /proc/sprd_sysdump` (init.common.rc does this on
`debug.sysdump.enabled=false`) the same panic reboots in 20 s and pstore keeps
console-ramoops and dmesg-ramoops. The off state survived the reboot.

## Kernel evidence

* `sysdump.c` `sprd_sysdump_write()`: `on` -> `sysdump_status = 1`,
  `sprd_set_reboot_mode("dumpenable")`, `set_sysdump_enable(1)`,
  `sysdump_enable_watchdog(0)`; `off` does the opposite.
* `sprd_pmic_wdt.c`: `sprd_set_reboot_mode()` only edits the in-RAM
  `reboot_mode_flag` (`dumpenable` ORs `HWRST_STATUS_SYSDUMP` 0x200,
  `dumpdisable` clears it). `set_sysdump_enable()` read-modify-writes the same
  bit directly in the PMIC register `ANA_REG_GLB_POR_RST_MONITOR` over ADI.
  `arch_reset()` (and `psci_sys_reset()`, which passes `reboot_mode_flag` to
  firmware) writes `reboot_mode_flag` to that register on every restart;
  `sprd_restart_prepare()` ORs the mode of the reboot command into it.
* `sysdump_enter()` (called from `panic()`) runs regardless of the status: it
  fills the sysdump info block (magic `SPRD_SYSDUMP_119`, cpu regs, reason),
  then `machine_restart("panic")`, i.e. `HWRST_STATUS_PANIC` 0x80 is written
  with or without the 0x200 bit.
* `sprd_reboot_probe()` sets `sysdump_status = 1` and the 0x200 bit when
  `CONFIG_SPRD_DEBUG` and `CONFIG_SPRD_SYSDUMP` are both set (dm63_defconfig
  sets both).
* The 0x200 bit lives in the PMIC, a power domain that the SoC watchdog reset
  does not clear, so it is what the next boot stage sees. Bootloader behaviour
  is inferred (no bootloader source here): panic (0x80) + 0x200 -> dump mode,
  which waits for a host (the logo hang); panic without 0x200 -> normal boot.
  `sysdump_status` itself is plain kernel RAM and is not what "survives": after
  a reboot it is re-derived (probe default, or a later userspace write; the
  stock init.common.rc only writes on `debug.sysdump.enabled`, and mirrors the
  result into `persist.sys.sysdump`, which nothing in the boot ramdisk reads
  back).

## Change

`sysdump.c` applies the stock `off` path once from `late_initcall_sync`
(`sysdump_set_state()`, shared with `/proc/sprd_sysdump`), unless the command
line has `sysdump=on` (then the stock `on` path is applied instead;
`sysdump=off` is the default). The panic path is untouched.
Lab use: add `sysdump=on` to the kernel command line, or write `on` at runtime.
A userspace write of `on` at boot (`debug.sysdump.enabled=true`) still wins.

`sysdump=on` (like the stock runtime `on` write) also stops the SoC watchdog
(`sysdump_enable_watchdog(0)`); `off` starts it again.

Drivers are not assumed to have probed: `late_initcall_sync` runs after the
device_initcall probes only if they bound, which is checked, not guaranteed.

* PMIC watchdog (`sprd_pmic_wdt_ready()`) not probed: nothing is applied
  (`sysdump_status`, reboot flag, PMIC dump bit and SoC watchdog stay as they
  were) and an error line says so.
* SoC watchdog (`sprd_wdt_sys`) not probed: the PMIC bit, reboot flag and
  `sysdump_status` are applied, the watchdog start/stop is not; a warning and
  an error line say so.
* PMIC register read/write (ADI) failure: `set_sysdump_enable()` returns the
  error, `sysdump_status`, reboot flag and watchdog are not touched, the failure
  is logged.

The whole transition runs under one mutex, `sysdump_state_lock`, shared by the
boot hook and the `/proc` write (sleeping context only; the panic path never
takes it). The probe-time default (`CONFIG_SPRD_DEBUG`) runs before either and
is unlocked.

## Not verified

No device test of this change. Expected on hardware: `sysdump_status = 0` right
after boot, panic reboots by itself and pstore keeps its records.

## Hardware result (DM63, v9, 2026-11-12)

On the DM63 ROM the vendor daemon `/vendor/bin/systemDebuggerd` (oneshot service in `init.common.rc`) applies
`persist.sys.sysdump` to `/proc/sprd_sysdump` at every boot, after this kernel default; with the property unset it
turns the dump **on**. So on these watches the property decides: v9 booted with `persist.sys.sysdump=on` reported
`sysdump_status = 1`. The kernel default only covers the time before the daemon runs and ROMs without it.

Operational fix, once per watch (stock or our kernel): `setprop debug.sysdump.enabled false` (init writes `off`
and stores `persist.sys.sysdump=off`; the daemon re-applies it at boot). Verified on v9: `sysdump_status = 0`, a
`sysrq` panic rebooted the watch by itself in 40 s and pstore kept console and dmesg records. With `on`, the same
panic left the watch at the boot logo until a forced power-off (bench DM63, v8).
