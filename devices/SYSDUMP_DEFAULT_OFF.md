# sysdump default-off (4.14)

See the 4.4 note for the hardware observation (DM63: a panic with
`sysdump_status = 1` leaves the watch stuck at the boot logo and loses the
pstore console record; after `off` it reboots in 20 s and pstore keeps its
records).

## 4.14 differs from 4.4

* One file, `drivers/soc/sprd/debug/sysdump/sysdump.c`; there is no
  `sprd_pmic_wdt.c` reboot-mode code and no `reboot_mode_flag`.
* `set_sysdump_enable()` read-modify-writes `HWRST_STATUS_SYSDUMP` (0x200) in
  the PMIC reset-status register (via regmap) and sets `sysdump_status`.
* `sysdump_sysctl_init()` (`late_initcall_sync`) enabled the dump by default
  when `CONFIG_SPRD_DEBUG=y` (sprd_sharkle_defconfig sets it).
* `sysdump_enable_watchdog()` is a no-op here (`CONFIG_SPRD_WATCHDOG_SYS` has no
  driver in this tree).

## Change

The `CONFIG_SPRD_DEBUG` default-on is replaced by the boot state: `off` (bit
cleared) unless the command line has `sysdump=on` (new `early_param`,
`sysdump=off` also accepted). `/proc/sprd_sysdump` on/off is unchanged and the
panic path is untouched. The init already runs after the PMIC regmap is
looked up (`sprd_sysdump_enable_prepare()`); with no regmap the call logs and
does nothing.

`set_sysdump_enable()` returns the regmap read/write error and updates
`sysdump_status` only after the register write succeeded (a failure is logged
and the status keeps its old value; the watchdog is not touched then). The
whole transition (PMIC bit, status, watchdog) runs under one mutex,
`sysdump_state_lock`, shared by the boot hook and the `/proc/sprd_sysdump`
write; the panic path never takes it.

## Not verified

Compile-tested only; no 4.14 device test of the reboot-mode path.
