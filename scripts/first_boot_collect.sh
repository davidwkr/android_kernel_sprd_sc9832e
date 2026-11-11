#!/usr/bin/env bash
# First-boot data collection from a connected watch running a GSI, over adb.
# Read-only on the device. Usage and what to look at: devices/FIRST_BOOT.md.
# Usage: scripts/first_boot_collect.sh [--serial SERIAL] [--out-dir DIR] [--dry-run]
#   --serial SERIAL   adb serial (needed when more than one device is attached)
#   --out-dir DIR     parent of the new timestamped directory (default
#                     ./first_boot_collect); the directory must not exist yet
#   --dry-run         print the commands, touch no device, write nothing
# Env: ADB (default adb; the tests set it to a stub function)
# Device side: only 'adb shell' of the commands listed below (each is checked
# against a write/state-change blacklist at start-up), plus 'adb devices' and
# 'adb get-state'. No root needed; 'su -c dmesg' is tried only when plain dmesg
# is refused and su exists. Never remounts, pushes, installs, reboots, sets a
# property or setting.
# Privacy: the adb serial, serial/IMEI/IMSI/ICCID/MAC-like values, Wi-Fi SSIDs
# and latitude/longitude pairs are replaced by placeholders before anything is
# written (the raw output only lives in a shell variable and a pipe).
set -euo pipefail

ADB=${ADB:-adb}
SERIAL= OUT_BASE=first_boot_collect DRY=0

die() { echo "FAIL: $*" >&2; exit 1; }
say() { echo "== $*"; }
usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --serial) [ $# -ge 2 ] || usage; SERIAL=$2; shift ;;
    --out-dir) [ $# -ge 2 ] || usage; OUT_BASE=$2; shift ;;
    --dry-run) DRY=1 ;;
    -h|--help) usage ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
  shift
done

# --- what is collected: file|command (run by the device shell, read only) ------
CMDS=(
  'getprop.txt|getprop'
  'uname.txt|uname -a'
  'cmdline.txt|cat /proc/cmdline'
  'version.txt|cat /proc/version'
  'modules.txt|lsmod 2>/dev/null || cat /proc/modules'
  'logcat.txt|logcat -d -b all'
  'meminfo.txt|cat /proc/meminfo'
  'pressure.txt|if [ -d /proc/pressure ]; then for f in /proc/pressure/*; do echo "== $f"; cat "$f"; done; else echo "/proc/pressure absent (no PSI)"; fi'
  'surfaceflinger.txt|dumpsys SurfaceFlinger'
  'gfxinfo.txt|dumpsys gfxinfo | grep -E "^\*\*|Total frames|Janky frames|percentile|Number (Missed|High|Slow|Frame)" | head -n 200'
  'battery.txt|dumpsys battery'
  'location.txt|dumpsys location'
  'wifi.txt|dumpsys wifi | head -n 80'
  'bluetooth.txt|dumpsys bluetooth_manager | head -n 80'
  'mount.txt|mount'
  'devnodes.txt|ls -lZ /dev/mali0 /dev/adf* /dev/gsp* 2>&1'
  'getenforce.txt|getenforce'
  'pstore.txt|ls -l /sys/fs/pstore 2>&1; for f in /sys/fs/pstore/*; do [ -f "$f" ] && { echo "== $f"; cat "$f"; }; done; true'
  'debugfs.txt|if [ -d /sys/kernel/debug ]; then ls /sys/kernel/debug | head -n 50; else echo "/sys/kernel/debug absent"; fi'
)
# dmesg is separate: plain first, 'su -c dmesg' only if that is refused
DMESG_CMD='dmesg'
DMESG_SU='command -v su >/dev/null 2>&1 && su -c dmesg'

