# First boot: collecting data

`scripts/first_boot_collect.sh` takes a read-only snapshot of a booted watch over adb. Use it on the first boot of every kernel/GSI combination, before changing anything, and attach the directory (after reading it) to the bug report.

```
scripts/first_boot_collect.sh --dry-run                    # prints the commands, no adb call
scripts/first_boot_collect.sh [--serial SN] [--out-dir DIR]
```

`--serial` is required when more than one device is attached. Output: `DIR/YYYYmmdd-HHMMSS/` (default `./first_boot_collect/`, the directory must not exist). Test: `scripts/tests/first_boot_collect_test.sh` (stub adb with canned output).

## What it does and does not do

- Device side: only `adb shell` of the commands in the script (list with `--dry-run`), `adb devices`, `adb get-state`. Each command is checked at start-up against a blacklist (redirections other than `2>&1`/`/dev/null`, `rm`, `mount -o`, `setprop`, `settings put`, `dd`, `pm`, `am`, ...) and the script stops if one matches. No root needed: `dmesg` is read plainly, and only if the kernel refuses (`dmesg_restrict`) `su -c dmesg` is tried, when `su` exists (Magisk asks for a grant on the DW100U). Nothing is remounted, pushed, installed, set or rebooted.
- Privacy: the adb serial, the values of serial/IMEI/MEID/IMSI/ICCID/MAC/bdaddr properties (found in `getprop`, then replaced everywhere), `*serialno=`-style key=value pairs (`/proc/cmdline`), MAC addresses, IMEI/IMSI/ICCID-looking numbers next to those words, Wi-Fi SSIDs and latitude/longitude pairs (`dumpsys location`) are replaced by `<SERIAL>`, `<REDACTED>`, `<MAC>`, `<ID>`, `<ICCID>`, `<SSID>`, `<LATLON>` before a file is written, and `summary.txt` says so at the top. A last scan for the serial and MAC-like values deletes the whole output and fails if one is left. Not redacted: package names, process names, anything the patterns do not recognise. Read the files before sharing them.
- A command that fails writes `(command failed)` into its file and the run goes on. `/sys/kernel/debug` and `/proc/pressure` absent are noted, not errors.

## Files

`summary.txt` (read first), `getprop`, `uname`, `cmdline`, `version`, `modules` (`lsmod`, else `/proc/modules`), `dmesg`, `logcat` (`-d -b all`), `meminfo`, `pressure` (`/proc/pressure/*`), `surfaceflinger`, `gfxinfo` (frame totals and jank lines only), `battery`, `location`, `wifi` and `bluetooth` (first 80 lines), `mount`, `devnodes` (`ls -lZ /dev/mali0 /dev/adf* /dev/gsp*`), `getenforce`, `pstore` (`/sys/fs/pstore/*`), `debugfs`, all `.txt`.

`summary.txt` has: kernel, SELinux mode, the properties the patch sets (`ro.config.low_ram`, `persist.sys.vndk`, `...use_freezer`, plus `ro.hardware`, `ro.ramsize`, `ro.run.ramconfig`), whether PSI exists, pstore file count, and up to 20 lines each of `dmesg` matching mali warnings (`mali` with warn/err/fail/fault/timeout/hang/reset), `Kernel panic`, `BUG`, `Oops`, `avc: denied` (also in `logcat`), and the `lmkd` lines of `logcat`.

## What to look at first

1. `summary.txt`: `Kernel panic`, `BUG`, `Oops` counts. A pstore file is a previous crash: read `pstore.txt` before anything else.
2. `ro.hardware` must be `sl8541e_1h10` (else the vendor rc is not read), `ro.config.low_ram` true, `persist.sys.vndk` 27, `use_freezer` false. An empty value means `/vendor/etc/init/gsi_defaults.rc` was not parsed: grep `logcat`/`dmesg` for `init:` errors on that file.
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
| App freezer | the property is false: `dumpsys activity` must not report the freezer enabled (cgroup v1 freezer only; `cgroup.freeze` is missing) | no freezer at all, nothing to check |
| netbpfload / eBPF | `dmesg` and `logcat` for `bpf`/`netbpfload` errors | not expected to work: no eBPF in that kernel |
| `avc: denied` | `dmesg` and `logcat` | same |

If `lmkd` picks the wrong interface on a route, do not force it in the shared vendor patch (see "lmkd" in `VENDOR_GSI_PATCH.md`): report it with this directory.
