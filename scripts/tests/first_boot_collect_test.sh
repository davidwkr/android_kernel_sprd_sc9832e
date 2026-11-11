#!/usr/bin/env bash
# Test harness for scripts/first_boot_collect.sh with a stub adb (a shell
# function) that feeds canned outputs and exit statuses, including fake
# identifiers of every category the script must not write: no device, no adb
# binary needed. All values are made up (example.test, +54 11 5555 0000 style).
# Usage: scripts/tests/first_boot_collect_test.sh
# Knobs of the stub: FAKE_DMESG_DENIED=1 (plain dmesg exits 1; su -c dmesg
# works), FAKE_NO_LSMOD=1 (lsmod exits 127), FAKE_HANG=1 (dumpsys battery
# sleeps past the timeout), FAKE_TERM_DEAF=1 (it also ignores TERM), FAKE_STALL_DEVICES=1
# ('adb devices' stalls), FAKE_TWO=1 (two devices attached).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../first_boot_collect.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# fake identifiers: none of them may appear anywhere in the output
SER=FAKESER12345 IMEI=356938035643809 MEID=A0000012345678 MAC=aa:bb:cc:dd:ee:ff MAC2=11-22-33-44-55-66
ICCID=8901260123456789012 IMSI=310150123456789
EMAIL=jane.doe@example.test PH1='+54 11 5555 0000' PH2=011-5555-0000 PH3=555-0123
LAT=-34.603722 LNG=-58.381592
SSID1='Home,Net Secret' SSID2=GuestNet SSID3='Cafe Wifi' SSID4=HomeNetSecret
SERV1=ABC123XYZ789 SERV2=55AA66BB77
# second and third values of each category, placed right next to the first
ICCID2=8901260123456789013 IMEI2=356938035643810 MEID2=A0000012345679 MAC3=aa:bb:cc:dd:ee:00 MAC4=aa:bb:cc:dd:ee:01
PH4=555-0124 PH5='+54 11 5555 0001' PH6=011-5555-0001 LAT2=-34.603999 LNG2=-58.381999 SERV3=QRS456TUV012 SERV4=ZZ99YY88XX
export ICCID2 IMEI2 MEID2 MAC3 MAC4 PH4 PH5 PH6 LAT2 LNG2 SERV3 SERV4 T SER IMEI MEID MAC MAC2 ICCID IMSI EMAIL PH1 PH2 PH3 LAT LNG SSID1 SSID2 SSID3 SSID4 SERV1 SERV2
LEAKS=("$SER" "$IMEI" "$MEID" "$MAC" "$MAC2" "$ICCID" "$IMSI" jane.doe example.test "5555 0000" 5555-0000 555-0123
       34.603722 58.381592 "$SSID1" "Net Secret" "$SSID2" "$SSID3" "$SSID4" "$SERV1" "$SERV2"
       "$ICCID2" "$IMEI2" "$MEID2" "$MAC3" "$MAC4" 555-0124 "5555 0001" 5555-0001 34.603999 58.381999 "$SERV3" "$SERV4")

