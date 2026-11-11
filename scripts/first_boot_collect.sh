#!/usr/bin/env bash
# First-boot data collection from a connected watch running a GSI, over adb.
# Read-only on the device. Usage and what to look at: devices/FIRST_BOOT.md.
# Usage: scripts/first_boot_collect.sh [--serial SERIAL] [--out-dir DIR] [--root] [--dry-run]
#   --serial SERIAL   adb serial (needed when more than one device is attached)
#   --out-dir DIR     parent of the new timestamped directory (default
#                     ./first_boot_collect); the directory must not exist yet
#   --root            if plain dmesg is refused, try 'su -c dmesg' (default: no
#                     su at all, dmesg is recorded as unavailable)
#   --dry-run         print the commands, touch no device, write nothing
# Env: ADB (default adb; the tests set it to a stub function)
#      FBC_TIMEOUT seconds per adb call (default 60)
#      FBC_KILL_AFTER seconds after that TERM before KILL (default 5), for a
#      command that ignores TERM
# Device side: only 'adb shell' of single commands (no pipe, redirection, ';',
# '&&' or substitution: each producer runs alone and is filtered and truncated
# on the host; each is checked against a write/state-change blacklist at
# start-up), plus 'adb devices' and 'adb get-state'. Every adb call is bounded
# by timeout -k; its exit status is recorded (status.txt, summary.txt). Never
# remounts, pushes, installs, reboots, sets a property or setting.
# Privacy: the sensitive dumps (location, wifi, bluetooth, battery) are reduced
# to an allowlist of diagnostic lines, never stored raw; logcat is warnings and
# above plus a few tags; everything is redacted on the host before it is written
# and the output is scanned again afterwards. The output still must be reviewed
# before it is shared.
set -euo pipefail

ADB=${ADB:-adb}
TMO=${FBC_TIMEOUT:-60}
KILLAFTER=${FBC_KILL_AFTER:-5}
SERIAL= OUT_BASE=first_boot_collect DRY=0 ROOT=0

die() { echo "FAIL: $*" >&2; exit 1; }
say() { echo "== $*"; }
usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --serial) [ $# -ge 2 ] || usage; SERIAL=$2; shift ;;
    --out-dir) [ $# -ge 2 ] || usage; OUT_BASE=$2; shift ;;
    --root) ROOT=1 ;;
    --dry-run) DRY=1 ;;
    -h|--help) usage ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
  shift
done
[[ $TMO =~ ^[0-9]+$ ]] && [ "$TMO" -gt 0 ] || die "FBC_TIMEOUT must be a positive number of seconds"
[[ $KILLAFTER =~ ^[0-9]+$ ]] && [ "$KILLAFTER" -gt 0 ] || die "FBC_KILL_AFTER must be a positive number of seconds"

# --- what is collected: file|host filter|command (one device command, read only) -
LOGCAT_TAGS="SurfaceFlinger:I hwcomposer:I mali:I gralloc:I lmkd:I init:I vold:I keystore2:I GnssHAL:I gps:I LocationManager:I wifi:I WifiService:I bluetooth:I auditd:I SELinux:W"
CMDS=(
  'getprop.txt|all|getprop'
  'uname.txt|all|uname -a'
  'cmdline.txt|all|cat /proc/cmdline'
  'version.txt|all|cat /proc/version'
  'modules.txt|all|lsmod'
  "logcat.txt|logcat|logcat -d -b all $LOGCAT_TAGS '*:W'"
  'meminfo.txt|all|cat /proc/meminfo'
  'surfaceflinger.txt|sf|dumpsys SurfaceFlinger'
  'gfxinfo.txt|gfx|dumpsys gfxinfo'
  'battery.txt|battery|dumpsys battery'
  'location.txt|location|dumpsys location'
  'wifi.txt|wifi|dumpsys wifi'
  'bluetooth.txt|bt|dumpsys bluetooth_manager'
  'mount.txt|all|mount'
  'devnodes.txt|all|ls -lZ /dev/mali0 /dev/adf* /dev/gsp*'
  'getenforce.txt|all|getenforce'
  'debugfs.txt|head50|ls /sys/kernel/debug'
)
# collected by code below, each as separate commands
EXTRA_CMDS=('cat /proc/modules' 'dmesg' 'su -c dmesg' 'ls /proc/pressure' 'ls -l /sys/fs/pstore')

