# DW100U AOD userspace (replaces the Magisk `wearit_aod`)

Kernel side: `CONFIG_SPRD_AOD_IDLE` (`sprd_aod_idle.c`, `dw100u_aod.fragment`). Nothing here was run on a device.

## Files

| File | Purpose |
|---|---|
| `aod_idled.sh` | Sets `/sys/kernel/aod_idle/idle` 1 when wakefulness is Dozing, 0 when Awake; optional `idle_brightness` from `persist.sys.aod_brightness` |
| `aod_idled.rc` | init service (starts at `sys.boot_completed=1`, user `system`, group `system log`) plus the sysfs chown |
| `overlay/` | framework-res RRO, `config_powerDecoupleAutoSuspendModeFromDisplay=true`; build in `overlay/BUILD.md` |

## Design and battery cost

- Signal: a blocking `logcat -b events` read of `power_screen_state` and `screen_toggled`. No polling, no wakeups while the screen state is stable; the process sleeps in the kernel and does not hold a wakelock. Cost per transition: one `dumpsys power` (about 0.1 to 0.3 s CPU, estimate, not measured) plus 1 s sleeps only while the state is unsettled. Resident: one sh and one logcat process.
- Fallback if those event tags do not fire in doze on this ROM (unverified): `setprop persist.sys.aod_idled.poll 10` polls `dumpsys power` every 10 s. Each poll is the same 0.1 to 0.3 s CPU; sleeps do not wake a suspended CPU, so polls only run when it is awake anyway.
- Double tap wake needs no code here: the driver emits `KEY_WAKEUP`, the framework wakes, the Awake event writes `idle=0` (the driver also drops idle on display RUN). Asleep (screen off after AOD) leaves idle as is, like stock.
- The script writes `idle=0` on SIGTERM so a stopped service never leaves the touch screen swallowed.
- Writes are refused with EIO while the panel is powered down; Dozing is retried 4 times 1 s apart.

## SELinux (Android 8.1, enforcing)

Not tested; we do not know how `wearit_aod` was labelled and did not inspect it. Options:
1. Permissive/userdebug test: leave `seclabel` out (runs in `init`, no policy change, too broad for production).
2. Magisk: use the module `service.sh` below; it runs in the Magisk root context.
3. Proper: a domain `aod_idled` (type + `init_daemon_domain`) needing at least: `allow aod_idled sysfs:file rw_file_perms` (or a dedicated label for `/sys/kernel/aod_idle` via `genfscon`), `shell_exec` exec for `sh`/toybox, `logd` read (`logd_socket`/`logdr_socket`), `service_manager` find for `power_service` and binder call to `system_server` (for `dumpsys`), `getprop` read of the two properties. Collect exact denials with `dmesg | grep avc` and refine; do not hand-write more than the denials show.

## Install

### A. System files (needs rw /system and /vendor)
    /system/bin/aod_idled.sh              0755 root:shell  u:object_r:system_file:s0
    /system/etc/init/aod_idled.rc         0644 root:root   u:object_r:system_file:s0
    /vendor/overlay/AodSleepOverlay.apk   0644 root:root   u:object_r:vendor_overlay_file:s0
Reboot. For `sprd_aod_idle` as a module add `insmod /vendor/lib/modules/sprd_aod_idle.ko` to a `on early-boot` line in the rc (the stock name was `aod_idle.ko`).

### B. Magisk module (systemless)
    aod_dw100u/module.prop            (id=aod_dw100u, name, version, versionCode, author)
    aod_dw100u/system/bin/aod_idled.sh
    aod_dw100u/system/vendor/overlay/AodSleepOverlay.apk
    aod_dw100u/service.sh
`service.sh` (an `.rc` in a module is parsed too late on Magisk before 24, so start the script from here):

    #!/system/bin/sh
    until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
    chmod 0664 /sys/kernel/aod_idle/idle /sys/kernel/aod_idle/idle_brightness
    nohup /system/bin/sh /system/bin/aod_idled.sh >/dev/null 2>&1 &

Disable or remove the old `wearit_aod` module first, or two writers fight over `idle`.

## Test (device owner's OK needed; battery, no USB for power numbers)

1. `cat /sys/kernel/aod_idle/status` shows `panel_powered=1 dtap=1`; `logcat -s aod_idled` shows `start`.
2. `cmd overlay list` shows the overlay enabled.
3. Let the watch doze (AOD face). `logcat -s aod_idled`: `idle=1`; `status` shows `dcs_0x0a=0xdc(idle)`; `dumpsys power | grep mWakefulness` = Dozing.
4. Suspend works in AOD: `cat /d/suspend_stats` (success count grows with the screen in AOD; root, debugfs).
5. `getevent -l`: no `hyn_ts` events in idle; double tap gives `KEY_WAKEUP` on `aod_dtap`; screen wakes, log shows `idle=0`, `0x9c`.
6. `dmesg | grep -i -E 'aod|panel|dsi|hyn'` clean across the cycle (`refused` stays 0 outside suspend).
7. Compare `current_now` against the Magisk `wearit_aod` AOD for 15 to 30 min.

## Rollback

- Magisk: disable/remove the module, reboot (re-enable `wearit_aod` if wanted).
- System files: delete the three files, reboot.
- Immediately: `stop aod_idled; echo 0 > /sys/kernel/aod_idle/idle` (root).
- Kernel side: `echo 0 > /sys/kernel/aod_idle/dtap` lets touches pass through.
