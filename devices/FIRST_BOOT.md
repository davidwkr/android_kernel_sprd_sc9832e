# First boot: collecting data

`scripts/first_boot_collect.sh` takes a read-only snapshot of a booted watch over adb. Use it on the first boot of every kernel/GSI combination, before changing anything, and attach the directory (after reading it) to the bug report.

```
scripts/first_boot_collect.sh --dry-run                    # prints the commands, no adb call
scripts/first_boot_collect.sh [--serial SN] [--out-dir DIR] [--root]
```

`--serial` is required when more than one device is attached. Output: `DIR/YYYYmmdd-HHMMSS/` (default `./first_boot_collect/`, the directory must not exist). Test: `scripts/tests/first_boot_collect_test.sh` (stub adb with canned output, exit statuses and fake identifiers of every redaction category).

## What it does and does not do

- Device side: only `adb shell` of single commands (list with `--dry-run`), `adb devices`, `adb get-state`. No pipe, redirection, `;`, `&&` or substitution on the device: each producer runs alone and the host filters and truncates its output. Each command is checked at start-up against a blacklist (those characters, `rm`, `mount -o`, `setprop`, `settings put`, `dd`, `pm`, `am`, `su` other than `su -c dmesg`, ...) and the script stops if one matches. Nothing is remounted, pushed, installed, set or rebooted.
- Unprivileged by default: `dmesg` is read plainly; if the kernel refuses (`dmesg_restrict`), `dmesg.txt` says it is unavailable. Only with `--root` is `su -c dmesg` tried then (Magisk asks for a grant on the DW100U).
- Every adb call runs under `timeout` (60 s, `FBC_TIMEOUT` seconds). The exit status of every device command goes to `status.txt` and `summary.txt`; a failed command (`FAILED, exit N`) or a timed-out one (`TIMED OUT after Ns`) says so on the first line of its file, and the run goes on. (`ls` of a path that does not exist exits non-zero: `debugfs.txt` and `devnodes.txt` can say FAILED for that reason.) The status is the one adb reports; an old adb that does not pass it on shows `ok`.
- Sensitive dumps are never stored raw. `dumpsys location`, `wifi`, `bluetooth_manager` and `battery` are reduced on the host to an allowlist of diagnostic lines (enabled/disabled, state, GNSS/HAL status, errors, battery level/status; `connection: yes|no` for Wi-Fi and Bluetooth, derived from the dump), and lines that carry names, addresses, coordinates or accounts are dropped even when they match. No account dump is collected. `logcat` is warnings and above (`logcat -d -b all '*:W'`) plus the tags SurfaceFlinger, hwcomposer, mali, gralloc, lmkd, init, vold, keystore2, GnssHAL, gps, LocationManager, wifi, bluetooth and auditd; the last 3000 lines are kept.
- Redaction, on the host, before a file is written: the adb serial, the values of serial/IMEI/MEID/IMSI/ICCID/MAC/bdaddr properties, `serial` values, MAC addresses, e-mail and account names, phone numbers (international and grouped digits), IMEI/MEID-like 14-15 digit/hex runs, `Location[...]`, `latitude=`/`longitude=` and decimal coordinate pairs, and everything after `ssid` on a line, become `<SERIAL>`, `<REDACTED>`, `<EMAIL>`, `<PHONE>`, `<ID>`, `<ICCID>`, `<MAC>`, `<LATLON>`, `<SSID>`. Afterwards the whole output is scanned for each category again (and for the exact values of this device); on a hit the whole output is deleted and the script fails. Patterns are not proof: package names, process names and anything unrecognised stay, and over-redaction happens (dates written `2026-11-11`, IP addresses). `summary.txt` says what was collected and that every file must still be read before it is shared.

## Files

`summary.txt` (read first), `status.txt` (file, command, result), `getprop`, `uname`, `cmdline`, `version`, `modules` (`lsmod`, else `/proc/modules`), `dmesg`, `logcat` (above), `meminfo`, `pressure` (`/proc/pressure/*`, each read alone), `surfaceflinger` (first 600 lines), `gfxinfo` (frame totals and jank lines only), `battery`, `location`, `wifi` and `bluetooth` (allowlisted lines), `mount`, `devnodes` (`ls -lZ /dev/mali0 /dev/adf* /dev/gsp*`), `getenforce`, `pstore` (`/sys/fs/pstore/*`, each read alone, 200 lines), `debugfs`, all `.txt`.

`summary.txt` has: the review note and the list of what was collected, the command status, kernel, SELinux mode, `ro.config.low_ram`, `persist.sys.vndk`, `ro.vndk.version`, `ro.hardware`, `ro.ramsize`, `ro.run.ramconfig`, whether PSI exists, pstore file count, and up to 20 lines each of `dmesg` matching mali warnings (`mali` with warn/err/fail/fault/timeout/hang/reset), `Kernel panic`, `BUG`, `Oops`, `avc: denied` (also in `logcat`), and the `lmkd` lines of `logcat`.

## What to look at first

1. `summary.txt`: `Kernel panic`, `BUG`, `Oops` counts. A pstore file is a previous crash: read `pstore.txt` before anything else.
2. `ro.hardware` must be `sl8541e_1h10` (else the vendor rc is not read), `ro.config.low_ram` true (from `/vendor/build.prop`), `persist.sys.vndk` 27 (the GSI's vndk-detect, else the fallback in `/vendor/etc/init/gsi_defaults.rc`; empty means neither worked: grep `logcat`/`dmesg` for `init:` errors on that file). The freezer is not a patch property: after the first boot the tester runs `adb shell settings put global cached_apps_freezer disabled` (a device write, not done by our scripts) and checks `adb shell dumpsys activity | grep -i freez`.
3. `devnodes.txt`: `/dev/mali0` and `/dev/adf0` must exist. A missing `/dev/mali0` on the 4.4 route means `mali.ko` was not installed or not loaded (`modules.txt`).
4. `modules.txt`: `sprdwl_ng` (Wi-Fi) and `sprdbt_tty` (Bluetooth) loaded.
5. `avc: denied` lines: DM63 is permissive. `{ module_load }` for `init` is the known one, see "Kernel modules" in `VENDOR_GSI_PATCH.md`.
6. `surfaceflinger.txt` and `gfxinfo.txt`: composition on HWC, jank counts.

## Per route

| | 4.14 route (`config/gsi-414`, `GSI_414.md`) | 4.4 route (`watch_build.sh`, 4.4.302) |
|---|---|---|
| Kernel line | `uname.txt` says 4.14.x | 4.4.302 |
| GPU | built in: `/dev/mali0` present with no module | `mali.ko` in `modules.txt` and in `/vendor/lib/modules` |
| `pressure.txt` | `memory`, `cpu`, `io` files: PSI is on; `lmkd` log lines mention psi | `/proc/pressure absent (no PSI)`: expected; lmkd should use the in-kernel LMK (`/sys/module/lowmemorykiller` in `mount`/`dmesg`) |
| App freezer | after the `cached_apps_freezer disabled` setting above: `dumpsys activity` must not report the freezer enabled (cgroup v1 freezer only; `cgroup.freeze` is missing) | no freezer at all, nothing to check |
| netbpfload / eBPF | `dmesg` and `logcat` for `bpf`/`netbpfload` errors | not expected to work: no eBPF in that kernel |
| `avc: denied` | `dmesg` and `logcat` | same |

If `lmkd` picks the wrong interface on a route, do not force it in the shared vendor patch (see "lmkd" in `VENDOR_GSI_PATCH.md`): report it with this directory.