# arm64 4.14 style oops: addresses (raw, 0x, %px, %p-hashed) and the symbols,
# offsets, timestamps and register names that must stay
oops() {
  cat <<'OOPS'
[   12.345678] Unable to handle kernel paging request at virtual address ffffffc0f0a3b240
[   12.345700] pgd = ffffffc0b1c2d000
[   12.345710] [ffffffc0f0a3b240] *pgd=0000000081234003, *pud=0000000081234003
[   12.345800] pc : mali_kbase_jd_submit+0x1c4/0x5e0 [mali]
[   12.345810] pc : ffffff8008123456
[   12.345820] lr : kbase_ioctl+0x58/0x90 [mali]
[   12.345830] sp : ffffffc0b1c2fe40
[   12.345840] x29: ffffffc0b1c2fe40 x28: ffffffc0f0a3b000
[   12.345850] x1 : 0000000000000001 x0 : ffffffc000123abc
[   12.345860] Call trace:
[   12.345870] [<ffffffc000123abc>] do_fault+0x1c/0x120
[   12.345880] [<ffffff8008123456>] el0_svc_naked+0x34/0x38
[   12.345890] hashed ptr 00000000a1b2c3d4 raw 0xffffffc000aabbcc end
[   12.345900] module Live 0xffffffbffc012000 (mali)
OOPS
}
adb_mock() {
  local ser=
  # a targeted call must name the attached device, else adb fails like this
  if [ "${1:-}" = -s ]; then
    ser=${2:-}
    [ "$ser" = "$SER" ] || { echo "REJECTED -s $ser $*" >>"$T/adb.log"; echo "adb: device '$ser' not found" >&2; return 1; }
    shift 2
  fi
  local sub=${1:-}
  shift || true
  echo "serial=${ser:-NONE} $sub $*" >>"$T/adb.log"
  case $sub in
    get-state) echo device ;;
    devices)
      [ "${FAKE_STALL_DEVICES:-0}" != 1 ] || { trap '' TERM; sleep 30; }
      printf 'List of devices attached\n%s\tdevice\n' "$SER"
      [ "${FAKE_TWO:-0}" != 1 ] || printf 'OTHER0002\tdevice\n' ;;
    shell)
      case $1 in
        getprop) printf '[ro.hardware]: [sl8541e_1h10]\n[ro.serialno]: [%s]\n[ro.boot.serialno]: [%s]\n[persist.radio.imei]: [%s]\n[ro.config.low_ram]: [true]\n[persist.sys.vndk]: [27]\n[ro.build.version.release]: [14]\n[persist.bt.macaddr]: [%s]\n' "$SER" "$SER" "$IMEI" "$MAC" ;;
        'uname -a') echo "Linux localhost 4.14.336 #1 SMP PREEMPT aarch64" ;;
        'cat /proc/cmdline') echo "console=ttyS1 androidboot.serialno=$SER androidboot.mode=normal" ;;
        'cat /proc/version') echo "Linux version 4.14.336 (build@host)" ;;
        lsmod)
          if [ "${FAKE_NO_LSMOD:-0}" = 1 ]; then echo "/system/bin/sh: lsmod: inaccessible or not found" >&2; return 127; fi
          printf 'sprdwl_ng 100 0 - Live 0x0\n' ;;
        'cat /proc/modules') printf 'sprdwl_ng 100 0 - Live 0x0 (O)\nproc_modules_marker 1 0 - Live 0x0\n' ;;
        'logcat -d -b all '*)
          printf '01-01 00:00:01.000  100  100 W lmkd: Using psi monitors\n'
          printf '01-01 00:00:02.000  100  100 W WifiService: SSID: "%s", BSSID: %s, ssid=%s, SSID "%s" SSID %s\n' "$SSID1" "$MAC2" "$SSID2" "$SSID3" "$SSID4"
          printf '01-01 00:00:03.000  100  100 E auditd: avc: denied { read } for comm=x\n'
          printf '01-01 00:00:04.000  100  100 W AccountManager: Account {name=%s, type=com.example} owner %s\n' "$EMAIL" "$EMAIL"
          printf '01-01 00:00:05.000  100  100 W Telephony: number %s / %s / %s imei=%s meid %s\n' "$PH1" "$PH2" "$PH3" "$IMEI" "$MEID"
          printf '01-01 00:00:06.000  100  100 W LocationManager: Location[gps %s,%s hAcc=5] latitude=%s longitude=%s at %s, %s\n' "$LAT" "$LNG" "$LAT" "$LNG" "$LAT" "$LNG"
          printf '01-01 00:00:08.000  100  100 W Adjacent: subscriptions: %s %s imei %s %s meids=%s,%s\n' "$ICCID" "$ICCID2" "$IMEI" "$IMEI2" "$MEID" "$MEID2"
          printf '01-01 00:00:09.000  100  100 W Adjacent: macs %s %s %s phones %s %s %s %s\n' "$MAC" "$MAC3" "$MAC4" "$PH3" "$PH4" "$PH1" "$PH5"
          printf '01-01 00:00:10.000  100  100 W Adjacent: coords %s, %s, %s, %s, %s serial: %s %s serials=%s %s\n' "$LAT" "$LNG" "$LAT2" "$LNG2" "$LAT" "$SERV1" "$SERV3" "$SERV2" "$SERV4"
          printf '01-01 00:00:07.000  100  100 W Build: serial: %s and Serial Number: %s iccid=%s imsi %s\n' "$SERV1" "$SERV2" "$ICCID" "$IMSI" ;;
        'cat /proc/meminfo') echo "MemTotal: 1800000 kB" ;;
        'ls /proc/pressure') printf 'cpu\nio\nmemory\n' ;;
        'cat /proc/pressure/'*) printf 'some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' ;;
        'dumpsys SurfaceFlinger') echo "Display 0 HWC: composition ok; serial $SERV1" ;;
        'dumpsys gfxinfo') printf '** Graphics info for pid 1 [system] **\nTotal frames rendered: 10\nsecret_gfx_line %s\n' "$EMAIL" ;;
        'dumpsys battery')
          [ "${FAKE_HANG:-0}" != 1 ] || sleep 5
          [ "${FAKE_TERM_DEAF:-0}" != 1 ] || { trap '' TERM; sleep 30; }
          printf 'Current Battery Service state:\n  AC powered: false\n  level: 80\n  scale: 100\n  Charge counter: 1234\n  serial number: %s\n' "$SERV2" ;;
        'dumpsys location')
          printf 'Location Settings: [enabled]\nLast Known Locations:\n  passive: Location[gps %s,%s hAcc=5]\n  RAWMARKER_LOC owner %s\n' "$LAT" "$LNG" "$EMAIL"
          printf 'GNSS HAL: initialized\nGNSS error: fix lat=%s lng=%s\n  network provider disabled\n' "$LAT" "$LNG" ;;
        'dumpsys wifi')
          printf 'Wi-Fi is enabled\nWifiState: 3 supplicant state: COMPLETED\nmWifiInfo SSID: "%s", BSSID: %s, MAC: %s, IP: 10.0.0.7\n' "$SSID1" "$MAC2" "$MAC"
          printf 'RAWMARKER_WIFI saved network %s contact %s\nWifi driver error: none\n' "$SSID2" "$PH1" ;;
        'dumpsys bluetooth_manager')
          printf 'Bluetooth Status\n  enabled: true\n  state: ON\n  name: %s-watch\n  address: %s\n  RAWMARKER_BT %s imei %s\n  Bonded devices: jane\n' "$SSID4" "$MAC" "$MAC2" "$IMEI" ;;
        mount) echo "/dev/block/dm-0 / ext4 ro 0 0" ;;
        'ls -lZ /dev/mali0'*) echo "crw-rw-rw- 1 root root u:object_r:gpu_device:s0 /dev/mali0" ;;
        getenforce) echo Permissive ;;
        'ls -l /sys/fs/pstore') printf 'total 0\n-r--r--r-- 1 root root 100 2000-01-01 00:00 console-ramoops\n' ;;
        'cat /sys/fs/pstore/console-ramoops')
          printf 'Kernel panic - not syncing: test\nwlan mac %s\n' "$MAC"
          oops ;;
        'ls /sys/kernel/debug') echo "ls: /sys/kernel/debug: No such file or directory" >&2; return 1 ;;
        dmesg)
          if [ "${FAKE_DMESG_DENIED:-0}" = 1 ]; then echo "klogctl: Operation not permitted" >&2; return 1; fi
          printf '[1.0] mali: warning: GPU fault\n[2.0] BUG: scheduling while atomic\n[3.0] Oops: 0000\n[4.0] avc: denied { x } for pid=1\n[5.0] wlan mac %s\n' "$MAC"
          oops ;;
        'su -c dmesg') printf '[1.0] mali: error: timeout\n[2.0] su dmesg ok\n' ;;
        *) echo "adb_mock: unexpected shell command: $1" >&2; return 1 ;;
      esac ;;
    *) echo "adb_mock: unsupported adb command: $sub" >&2; return 1 ;;
  esac
}
export -f adb_mock oops