# nothing that writes or changes state, and nothing that is a pipeline or a
# redirection: one command, its output is handled on the host
safe_cmd() {
  local c=$1
  case $c in
    *'|'*|*';'*|*'&'*|*'<'*|*'>'*|*'$'*|*'`'*|*'('*|*')'*|*'{'*|*'}'*|*$'\n'*) return 1 ;;
  esac
  [[ $c =~ (^|[^A-Za-z0-9_])(rm|mv|cp|dd|tee|chmod|chown|chcon|mkdir|touch|ln|mount\ -o|umount|remount|setprop|settings\ put|svc|am|pm|reboot|stop|start|kill|insmod|rmmod|sysctl)([^A-Za-z0-9_]|$) ]] && return 1
  if [[ $c =~ (^|[^A-Za-z0-9_])su([^A-Za-z0-9_]|$) ]]; then [ "$c" = 'su -c dmesg' ] || return 1; fi
  return 0
}
for e in "${CMDS[@]}"; do safe_cmd "${e##*|}" || die "command is not read-only: ${e##*|}"; done
for e in "${EXTRA_CMDS[@]}"; do safe_cmd "$e" || die "command is not read-only: $e"; done

adb_() { timeout -k "$KILLAFTER" "$TMO" bash -c '"$@"' fbc "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }

if [ "$DRY" -eq 1 ]; then
  say "DRY RUN: no device access, nothing written. Commands (adb ${SERIAL:+-s <SERIAL> }shell '...', each alone, timeout ${TMO}s):"
  for e in "${CMDS[@]}"; do f=${e#*|}; printf '  %-18s %-9s %s\n' "${e%%|*}" "${f%%|*}" "${e##*|}"; done
  printf '  %-18s %s\n' modules.txt "(if lsmod fails: cat /proc/modules)"
  if [ "$ROOT" -eq 1 ]; then
    printf '  %-18s %s\n' dmesg.txt "dmesg   (if refused: su -c dmesg)"
  else
    printf '  %-18s %s\n' dmesg.txt "dmesg   (if refused: unavailable, no su without --root)"
  fi
  printf '  %-18s %s\n' pressure.txt "ls /proc/pressure, then cat of each file"
  printf '  %-18s %s\n' pstore.txt "ls -l /sys/fs/pstore, then cat of each file"
  echo "  status.txt, summary.txt  built on the host"
  echo "output: $OUT_BASE/<YYYYmmdd-HHMMSS>/ (sensitive dumps reduced to an allowlist, then redacted)"
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- device ------------------------------------------------------------------
if [ -z "$SERIAL" ]; then
  rc=0
  devs=$(adb_ devices 2>/dev/null </dev/null) || rc=$?
  case $rc in
    0) ;;
    124) die "adb devices: TIMED OUT after ${TMO}s" ;;
    137) die "adb devices: TIMED OUT after ${TMO}s (killed after a ${KILLAFTER}s grace: it ignored TERM)" ;;
    *) die "adb devices: FAILED, exit $rc" ;;
  esac
  list=$(printf '%s\n' "$devs" | tr -d '\r' | awk 'NR>1 && $2=="device"{print $1}')
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
    [[ $k =~ (serial|imei|meid|imsi|iccid|mac|bdaddr|btaddr|msisdn) ]] || continue
    [ "${#v}" -ge 6 ] && [[ $v =~ [0-9A-Fa-f] ]] || continue
    echo "s/$(esc "$v")/<REDACTED>/g" >>"$s"
  done <<<"$1"
  cat >>"$s" <<'EOF'
