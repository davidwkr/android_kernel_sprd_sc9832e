#!/usr/bin/env bash
# Test harness for scripts/first_boot_collect.sh with a stub adb (a shell
# function) that feeds canned outputs, including fake serials, MACs and IMEIs:
# no device, no adb binary needed.
# Usage: scripts/tests/first_boot_collect_test.sh
# Knobs: FAKE_DMESG_DENIED=1 (plain dmesg refused, su -c dmesg works),
# FAKE_TWO=1 (two devices attached), FAKE_LEAK=1 (a MAC the patterns cannot
# see, in a form that survives: tests the final check).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../first_boot_collect.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# fake identifiers: none of them may appear in the output
SER=FAKESER12345 PSER=P0RPSER98765 IMEI=356938035643809 MAC=aa:bb:cc:dd:ee:ff MAC2=11-22-33-44-55-66
ICCID=8901260123456789012 IMSI=310150123456789 LATLON='37.4219983,-122.0840000' SSID=HomeNetSecret
export T SER PSER IMEI MAC MAC2 ICCID IMSI LATLON SSID

adb_mock() {
  [ "${1:-}" != -s ] || shift 2
  local sub=${1:-}
  shift || true
  echo "$sub $*" >>"$T/adb.log"
  case $sub in
    get-state) echo device ;;
    devices)
      printf 'List of devices attached\n%s\tdevice\n' "$SER"
      [ "${FAKE_TWO:-0}" != 1 ] || printf 'OTHER0002\tdevice\n' ;;
    shell)
      case $1 in
        getprop) printf '[ro.hardware]: [sl8541e_1h10]\n[ro.serialno]: [%s]\n[ro.boot.serialno]: [%s]\n[persist.radio.imei]: [%s]\n[ro.config.low_ram]: [true]\n[persist.sys.vndk]: [27]\n[persist.device_config.activity_manager_native_boot.use_freezer]: [false]\n[ro.build.version.release]: [14]\n[persist.bt.macaddr]: [%s]\n' "$SER" "$SER" "$IMEI" "$MAC" ;;
        'uname -a') echo "Linux localhost 4.14.336 #1 SMP PREEMPT aarch64" ;;
        'cat /proc/cmdline') echo "console=ttyS1 androidboot.serialno=$SER androidboot.mode=normal" ;;
        'cat /proc/version') echo "Linux version 4.14.336 (build@host)" ;;
        lsmod*) printf 'sprdwl_ng 100 0 - Live 0x0\n' ;;
        'logcat -d -b all') printf '01-01 00:00:01.000 I lmkd: Using psi monitors\n01-01 00:00:02.000 W wifi: mac %s ssid\n01-01 00:00:03.000 E auditd: avc: denied { read } for comm=x\n' "$MAC2" ;;
        'cat /proc/meminfo') echo "MemTotal: 1800000 kB" ;;
        'if [ -d /proc/pressure'*) printf '== /proc/pressure/memory\nsome avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' ;;
        'dumpsys SurfaceFlinger') echo "Display 0 HWC: composition ok; serial $SER" ;;
        'dumpsys gfxinfo'*) printf '** Graphics info for pid 1 [system] **\nTotal frames rendered: 10\n' ;;
        'dumpsys battery') printf 'Current Battery Service state:\n  level: 80\n' ;;
        'dumpsys location') printf 'Last Known Locations:\n  passive: Location[gps %s hAcc=5]\n' "$LATLON" ;;
        'dumpsys wifi'*) printf 'Wi-Fi is enabled\nMAC: %s\nSSID: "%s", BSSID: %s\n' "$MAC" "$SSID" "$MAC2" ;;
        'dumpsys bluetooth_manager'*) printf 'Bluetooth enabled\n  address: %s\n  IMEI-like deviceId: %s\n  iccid=%s imsi %s\n' "$MAC" "$IMEI" "$ICCID" "$IMSI" ;;
        mount) echo "/dev/block/dm-0 / ext4 ro 0 0" ;;
        'ls -lZ /dev/mali0'*) echo "crw-rw-rw- 1 root root u:object_r:gpu_device:s0 /dev/mali0" ;;
        getenforce) echo Permissive ;;
        'ls -l /sys/fs/pstore'*) printf 'total 0\n== /sys/fs/pstore/console-ramoops\nKernel panic - not syncing: test\n' ;;
        'if [ -d /sys/kernel/debug'*) echo "/sys/kernel/debug absent" ;;
        dmesg)
          if [ "${FAKE_DMESG_DENIED:-0}" = 1 ]; then echo "dmesg: klogctl: Operation not permitted"; else
            printf '[1.0] mali: warning: GPU fault\n[2.0] BUG: scheduling while atomic\n[3.0] Oops: 0000\n[4.0] avc: denied { x } for pid=1\n[5.0] wlan mac %s\n' "$MAC"; fi ;;
        'command -v su'*) printf '[1.0] mali: error: timeout\n[2.0] su dmesg ok\n' ;;
        *) echo "adb_mock: unexpected shell command: $1" >&2; return 1 ;;
      esac ;;
    *) echo "adb_mock: unsupported adb command: $sub" >&2; return 1 ;;
  esac
}
export -f adb_mock