PASS=0 FAIL=0
check() { local d=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "ok   $d"; else FAIL=$((FAIL + 1)); echo "FAIL $d"; fi; }
run() {
  : >"$T/adb.log"
  OUT=$(ADB=adb_mock TMPDIR=$T bash "${SCRIPT_RUN:-$SCRIPT}" "$@" 2>&1); RC=$?
  [ -z "${FBC_DEBUG:-}" ] || { echo "--- $*: rc $RC"; echo "$OUT" | tail -n 12; }
}
has() { grep -q -- "$1" <<<"$OUT"; }
D() { find "$T/out" -mindepth 1 -maxdepth 1 -type d | head -n 1; }
no_leak() { # none of the fake values in any file of the directory
  local l
  for l in "${LEAKS[@]}"; do
    if grep -rqF -e "$l" "$1"; then echo "leaked: $l" >&2; return 1; fi
  done
}

echo "-- syntax"
check "bash -n" bash -n "$SCRIPT"
if command -v shellcheck >/dev/null; then check "shellcheck" shellcheck "$SCRIPT"; else echo "skip shellcheck (not installed)"; fi

echo "-- the stub itself checks -s"
check "stub: -s with a wrong serial fails" bash -c "! adb_mock -s WRONG get-state 2>/dev/null"
check "stub: -s with the right serial works" bash -c "adb_mock -s $SER get-state >/dev/null"