s/^(\[[^]]*(imei|meid|esn|imsi|iccid|serial|mac|bdaddr|btaddr|msisdn|phone)[^]]*\]: \[)[^]]*\]/\1<REDACTED>]/I
s/([A-Za-z0-9_.-]*(serialno|serial|imei|meid|imsi|iccid|mac|bdaddr)[A-Za-z0-9_.-]*=)[^ ,;]+([ ,;]+[A-Za-z]*[0-9][A-Za-z0-9]{5,})*/\1<REDACTED>/gI
s/(serial[A-Za-z0-9_ .-]{0,12}[:=] *"?)[^ ,;"]+([ ,;]+[A-Za-z]*[0-9][A-Za-z0-9]{5,})*/\1<SERIAL>/gI
s/(serial[ ]+)[A-Za-z]*[0-9][A-Za-z0-9]{5,}([ ,;]+[A-Za-z]*[0-9][A-Za-z0-9]{5,})*/\1<SERIAL>/gI
s/(Account ?\{name=)[^,}]*/\1<EMAIL>/gI
s/[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}/<EMAIL>/g
s/([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}/<MAC>/g
s/((imei|meid|esn|imsi|iccid|msisdn|phone ?number|subscriber ?id|device ?id)[A-Za-z0-9_.-]*[=: ]+"?)[0-9A-Za-z]{6,}([ ,;\/]+[A-Za-z]*[0-9][0-9A-Za-z]{5,})*/\1<ID>/gI
s/\b(0x)?[0-9A-Fa-f]{16}\b/<KADDR>/g
:iccid
s/(^|[^0-9])89[0-9]{17,18}([^0-9]|$)/\1<ICCID>\2/g
t iccid
:imei
s/(^|[^0-9])[0-9]{14,15}([^0-9]|$)/\1<ID>\2/g
t imei
:meid
s/(^|[^0-9A-Za-z])[0-9A-Fa-f]{14}([^0-9A-Za-z]|$)/\1<ID>\2/g
t meid
s/Location\[[^]]*\]/Location[<LATLON>]/g
s/\b(lat(itude)?|lng|lon|long(itude)?)[ =:]+-?[0-9]+(\.[0-9]+)?/\1=<LATLON>/gI
s/-?[0-9]{1,3}\.[0-9]{4,}([ ,;\/]+-?[0-9]{1,3}\.[0-9]{4,})+/<LATLON>/g
s/(ssid[A-Za-z_]*[=: ]+).*/\1<SSID>/I
s/\+[0-9][0-9 ().-]{6,}[0-9]/<PHONE>/g
s/\(?[0-9]{2,4}\)?[ .-][0-9]{3,4}[ .-][0-9]{3,4}/<PHONE>/g
:phone
s/(^|[^0-9])[0-9]{3}-[0-9]{4}([^0-9]|$)/\1<PHONE>\2/g
t phone
EOF
}
redact() { sed -E -f "$WORK/redact.sed"; }

# --- host filters (stdin -> stdout) --------------------------------------------
f_all() { cat; }
f_head50() { head -n 50; }
f_logcat() { tail -n 3000; }
f_sf() { head -n 600; }
f_gfx() { { grep -E '^\*\*|Total frames|Janky frames|percentile|Number (Missed|High|Slow|Frame)' || true; } | head -n 200; }
# allowlist: lines matching $1 and not $2, at most 60
allow() { { grep -iE -e "$1" | grep -ivE -e "$2" || true; } | head -n 60; }
SENS='ssid|mac|address|bonded|paired|name|account|@|latitude|longitude|lat=|lng|location\[|[0-9]+\.[0-9]{3,}|[0-9]{1,3}(\.[0-9]{1,3}){3}'
f_battery() { allow '^[[:space:]]*(AC powered|USB powered|Wireless powered|status|health|present|level|scale|technology):' 'serial'; }
f_location() { allow '(enabled|disabled)|gnss|gps hal|\bhal\b|error|fail|exception' "$SENS"; }
f_wifi() {
  local raw
  raw=$(cat)
  printf 'connection: %s (derived from the dump, which is not stored)\n' \
    "$(grep -qiE '(state|mWifiInfo)[^,]*[=: ](CONNECTED|COMPLETED)|Wi-Fi is connected' <<<"$raw" && echo yes || echo no)"
  allow 'wi-?fi is |wifistate|wifi state|scanning|supplicant|\bhal\b|driver|firmware|error|fail|exception' "$SENS" <<<"$raw"
}
f_bt() {
  local raw
  raw=$(cat)
  printf 'connection: %s (derived from the dump, which is not stored)\n' \
    "$(grep -qiE 'STATE_CONNECTED|connected[=: ]+true' <<<"$raw" && echo yes || echo no)"
  allow '(enabled|disabled)|state[:=]|STATE_(ON|OFF|TURNING|BLE)|\bhal\b|error|fail|exception' "$SENS" <<<"$raw"
}