PASS=0 FAIL=0
check() { local d=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "ok   $d"; else FAIL=$((FAIL + 1)); echo "FAIL $d"; fi; }
run() { : >"$T/adb.log"; OUT=$(ADB=adb_mock TMPDIR=$T bash "$SCRIPT" "$@" 2>&1); RC=$?; }
[ -z "${FBC_DEBUG:-}" ] || run() { : >"$T/adb.log"; OUT=$(ADB=adb_mock TMPDIR=$T bash "$SCRIPT" "$@" 2>&1); RC=$?; echo "--- $*: rc $RC"; echo "$OUT" | tail -n 12; }
has() { grep -q -- "$1" <<<"$OUT"; }
D() { find "$T/out" -mindepth 1 -maxdepth 1 -type d | head -n 1; }
no_leak() { # none of the fake identifiers in any file of the directory
  ! grep -rqF -e "$SER" -e "$PSER" -e "$IMEI" -e "$MAC" -e "$MAC2" -e "$ICCID" -e "$IMSI" -e "$LATLON" -e "$SSID" -e 37.4219983 "$1"
}

echo "-- syntax"
check "bash -n" bash -n "$SCRIPT"
if command -v shellcheck >/dev/null; then check "shellcheck" shellcheck "$SCRIPT"; else echo "skip shellcheck (not installed)"; fi

echo "-- dry run"
run --dry-run --out-dir "$T/out"
check "exit 0" test "$RC" = 0
check "prints the commands" has 'logcat -d -b all'
check "no adb call at all" test ! -s "$T/adb.log"
check "nothing written" test ! -e "$T/out"

echo "-- collection"
run --out-dir "$T/out"
check "exit 0" test "$RC" = 0
d=$(D)
for f in getprop uname cmdline version modules dmesg logcat meminfo pressure surfaceflinger gfxinfo battery location wifi bluetooth mount devnodes getenforce pstore debugfs summary; do
  check "$f.txt written" test -s "$d/$f.txt"
done
check "no identifier survives (fake serials, IMEI, IMSI, ICCID, MACs, SSID, lat/lng)" no_leak "$d"
check "serial not in the directory name" bash -c "[[ '$d' != *$SER* ]]"
check "placeholders present" bash -c "grep -rq '<SERIAL>' '$d' && grep -rq '<MAC>' '$d' && grep -rq '<REDACTED>' '$d' && grep -rq '<LATLON>' '$d' && grep -rq '<SSID>' '$d' && grep -rq '<ICCID>' '$d' && grep -rq '<ID>' '$d'"
check "non-sensitive properties kept" grep -q '^\[ro.hardware\]: \[sl8541e_1h10\]' "$d/getprop.txt"
check "summary starts with the privacy note" bash -c "sed -n 2p '$d/summary.txt' | grep -q '^PRIVACY'"
check "summary: mali, BUG, Oops, avc found" bash -c "grep -q 'mali warnings (dmesg.txt): 1' '$d/summary.txt' && grep -q 'BUG (dmesg.txt): 1' '$d/summary.txt' && grep -q 'Oops (dmesg.txt): 1' '$d/summary.txt' && grep -q 'avc: denied (dmesg.txt): 1' '$d/summary.txt'"
check "summary: GSI defaults shown" bash -c "grep -q 'ro.config.low_ram: true' '$d/summary.txt' && grep -q 'use_freezer: false' '$d/summary.txt'"
check "dmesg: no su when plain dmesg works" bash -c "! grep -q 'command -v su' '$T/adb.log'"

echo "-- device is only read"
check "only devices/get-state/shell used" bash -c "! grep -vE '^(devices|get-state|shell) ' '$T/adb.log'"
check "no write, mount, set or install command sent" bash -c "! grep -E 'remount|mount -o|setprop|settings put|\bpm |\bam |\bsvc|\brm |\bdd |\btee|\bchmod|\binsmod|reboot|install|push|root' '$T/adb.log'"
check "the only redirections are 2>&1, 2>/dev/null, >/dev/null" bash -c "! sed -E 's#2>&1|2>/dev/null|>/dev/null##g' '$T/adb.log' | grep -q '[<>]'"

echo "-- dmesg refused: su -c dmesg"
rm -rf "$T/out"
export FAKE_DMESG_DENIED=1; run --out-dir "$T/out"; unset FAKE_DMESG_DENIED
d=$(D)
check "exit 0" test "$RC" = 0
check "su -c dmesg used once, only for dmesg" bash -c "[ \$(grep -c 'command -v su' '$T/adb.log') = 1 ] && ! grep -E 'su -c' '$T/adb.log' | grep -qv dmesg"
check "dmesg.txt holds the su output" grep -q 'su dmesg ok' "$d/dmesg.txt"

echo "-- serial handling"
rm -rf "$T/out"
export FAKE_TWO=1; run --out-dir "$T/out"
check "two devices without --serial: refused" test "$RC" != 0
check "...nothing written" test ! -e "$T/out"
run --serial "$SER" --out-dir "$T/out"
check "two devices with --serial: ok" test "$RC" = 0
check "...-s given to every adb command" bash -c "! grep -q 'adb_mock' '$T/adb.log'"
unset FAKE_TWO

echo "-- usage"
run --bogus
check "unknown option: usage, exit 2" test "$RC" = 2

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