echo "-- dry run"
run --dry-run --out-dir "$T/out"
check "exit 0" test "$RC" = 0
check "prints the commands" has 'logcat -d -b all'
check "logcat is warnings and above" has "'\*:W'"
check "no adb call at all" test ! -s "$T/adb.log"
check "nothing written" test ! -e "$T/out"

echo "-- collection"
run --out-dir "$T/out"
check "exit 0" test "$RC" = 0
d=$(D)
for f in getprop uname cmdline version modules dmesg logcat meminfo pressure surfaceflinger gfxinfo battery location wifi bluetooth mount devnodes getenforce pstore debugfs status summary; do
  check "$f.txt written" test -s "$d/$f.txt"
done
check "no fake identifier of any category survives anywhere (serial, IMEI, MEID, IMSI, ICCID, MACs, e-mail, phones, coordinates, SSIDs, serial values)" no_leak "$d"
check "serial not in the directory name" bash -c "[[ '$d' != *$SER* ]]"
check "placeholders present" bash -c "for p in '<SERIAL>' '<MAC>' '<REDACTED>' '<LATLON>' '<SSID>' '<ICCID>' '<ID>' '<EMAIL>' '<PHONE>'; do grep -rqF \"\$p\" '$d' || { echo missing \$p; exit 1; }; done"
check "non-sensitive properties kept" grep -q '^\[ro.hardware\]: \[sl8541e_1h10\]' "$d/getprop.txt"
check "logcat: the lmkd and avc lines are kept" bash -c "grep -q 'Using psi monitors' '$d/logcat.txt' && grep -q 'avc: denied' '$d/logcat.txt'"
check "sensitive dumps: allowlisted lines kept" bash -c "grep -q 'level: 80' '$d/battery.txt' && grep -q 'GNSS HAL: initialized' '$d/location.txt' && grep -q 'Wi-Fi is enabled' '$d/wifi.txt' && grep -q 'enabled: true' '$d/bluetooth.txt'"
check "sensitive dumps: connection state derived (wifi yes)" grep -q '^connection: yes' "$d/wifi.txt"
check "sensitive dumps: everything else dropped (no raw dump)" bash -c "! grep -rq 'RAWMARKER\\|Last Known\\|saved network\\|Bonded\\|Charge counter\\|secret_gfx' '$d'"
check "sensitive dumps: coordinate and address lines dropped" bash -c "! grep -qiE 'lat=|passive|address|BSSID|IP:' '$d/location.txt' '$d/wifi.txt' '$d/bluetooth.txt'"
check "summary: review note and what was collected" bash -c "grep -q 'REVIEW BEFORE SHARING' '$d/summary.txt' && grep -q 'Collected:' '$d/summary.txt' && grep -q 'raw dumps are not stored' '$d/summary.txt'"
check "summary: mali, BUG, Oops, avc found" bash -c "grep -q 'mali warnings (dmesg.txt): 1' '$d/summary.txt' && grep -q 'BUG (dmesg.txt): 1' '$d/summary.txt' && grep -q 'Oops (dmesg.txt): 1' '$d/summary.txt' && grep -q 'avc: denied (dmesg.txt): 1' '$d/summary.txt'"
check "summary: GSI properties shown" bash -c "grep -q 'ro.config.low_ram: true' '$d/summary.txt' && grep -q 'persist.sys.vndk: 27' '$d/summary.txt'"
check "summary: the failed commands are named" bash -c "grep -q 'FAILED, exit 1: ls /sys/kernel/debug' '$d/summary.txt'"
check "debugfs.txt says the command failed" grep -q '^(ls /sys/kernel/debug: FAILED, exit 1)' "$d/debugfs.txt"
check "status.txt: one line per command with its result" bash -c "grep -q $'^getprop.txt\\tgetprop\\tok\$' '$d/status.txt' && grep -q $'^debugfs.txt\\tls /sys/kernel/debug\\tFAILED, exit 1\$' '$d/status.txt'"
check "pressure: each file read on its own" bash -c "grep -c '^== /proc/pressure/' '$d/pressure.txt' | grep -qx 3"
check "pstore: file read on its own" grep -q 'Kernel panic' "$d/pstore.txt"
check "dmesg readable: no su" bash -c "! grep -q 'su ' '$T/adb.log'"

