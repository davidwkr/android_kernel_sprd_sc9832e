# Stock config parity

What `devices/<dev>/stock.config` (4.4.83) enables (`=y` or `=m`) that our per-watch builds
(`scripts/watch_build.sh`, before `watch_stock_parity.fragment`) did not, checked for DM63,
DW100U silver, MT107, MT108 and DM82 (K18: its stock config is the MT107's plus `HEARTRATE_BD1820` and `INPUT_SENSORS_BD1XXX`, both without source here, so the same classes apply; its `INPUT_GPIO_ROTARY_ENCODER` is inert, the DT has no rotary node). Evidence: the stock DTs (`devices/*/stock.dts`), the stock
boot images (ramdisk unpacked from read-only copies with `sprd_bootimg.py`), the driver sources
and Kconfig of this tree, `kernel/configs/android-*.config`, and `arch/arm64/configs/lineageos_dw99_defconfig`
(a sibling arm64 config of this tree that uses the same WCN stack). Nothing here was run on a watch.

Counts of stock `=y/m` options not set in our old build: DM63 122, DW100U 122, MT107 119, MT108 122,
DM82 119. Of those 113 distinct symbols exist in this tree's Kconfig (109 to 112 per device) and 14
distinct ones do not (9 to 12 per device).

Classes: **A** needed for boot or function (enabled in `watch_stock_parity.fragment` or a device
fragment), **B** harmless or optional (left off), **C** conflicts with a deliberate choice (left off).
The ramdisk of both stock images is gzip-compressed and has no `.rc` that selects a governor,
loads i2c-dev, or uses message queues; `/system` and `/vendor` were not available, so userspace
use of an option is "not seen in the ramdisk", not "proven unused".

## A: enabled

| Option | Devices | Reason |
|---|---|---|
| `TRUSTY`, `TRUSTY_VIRTIO`, `TRUSTY_VIRTIO_IPC` (`TRUSTY_LOG` defaults to y) | all | All five stock DTs have `android,trusty-smc-v1` with irq, virtio and log children; the stock ramdisk has `/dev/trusty-ipc-dev0` in ueventd and `/data` is `encryptable=footer` (keymaster/gatekeeper). Without Trusty the Android 8.1 HALs fall back to software keymaster; keys wrapped by the TEE on a stock install could not be unwrapped. Risk: untested on our kernel. If the secure world answers `SM_ERR_UNDEFINED_SMC`, the probe continues with api_version 0 and the children (irq, virtio, log, ipc) fail later instead of failing the probe, so look for Trusty errors in the first-boot dmesg.  |
| `TRUSTY_FIQ`, `TRUSTY_FIQ_ARM64` | none | Not enabled: no `trusty-fiq` node in any DT, `FIQ_GLUE` exists only under `arch/arm`, and `TRUSTY_FIQ_ARM64` has no prompt (cannot be set from a config; the stock value is a leftover). |
| `INPUT_GPIO_ROTARY_ENCODER` | all (used on DM63, MT107, MT108, DM82) | `rotary@0` / `rotary-encoder` (gpios 78 and 92, relative axis) is the crown. Probes only when the node exists, so it is inert on the DW100U (no node). |
| `SPRD_WATCHDOG_SYS` | all | `watchdog@40310000` / `sprd,sharkl2-wdt` is in every DT and matches `drivers/watchdog/sprd_wdt_sys.c`, which arms the AP watchdog at probe and feeds it from a kthread every 3 s (panic IRQ at 9 s, reset at 12 s, stopped in suspend, writes the watchdog reboot reason for the bootloader). The kernel is the only feeder, so without it a hung kernel is never reset. `SPRD_PMIC_WATCHDOG` (already on) is a different watchdog. Stock and `lineageos_dw99_defconfig` have it. Risk: the 9 s / 12 s window could reset a unit that stalls with interrupts off (long flash or GNSS dumps); check on the first boot. |
| `SPRD_SIPC_WCN` | all | Only effect: `sblock_register_notifier()` skips the "handler already registered" `-EBUSY` check; `sc23xx/sipc/wcn_sipc.c` registers its sblock handler on each channel open. Stock (which builds the older `INTEGRATED_MARLIN2_GE2` stack) and `lineageos_dw99_defconfig` (which uses the `SC2342_INTEG` stack we build) both set it. |
| `TASK_DELAY_ACCT` | all | `android-recommended.config`; stock has it; `TASKSTATS` is already on. Small per-task cost. |
| `SENSORS_SC7A22` | DM63, DW100U (`watch_sc7a22.fragment`) | `sprd,sc7a22` accelerometer / step counter node (i2c 0x18) in the DM63 and DW100U DTs. MT107, MT108 and DM82 already enable it in their own fragment. |
| `DISP_LITE_R2P0` | all | Stock has it =y and every stock DT carries video-mode panel entries. The option only affects the boot-logo copy in `sprd_dispc.c` (`sprd_dispc_logo_proc()`: 64-bit DMA mask and `iova - 0x80000000`), skipped for command-mode panels; without it the first frame on a video-mode panel scans from a wrong address. The DPU variant needs no option: all dpu/dispc code is built in and the DT `sprd,ip = "dpu-lite-r1p0"` selects it. |

| `IODEBUG` off | all | `sprd_sharkle_defconfig` enables it; none of the six stock configs has it. It adds fs-IO accounting and a D-state monitor (`/sys/kernel/debug/iodebug`) and claims netlink protocol 29 (`NETLINK_IODEBUG`), which the DW100U Goodix fingerprint driver also uses. |
| `BACKLIGHT_SC2721_RGBLED`, and `BACKLIGHT_PWM` off | all (RGBLED only binds on the MT108) | All six stock configs have `BACKLIGHT_PWM` off and `BACKLIGHT_SC2721_RGBLED=y`. `sprd_sharkle_defconfig` has `BACKLIGHT_PWM=y`; its driver registers `sprd_backlight`, the same name `sprd_panel.c` registers for the OLED backlight (`oled_backlight_init`, return value ignored), so brightness/AOD could break; now off. `sc2721_rgbled_bl.c` matches only `sprd,sc2721-bltc-rgb`, which is `okay` only in the MT108 DT (ST77916 LCD, `is_oled` false, so no OLED backlight there): it is the MT108 LCD backlight. Disabled in the other DTs, so it does not probe and cannot collide. |
## A candidates examined and not enabled

| Option | Decision |
|---|---|
| `INTEGRATED_MARLIN2_GE2`, `INTEGRATED_MARLIN2_GE2_SOC` | Not enabled, and they must not be. They build `drivers/misc/sprdwcn_bsp/sc2342/` (the stock kernel's WCN driver; stock uses this one, not `SC2342_INTEG`). We build `sc23xx/` (`SC23XX` + `SC2342_INTEG` + `WCN_SIPC` + `WCN_PLATFORM`, the stack `sprdwl_ng`, `sprdbt_tty`, `sprd_fm` and the GNSS work are built against). Both generations bind the same DT compatibles (`sprd,integrate_marlin`, `sprd,integrate_gnss`) and would register the same platform drivers and misc devices. Same function, different driver generation; not a rename. Only `lineageos_dw99_defconfig` uses the `SC2342_INTEG` stack. Notes for the first boot: the char device is `/dev/slog_wcn0` here (the stock userspace expects `/dev/slog_wcn`); check dmesg for `gnss path=` and `firmware path=` to confirm the WCN and GNSS firmware files are found. Open: whether the sc23xx firmware load matches what the stock WCN firmware partitions expect has not been tested on a unit. |
| `CPU_FREQ_GOV_SPRDEMAND` (+ `CPU_FREQ_GOV_COMMON`) | Not enabled. The stock default governor is `interactive`, same as ours, and no ramdisk `.rc` selects `sprdemand` (only chown/chmod on `scaling_governor`). sprdemand calls `cpu_down()`/`cpu_up()` from its governor work (`cpufreq_sprdemand.c`); the CIP `cpufreq_suspend()` holds `get_online_cpus()` while it stops governors, so stopping a governor whose work is inside `cpu_down()` can deadlock against the hotplug lock. Revisit only if a vendor PowerHAL needs it (not checked, no `/vendor`). |
| `LEDS_SPRD_BLTC_RGB`, `LEDS_SPRD_KPLED`, `BACKLIGHT_GENERIC` | Not needed. The `sc2721-bltc-rgb` and `sc2721-kpled` nodes are `status = "disabled"` except the MT108's bltc-rgb (which stock serves with the RGBLED backlight driver, above), and the MT108 stock config has no LED driver either. `BACKLIGHT_GENERIC` has no DT user. The `red_bl/green_bl/blue_bl` chowns in `init.common.rc` fail harmlessly without the nodes. |
| `USB_OTG_WAKELOCK` | Not enabled (inert): `android-base.config` lists it, but `otg-wakelock.c` only holds the wakelock on `usb_phy` notifier events, and nothing in `phy-sprd-musb.c` or `musb_sprd.c` emits them (only `phy-ab8500-usb.c` does). It would add `USB_OTG_UTILS` and an error line at `late_initcall`. Host-mode suspend handling is in `musb_sprd.c` (see KERNEL_TWEAKS.md). |
| `CMDLINE_FROM_BOOTLOADER` | No-op: our `CMDLINE=""` hides the choice (`prompt ... if CMDLINE != ""`) and the bootloader command line is used (`drivers/of/fdt.c`). Same behaviour as stock. |
| `RD_LZ4`, `RD_LZO`, `RD_XZ`, `DECOMPRESS_*`, `XZ_DEC*` | Not needed: both stock ramdisks are gzip (`RD_GZIP` is on). Enable only if a repacked ramdisk uses another codec. |
| `SCHED_AUTOGROUP` | Not needed: Android puts tasks in cpu cgroups, and autogroup only applies to tasks left in the root group. |
| `PID_NS`, `POSIX_MQUEUE`(+`_SYSCTL`) | Not needed by Android 8.1 (no use in the ramdisk); `USER_NS` is off, so these would only be reachable by root. Left off for attack surface. |
| `I2C_CHARDEV` | Not seen in any `.rc` or `ueventd`. Raw I2C access from userspace is a debug tool; off. Add per device when bringing up a sensor. |

## C: conflicts with our choices (kept ours)

| Option | Why ours stays |
|---|---|
| `SYSCTL_SYSCALL` | Obsolete binary sysctl syscall (removed upstream); extra attack surface, no user. |
| `AUDITSYSCALL`, `AUDIT_WATCH`, `AUDIT_TREE` | `AUDIT=y` already gives SELinux AVC messages. Syscall auditing adds work to every syscall on a 4-core A53 watch for no Android 8.1 consumer. |
| `VIRTUALIZATION`, `NET_9P`, `NET_9P_VIRTIO`, `NETWORK_FILESYSTEMS` | Attack surface with no use on a watch. |
| `USB_USBNET` and its drivers (`USB_NET_*`, `USB_ARMLINUX`, `USB_BELKIN`, `MII`), `ETHERNET`, `NET_VENDOR_*`, `SMC91X`, `SMSC911X`, `NET_CADENCE` | Host-mode USB ethernet and board Ethernet drivers reachable from a plugged device; the OTG host path is already an off-by-default option (`watch_otg.fragment`). `USB_USBNET` is in `android-recommended.config`; add it with `watch_otg` if USB tethering in host mode is wanted. |
| `ENABLE_DEFAULT_TRACERS` | Needs `FTRACE`, which `watch_release.fragment` turns off. |
| `JOYSTICK_XPAD*`, `HID_ACRUX_FF`, `MEDIA_SUPPORT`, `DVB_TUNER_DIB*` | No gamepad, DVB or camera-by-V4L2 use on the watch (the camera is the SPRD driver, not `MEDIA_SUPPORT`); size and attack surface. |

## B: harmless or optional (left off)

`ARM64_ERRATUM_832075` (Cortex-A57 only, the watches have A53), `BSD_PROCESS_ACCT`(+`_V3`),
`PRINT_QUOTA_WARNING`, `CC_OPTIMIZE_FOR_SIZE` (stock builds `-Os`; ours `-O2`, a size/speed choice, not a
function), `LOCALVERSION_AUTO`, `DEBUG_GPIO`, `IP_PNP_BOOTP`, `CRC7`, `CRC_ITU_T` (libraries pulled in
by drivers that need them), `GENERIC_PHY`, `PHY_XGENE`, `SERIO`, `SERIO_LIBPS2`, `INPUT_MOUSEDEV_PSAUX`,
`LCD_CLASS_DEVICE` (no DT user), `EXT2_FS`, `EXT2_FS_XATTR`, `EXT3_FS` (the stock fstab uses ext4 and f2fs),
`TOUCHSCREEN_TLSC6X` (no `tlsc6x,tlsc6x_ts` node in the MT108 or DM82 DT), `TOUCHSCREEN_CST9217`
(DM63: `hyn_ts` unbound on this unit; MT108: no `hyn_ts` node, explicitly off in `mt108.fragment`).

## Stock symbols that do not exist in this tree

| Stock option | Devices | Same function elsewhere? |
|---|---|---|
| `INPUT_QMAX981` | all | Yes: `INPUT_QMA6981_I2C` (`qma6981.c`, compatible `QST,qmax981`); MT107, MT108, DM82 enable it, DM63 and DW100U leave that chip unbound ("not needed"). |
| `INPUT_FINGERPRINT` | all | Only a Makefile hook in `drivers/input`; no fingerprint driver in tree (not checked whether the DTs carry a fingerprint node). |
| `GOODIX_TEE_FINGERPRINT` (DW100U), `SUNWAVE_FINGERPRINT` (DM63, MT107, MT108, DM82), `HEALTH_GOODIX` (DM63, MT108) | as listed | No. Vendor drivers, no source here. |
| `HEARTRATE_BD1820` (DW100U, MT108, DM82), `INPUT_SENSORS_BD1XXX` (all but MT107) | as listed | No. The `bd1xxx_hr` DT node has no driver (HRS3300/HRS3918 are covered by `INPUT_SENSORS_HRS3XXX`). |
| `SENSORS_MSP891`, `SENSORS_OCH1970` (all but DM63), `SENSORS_COMPASS_AF6837` (all), `INPUT_FALL_OFF` (DM63, MT108) | as listed | No. No source or compatible in tree (OCH1970 is the MT108's magnetic open/rotate sensor, see its SPEC). |
| `TOUCHSCREEN_BETTERLIFE_COB_JR` | all | No: the `btl,btl_ts` node (present in every stock DT) has no driver. |
| `TOUCHSCREEN_CTS0XX_TS_SMARTWATCH_JR` (all), `TOUCHSCREEN_CTS0XX_E10_GS17` (DM63, DW100U, MT107) | as listed | Yes, renamed: `TOUCHSCREEN_CST0XX` (`cst0xx_ts.c`, "smartwatch" strings; compatible `cst0xx,cst0xx_ts`), enabled in the DM63 and MT fragments. |

## Result

Fragment order in `scripts/watch_build.sh`: `watch_base`, `watch_stock_parity`, `watch_hardening`,
device fragments. The script's fragment verification checks every line of every fragment against the
final `.config`, so the hardening options are still confirmed after the parity options are on.

Image size after the change (bytes, all six builds pass the fragment verification): DM63, DW100U silver
and V13M 24134440 (+86008), MT107 and DM82 24168480 (+85624), MT108 24151328 (+85624).