# nothing that writes or changes state: no redirection other than 2>&1 and
# 2>/dev/null, no write/mount/set command
safe_cmd() {
  local c=$1 t
  t=${c//2>&1/}
  t=${t//2>\/dev\/null/}
  t=${t//>\/dev\/null/}
  case $t in *'>'*|*'<'*) return 1 ;; esac
  [[ $t =~ (^|[^A-Za-z0-9_])(rm|mv|cp|dd|tee|chmod|chown|chcon|mkdir|touch|ln|mount\ -o|umount|remount|setprop|settings\ put|svc|am|pm|reboot|stop|start|kill|insmod|rmmod|sysctl)([^A-Za-z0-9_]|$) ]] && return 1
  return 0
}
for e in "${CMDS[@]}" "x|$DMESG_CMD" "x|$DMESG_SU"; do
  safe_cmd "${e#*|}" || die "command is not read-only: ${e#*|}"
done

adb_() { "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }
dev() { adb_ shell "$1" </dev/null | tr -d '\r'; }

if [ "$DRY" -eq 1 ]; then
  say "DRY RUN: no device access, nothing written. Commands (adb ${SERIAL:+-s <SERIAL> }shell '...'):"
  for e in "${CMDS[@]}"; do printf '  %-18s %s\n' "${e%%|*}" "${e#*|}"; done
  printf '  %-18s %s\n' dmesg.txt "$DMESG_CMD   (if refused: $DMESG_SU)"
  echo "  summary.txt        built on the host from the files above"
  echo "output: $OUT_BASE/<YYYYmmdd-HHMMSS>/ (redacted: serial, IMEI/IMSI/ICCID, MAC, SSID, lat/lng)"
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- device ------------------------------------------------------------------
if [ -z "$SERIAL" ]; then
  list=$("$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="device"{print $1}')
  n=$(printf '%s\n' "$list" | sed '/^$/d' | wc -l)
  [ "$n" -eq 1 ] || die "$n adb devices attached, pass --serial"
  SERIAL=$list
fi
adb_ get-state >/dev/null 2>&1 || die "no adb device in state 'device'"

# --- redaction ---------------------------------------------------------------
esc() { printf '%s' "$1" | sed 's/[][\.*^$/|+?(){}]/\\&/g'; }
# sed script: the exact values first (adb serial, serial/IMEI/... properties of
# this device), then patterns for what the exact values do not cover
build_redactor() { # $1: getprop output
  local s=$WORK/redact.sed k v
  : >"$s"
  echo "s/$(esc "$SERIAL")/<SERIAL>/g" >>"$s"
  while IFS= read -r line; do
    [[ $line =~ ^\[([^]]*)\]:\ \[([^]]*)\]$ ]] || continue
    k=${BASH_REMATCH[1]} v=${BASH_REMATCH[2]}
    [[ $k =~ (serial|imei|meid|imsi|iccid|mac|bdaddr|btaddr) ]] || continue
    [ "${#v}" -ge 6 ] && [[ $v =~ [0-9A-Fa-f] ]] || continue
    echo "s/$(esc "$v")/<REDACTED>/g" >>"$s"
  done <<<"$1"
  cat >>"$s" <<'EOF'
s/^(\[[^]]*(imei|meid|imsi|iccid|serial|mac|bdaddr|btaddr)[^]]*\]: \[)[^]]*\]/\1<REDACTED>]/I
s/([A-Za-z0-9_.-]*(serialno|serial|imei|meid|imsi|iccid|mac|bdaddr)[A-Za-z0-9_.-]*=)[^ ,;]+/\1<REDACTED>/gI
s/([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}/<MAC>/g
s/((imei|meid|imsi|iccid|subscriber ?id|device ?id)[^0-9]{0,24})[0-9]{14,20}/\1<ID>/gI
s/(^|[^0-9])89[0-9]{17,18}([^0-9]|$)/\1<ICCID>\2/g
s/-?[0-9]{1,3}\.[0-9]{4,}, ?-?[0-9]{1,3}\.[0-9]{4,}/<LATLON>/g
s/(SSID[=:] ?)("[^"]*"|[^,]*)/\1<SSID>/gI
EOF
}
redact() { sed -E -f "$WORK/redact.sed"; }