echo "-- adjacent values of the same category, kernel addresses"
check "adjacent values: none of the second/third ones survive (ICCID, IMEI, MEID, MAC, phone, coordinates, serial)" no_leak "$d"
check "kernel addresses: every form gone" bash -c "! grep -rqiE '(^|[^0-9a-z])(0x)?[0-9a-f]{16}([^0-9a-z]|\$)' '$d' && ! grep -rqiE 'ffffff[0-9a-f]{10}' '$d'"
check "kernel addresses: <KADDR> present in dmesg and pstore" bash -c "grep -q '<KADDR>' '$d/dmesg.txt' && grep -q '<KADDR>' '$d/pstore.txt'"
check "oops: symbols, offsets, sizes, timestamps, register names stay" bash -c "grep -q 'mali_kbase_jd_submit+0x1c4/0x5e0 \[mali\]' '$d/dmesg.txt' && grep -q 'do_fault+0x1c/0x120' '$d/dmesg.txt' && grep -q 'el0_svc_naked+0x34/0x38' '$d/pstore.txt' && grep -q '^\[   12.345678\] Unable to handle kernel paging request at virtual address <KADDR>' '$d/dmesg.txt' && grep -q 'sp : <KADDR>' '$d/dmesg.txt' && grep -q 'x29: <KADDR> x28: <KADDR>' '$d/dmesg.txt' && grep -q '\[<<KADDR>>\] do_fault' '$d/dmesg.txt'"
check "summary says <KADDR> covers hashed pointers too" grep -q 'hashed pointers' "$d/summary.txt"

echo "-- device is only read, one plain command at a time"
check "every targeted call carries -s SERIAL (and the stub accepted it)" bash -c "! grep -v '^serial=$SER ' '$T/adb.log' | grep -v '^serial=NONE devices *\$' | grep -q ."
check "no call was rejected" bash -c "! grep -q REJECTED '$T/adb.log'"
check "only devices/get-state/shell used" bash -c "! grep -vE '^serial=[A-Z0-9]+ (devices|get-state|shell) ' '$T/adb.log'"
check "no pipe, redirection, ';', '&&', '||', substitution or loop in any shell command" bash -c "! grep '^serial=[A-Z0-9]* shell ' '$T/adb.log' | grep -E '[|;&<>\`\$()]|\\bfor\\b|\\bif\\b'"
check "no write, mount, set or install command sent" bash -c "! grep -E 'remount|mount -o|setprop|settings put|\\bpm |\\bam |\\bsvc|\\brm |\\bdd |\\btee|\\bchmod|\\binsmod|reboot|install|push|root' '$T/adb.log'"

echo "-- dmesg refused, no --root"
rm -rf "$T/out"
export FAKE_DMESG_DENIED=1; run --out-dir "$T/out"
d=$(D)
check "exit 0" test "$RC" = 0
check "su is never called without --root" bash -c "! grep -q 'su ' '$T/adb.log'"
check "dmesg.txt says unavailable and why" bash -c "grep -q 'dmesg unavailable' '$d/dmesg.txt' && grep -q 'FAILED, exit 1' '$d/dmesg.txt'"
check "status.txt records dmesg as failed" grep -q $'^dmesg.txt\tdmesg\tFAILED, exit 1$' "$d/status.txt"
rm -rf "$T/out"
run --root --out-dir "$T/out"
d=$(D)
check "--root: exit 0" test "$RC" = 0
check "--root: su -c dmesg used once, only for dmesg" bash -c "[ \$(grep -c 'su ' '$T/adb.log') = 1 ] && grep -q 'shell su -c dmesg\$' '$T/adb.log'"
check "--root: dmesg.txt holds the su output" grep -q 'su dmesg ok' "$d/dmesg.txt"
check "--root: both dmesg results in status.txt" test "$(grep -c '	dmesg.txt$\|^dmesg.txt	' "$d/status.txt")" = 2
unset FAKE_DMESG_DENIED