# --- run one device command ------------------------------------------------------
STATUS=()   # "file<TAB>command<TAB>result" per device command
RC=0 NOTE=
# stdout in $WORK/o, stderr in $WORK/e; RC is the exit status, NOTE says if it
# timed out or failed; the result is recorded under $2 (the output file name)
devrun() { # command, file
  safe_cmd "$1" || die "command is not read-only: $1"
  RC=0
  adb_ shell "$1" </dev/null >"$WORK/o.raw" 2>"$WORK/e.raw" || RC=$?
  tr -d '\r' <"$WORK/o.raw" >"$WORK/o"
  tr -d '\r' <"$WORK/e.raw" >"$WORK/e"
  case $RC in
    0) NOTE=ok ;;
    124) NOTE="TIMED OUT after ${TMO}s" ;;
    137) NOTE="TIMED OUT after ${TMO}s (killed after a ${KILLAFTER}s grace: it ignored TERM)" ;;
    *) NOTE="FAILED, exit $RC" ;;
  esac
  STATUS+=("$(printf '%s\t%s\t%s' "$2" "$1" "$NOTE")")
}
# the failure line goes first in the file, then what the command did print
failnote() { [ "$RC" -eq 0 ] || { echo "($1: $NOTE)"; head -n 5 "$WORK/e"; }; }