# --- collect -----------------------------------------------------------------
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$OUT_BASE/$STAMP
[ ! -e "$OUT" ] || die "$OUT exists"
# the properties are read first (in memory) to learn the values to redact
PROPS=$(dev getprop) || die "getprop failed"
[ -n "$PROPS" ] || die "getprop returned nothing: is the device booted?"
build_redactor "$PROPS"
mkdir -p "$OUT"
say "collecting into $OUT"
printf '%s\n' "$PROPS" | redact >"$OUT/getprop.txt"
for e in "${CMDS[@]}"; do
  f=${e%%|*}
  [ "$f" != getprop.txt ] || continue
  echo "  $f"
  { dev "${e#*|}" 2>&1 || echo "(command failed)"; } | redact >"$OUT/$f"
done
echo "  dmesg.txt"
d=$(dev "$DMESG_CMD" 2>&1 || true)
if [ -z "$d" ] || grep -qiE 'not permitted|permission denied|klogctl' <<<"$d"; then
  d2=$(dev "$DMESG_SU" 2>&1 || true)
  if [ -n "$d2" ]; then d=$d2; else d="${d:-"(dmesg empty)"}
(no su, or su refused)"; fi
fi
printf '%s\n' "$d" | redact >"$OUT/dmesg.txt"

# --- summary -----------------------------------------------------------------
prop() { sed -n "s/^\[$1\]: \[\(.*\)\]\$/\1/p" "$OUT/getprop.txt" | head -n 1; }
grep_sec() { # title, file, ERE
  local n
  n=$(grep -ciE -- "$3" "$OUT/$2" || true)
  echo "-- $1 ($2): $n line(s)"
  grep -iE -- "$3" "$OUT/$2" | head -n 20 || true
}
{
  echo "first_boot_collect summary, $STAMP"
  echo "PRIVACY: the adb serial, serial/IMEI/IMSI/ICCID/MAC-like property values, MAC addresses,"
  echo "Wi-Fi SSIDs and latitude/longitude pairs were replaced by placeholders (<SERIAL>, <MAC>,"
  echo "<REDACTED>, <ID>, <ICCID>, <SSID>, <LATLON>) before any file here was written. Other"
  echo "content (app names, package lists in logcat) is as the device printed it: check before sharing."
  echo
  echo "uname:        $(cat "$OUT/uname.txt")"
  echo "getenforce:   $(head -n 1 "$OUT/getenforce.txt")"
  for p in ro.hardware ro.build.version.release ro.build.version.sdk ro.config.low_ram persist.sys.vndk \
           persist.device_config.activity_manager_native_boot.use_freezer ro.ramsize ro.run.ramconfig; do
    echo "$p: $(prop "$p")"
  done
  echo "PSI:          $(head -n 1 "$OUT/pressure.txt")"
  echo "pstore:       $(grep -c '^==' "$OUT/pstore.txt" || true) file(s)"
  echo
  grep_sec 'mali warnings' dmesg.txt 'mali.*(warn|err|fail|fault|timeout|hang|reset)|(warn|err|fail|fault|timeout).*mali'
  grep_sec 'Kernel panic' dmesg.txt 'Kernel panic'
  grep_sec 'BUG' dmesg.txt 'BUG'
  grep_sec 'Oops' dmesg.txt 'Oops'
  grep_sec 'avc: denied' dmesg.txt 'avc: *denied'
  grep_sec 'avc: denied, logcat' logcat.txt 'avc: *denied'
  grep_sec 'lmkd (which interface it chose)' logcat.txt 'lmkd'
} >"$OUT/summary.txt"

# --- the redaction is checked on what was written ------------------------------
bad=
grep -rEq '([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}' "$OUT" && bad="MAC-like value"
grep -rqF -- "$SERIAL" "$OUT" && bad="adb serial"
if [ -n "$bad" ]; then
  rm -rf "$OUT"
  die "$bad left in the output after redaction: output removed, nothing kept"
fi
say "done: $OUT (read summary.txt first)"