echo "-- lsmod fails, command timeout"
rm -rf "$T/out"
export FAKE_NO_LSMOD=1; run --out-dir "$T/out"; unset FAKE_NO_LSMOD
d=$(D)
check "falls back to /proc/modules" grep -q proc_modules_marker "$d/modules.txt"
check "lsmod failure recorded with exit 127" grep -q $'^modules.txt\tlsmod\tFAILED, exit 127$' "$d/status.txt"
rm -rf "$T/out"
export FAKE_HANG=1; FBC_TIMEOUT=1 run --out-dir "$T/out"; unset FAKE_HANG
d=$(D)
check "timeout: the run goes on, exit 0" test "$RC" = 0
check "timeout: battery.txt says it timed out" grep -q 'TIMED OUT after 1s' "$d/battery.txt"
check "timeout: status.txt and summary.txt say so" bash -c "grep -q $'^battery.txt\\tdumpsys battery\\tTIMED OUT after 1s\$' '$d/status.txt' && grep -q 'TIMED OUT after 1s: dumpsys battery' '$d/summary.txt'"
check "timeout: the other files are there" test -s "$d/wifi.txt"
check "bad FBC_TIMEOUT refused" bash -c "! FBC_TIMEOUT=0 ADB=adb_mock bash '$SCRIPT' --dry-run >/dev/null 2>&1"

echo "-- stalled discovery and a TERM-ignoring command end within the bound"
rm -rf "$T/out"
export FAKE_STALL_DEVICES=1; SECONDS=0; FBC_TIMEOUT=1 FBC_KILL_AFTER=1 run --out-dir "$T/out"; el=$SECONDS; unset FAKE_STALL_DEVICES
check "stalled adb devices: ends within timeout + grace" test "$el" -le 5
check "stalled adb devices: refused, says it timed out, nothing written" bash -c "test '$RC' != 0 && grep -q 'adb devices: TIMED OUT after 1s' <<<'$OUT' && test ! -e '$T/out'"
rm -rf "$T/out"
export FAKE_TERM_DEAF=1; SECONDS=0; FBC_TIMEOUT=1 FBC_KILL_AFTER=1 run --out-dir "$T/out"; el=$SECONDS; unset FAKE_TERM_DEAF
d=$(D)
check "TERM-ignoring command: the whole run ends within the bound" test "$el" -le 8
check "TERM-ignoring command: run goes on, exit 0" test "$RC" = 0
check "TERM-ignoring command: recorded as killed in file, status and summary" bash -c "grep -q 'TIMED OUT after 1s (killed after a 1s grace' '$d/battery.txt' && grep -q $'^battery.txt\\tdumpsys battery\\tTIMED OUT after 1s (killed' '$d/status.txt' && grep -q '(killed after a 1s grace: it ignored TERM): dumpsys battery' '$d/summary.txt'"
check "bad FBC_KILL_AFTER refused" bash -c "! FBC_KILL_AFTER=0 ADB=adb_mock bash '$SCRIPT' --dry-run >/dev/null 2>&1"

echo "-- the final scan removes a leaking output"
# a copy of the script whose redaction does nothing: whatever it leaks must be caught
sed 's/^redact() .*/redact() { cat; }/' "$SCRIPT" >"$T/noredact.sh"
rm -rf "$T/out"
SCRIPT_RUN=$T/noredact.sh run --out-dir "$T/out"
unset SCRIPT_RUN
check "leak: exit non-zero" test "$RC" != 0
check "leak: says what was left" has 'left in the output after redaction'
check "leak: output directory removed" test -z "$(find "$T/out" -mindepth 1 2>/dev/null)"

echo "-- serial handling"
rm -rf "$T/out"
export FAKE_TWO=1; run --out-dir "$T/out"
check "two devices without --serial: refused" test "$RC" != 0
check "...nothing written" test ! -e "$T/out"
run --serial "$SER" --out-dir "$T/out"
check "two devices with --serial: ok" test "$RC" = 0
check "...-s given to every targeted adb command" bash -c "! grep -v '^serial=$SER ' '$T/adb.log' | grep -v '^serial=NONE devices *\$' | grep -q ."
rm -rf "$T/out"
run --serial OTHER0002 --out-dir "$T/out"
check "serial the stub does not know: refused, nothing written" bash -c "test '$RC' != 0 && test ! -e '$T/out'"
unset FAKE_TWO

echo "-- usage"
run --bogus
check "unknown option: usage, exit 2" test "$RC" = 2

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