# --- collect -----------------------------------------------------------------
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$OUT_BASE/$STAMP
[ ! -e "$OUT" ] || die "$OUT exists"
# the properties are read first (in memory) to learn the values to redact
devrun getprop getprop.txt
[ "$RC" -eq 0 ] || die "getprop failed ($NOTE)"
[ -s "$WORK/o" ] || die "getprop returned nothing: is the device booted?"
PROPS=$(cat "$WORK/o")
build_redactor "$PROPS"
mkdir -p "$OUT"
say "collecting into $OUT"
printf '%s\n' "$PROPS" | redact >"$OUT/getprop.txt"
for e in "${CMDS[@]}"; do
  f=${e%%|*} filter=${e#*|} cmd=${e##*|}
  filter=${filter%%|*}
  [ "$f" != getprop.txt ] || continue
  echo "  $f"
  devrun "$cmd" "$f"
  if [ "$f" = modules.txt ] && [ "$RC" -ne 0 ]; then
    devrun 'cat /proc/modules' "$f"
  fi
  { failnote "$cmd"; "f_$filter" <"$WORK/o"; } | redact >"$OUT/$f"
done

echo "  dmesg.txt"
devrun dmesg dmesg.txt
if [ "$RC" -ne 0 ] || grep -qiE 'not permitted|permission denied|klogctl' "$WORK/o"; then
  if [ "$ROOT" -eq 1 ]; then
    devrun 'su -c dmesg' dmesg.txt
    { failnote 'su -c dmesg'; tail -n 4000 "$WORK/o"; } | redact >"$OUT/dmesg.txt"
  else
    echo "(dmesg unavailable: plain dmesg: $NOTE; re-run with --root to try su -c dmesg)" >"$OUT/dmesg.txt"
  fi
else
  tail -n 4000 "$WORK/o" | redact >"$OUT/dmesg.txt"
fi

echo "  pressure.txt"
devrun 'ls /proc/pressure' pressure.txt
if [ "$RC" -ne 0 ]; then
  echo "/proc/pressure absent (no PSI) ($NOTE)" | redact >"$OUT/pressure.txt"
else
  cp "$WORK/o" "$WORK/pnames"
  : >"$WORK/p"
  while read -r n; do
    [[ $n =~ ^[a-z]+$ ]] || continue
    devrun "cat /proc/pressure/$n" pressure.txt
    { echo "== /proc/pressure/$n"; failnote "cat /proc/pressure/$n"; cat "$WORK/o"; } >>"$WORK/p"
  done <"$WORK/pnames"
  redact <"$WORK/p" >"$OUT/pressure.txt"
fi

echo "  pstore.txt"
devrun 'ls -l /sys/fs/pstore' pstore.txt
{ echo "== ls -l /sys/fs/pstore"; failnote 'ls -l /sys/fs/pstore'; cat "$WORK/o"; } >"$WORK/p"
if [ "$RC" -eq 0 ]; then
  awk 'NF && $1 !~ /^total/ {print $NF}' "$WORK/o" >"$WORK/pnames"
  while read -r n; do
    [[ $n =~ ^[A-Za-z0-9_.-]+$ ]] || continue
    devrun "cat /sys/fs/pstore/$n" pstore.txt
    { echo "== /sys/fs/pstore/$n"; failnote "cat /sys/fs/pstore/$n"; head -n 200 "$WORK/o"; } >>"$WORK/p"
  done <"$WORK/pnames"
fi
redact <"$WORK/p" >"$OUT/pstore.txt"

# --- status and summary --------------------------------------------------------
printf '%s\n' "${STATUS[@]}" >"$OUT/status.txt"
nfail=$(grep -vc "$(printf '\tok$')" "$OUT/status.txt" || true)
prop() { sed -n "s/^\[$1\]: \[\(.*\)\]\$/\1/p" "$OUT/getprop.txt" | head -n 1; }
grep_sec() { # title, file, ERE
  local n
  n=$(grep -ciE -- "$3" "$OUT/$2" || true)
  echo "-- $1 ($2): $n line(s)"
  grep -iE -- "$3" "$OUT/$2" | head -n 20 || true
}
{
  echo "first_boot_collect summary, $STAMP"
  echo "REVIEW BEFORE SHARING: this was reduced and redacted by patterns, not proven clean. Collected:"
  echo "  getprop (identifier values redacted), kernel and module lists, meminfo, PSI, mounts,"
  echo "  device nodes, SELinux mode, pstore, dmesg (if readable), logcat warnings and above plus"
  echo "  tags SurfaceFlinger hwcomposer mali gralloc lmkd init vold keystore2 GnssHAL gps"
  echo "  LocationManager wifi bluetooth auditd, SurfaceFlinger and gfxinfo dumps (truncated);"
  echo "  location, wifi, bluetooth_manager and battery only as allowlisted state/error lines"
  echo "  (the raw dumps are not stored). Not collected: accounts, packages, settings, contacts."
  echo "  Redacted: adb serial, serial/IMEI/MEID/IMSI/ICCID values, MAC, e-mail/account names,"
  echo "  phone numbers, coordinates, Wi-Fi network names, kernel addresses (<SERIAL> <REDACTED> <ID> <ICCID>"
  echo "  <MAC> <EMAIL> <PHONE> <LATLON> <SSID> <KADDR>). <KADDR> is every 16-hex-digit value, 0x or not: raw"
  echo "  64-bit kernel addresses and %p-hashed pointers print alike, so both go; symbols, +offset/size,"
  echo "  timestamps and register names stay. Package and process names, and anything the patterns do not"
  echo "  recognise, are as the device printed them: read every file before sharing."
  echo
  echo "device commands: $(grep -c . "$OUT/status.txt"), not ok: $nfail (a failed or timed-out command says so at the top of its file)"
  awk -F'\t' '$3 != "ok" {print "  " $3 ": " $2 "  -> " $1}' "$OUT/status.txt"
  echo "  (exit status as reported by adb shell, timeout ${TMO}s per call; all in status.txt)"
  echo
  echo "uname:        $(cat "$OUT/uname.txt")"
  echo "getenforce:   $(head -n 1 "$OUT/getenforce.txt")"
  for p in ro.hardware ro.build.version.release ro.build.version.sdk ro.config.low_ram persist.sys.vndk \
           ro.vndk.version ro.ramsize ro.run.ramconfig; do
    echo "$p: $(prop "$p")"
  done
  echo "PSI:          $(head -n 1 "$OUT/pressure.txt")"
  echo "pstore:       $(grep -c '^== /sys/fs/pstore/' "$OUT/pstore.txt" || true) file(s)"
  echo
  grep_sec 'mali warnings' dmesg.txt 'mali.*(warn|err|fail|fault|timeout|hang|reset)|(warn|err|fail|fault|timeout).*mali'
  grep_sec 'Kernel panic' dmesg.txt 'Kernel panic'
  grep_sec 'BUG' dmesg.txt 'BUG'
  grep_sec 'Oops' dmesg.txt 'Oops'
  grep_sec 'avc: denied' dmesg.txt 'avc: *denied'
  grep_sec 'avc: denied, logcat' logcat.txt 'avc: *denied'
  grep_sec 'lmkd (which interface it chose)' logcat.txt 'lmkd'
} >"$OUT/summary.txt"

# --- the redaction is checked on what was written, every category ------------------
# independent of the sed script: a category the script misses is caught here
bad=
leak() { grep -rEqi -e "$2" "$OUT" && bad="$bad ${1}," || true; }
leak 'e-mail' '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z]{2,}'
leak 'MAC-like value' '([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}'
leak 'IMEI/IMSI-like digits' '(^|[^0-9])[0-9]{14,15}([^0-9]|$)'
leak 'ICCID-like digits' '(^|[^0-9])89[0-9]{17,18}([^0-9]|$)'
leak 'kernel address' '(0x)?ffff[0-9a-f]{12}([^0-9a-f]|$)|(^|[^0-9A-Za-z])(0x)?[0-9a-f]{16}([^0-9A-Za-z]|$)'
leak 'MEID-like hex' '(^|[^0-9A-Za-z])[0-9A-F]{14}([^0-9A-Za-z]|$)'
leak 'coordinates' '-?[0-9]{1,3}\.[0-9]{4,}[ ,;/]+-?[0-9]{1,3}\.[0-9]{4,}|Location\[[a-z]+ [-0-9]'
leak 'latitude/longitude value' '(latitude|longitude|lat|lng)[ =:]+-?[0-9]'
leak 'phone number' '\+[0-9][0-9 ().-]{6,}[0-9]|(^|[^0-9.])[0-9]{2,4}[ -][0-9]{3,4}[ -][0-9]{3,4}([^0-9]|$)'
leak 'serial value' 'serial[A-Za-z0-9_ .-]{0,12}[:=] *"?[^<[:space:]"]'
leak 'serial value' 'serial +[A-Za-z]*[0-9][A-Za-z0-9]{5,}'
leak 'SSID' 'ssid[A-Za-z_]*[=: ]+[^<[:space:]]'
if grep -rqF -- "$SERIAL" "$OUT"; then bad="$bad adb serial,"; fi
while IFS= read -r line; do
  [[ $line =~ ^\[([^]]*)\]:\ \[([^]]*)\]$ ]] || continue
  v=${BASH_REMATCH[2]}
  [[ ${BASH_REMATCH[1]} =~ (serial|imei|meid|imsi|iccid|mac|bdaddr|btaddr|msisdn) ]] || continue
  [ "${#v}" -ge 6 ] && [[ $v =~ [0-9A-Fa-f] ]] || continue
  ! grep -rqF -- "$v" "$OUT" || bad="$bad identifier property value,"
done <<<"$PROPS"
if [ -n "$bad" ]; then
  rm -rf "$OUT"
  die "left in the output after redaction:${bad%,}: output removed, nothing kept"
fi
say "done: $OUT (read summary.txt first; review every file before sharing)"
