#!/usr/bin/env bash
# Install the vendor GSI patch files (devices/DEVICE/vendor_gsi/) into /vendor
# of a connected watch, over adb with root. Rationale: devices/VENDOR_GSI_PATCH.md.
# Usage: scripts/vendor_gsi_patch.sh --device DEVICE [--apply] [options]
#   --device DEVICE     dm63 | dw100u-silver (selects the file set)
#   --dry-run           default: read-only probes on the device, prints the plan
#   --apply             write: backup of the vendor partition first, then install
#   --serial SERIAL     adb serial (needed when more than one device is attached)
#   --backup-dir DIR    default ./vendor_gsi_backup (the dump needs the size of
#                       the vendor partition free on the host)
#   --modules-dir DIR   host dir with sprdbt_tty.ko sprd_fm.ko sprdwl_ng.ko
#                       (and mali.ko for a kernel that builds it as a module);
#                       installed to /vendor/lib/modules. Not on the device: the
#                       stock copies live in the boot image ramdisk.
#   --vendor-blk PATH   by-name path of the vendor partition (default: detect);
#                       must end in /by-name/vendor
# Env: ADB (default adb; the tests set it to a stub function)
# Root: adbd already root, Magisk (su -c CMD, DW100U) or the vendor su
#   (su 0 sh -c CMD, DM63); detected. Magisk asks for a grant on the watch.
# Touches only the vendor partition: reads it (dd), and writes files under
# /vendor through its mount point. No other partition is written, nothing in
# /data (files are streamed with adb exec-in, no adb push), no disable-verity.
set -euo pipefail

ADB=${ADB:-adb}
APPLY=0 DEVICE= SERIAL= BACKUP_DIR=vendor_gsi_backup MODULES_DIR= VENDOR_BLK=
SU=adbd
BLOCKERS=()

die() { echo "FAIL: $*" >&2; exit 1; }
say() { echo "== $*"; }
blocker() { BLOCKERS+=("$*"); echo "BLOCKER: $*"; }
usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --apply) APPLY=1 ;;
    --dry-run) APPLY=0 ;;
    --device) [ $# -ge 2 ] || usage; DEVICE=$2; shift ;;
    --serial) [ $# -ge 2 ] || usage; SERIAL=$2; shift ;;
    --backup-dir) [ $# -ge 2 ] || usage; BACKUP_DIR=$2; shift ;;
    --modules-dir) [ $# -ge 2 ] || usage; MODULES_DIR=$2; shift ;;
    --vendor-blk) [ $# -ge 2 ] || usage; VENDOR_BLK=$2; shift ;;
    -h|--help) usage ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
  shift
done
case $DEVICE in dm63|dw100u-silver) ;; *) usage ;; esac

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/devices/$DEVICE/vendor_gsi
for f in etc/init/hw/init.sl8541e_1h10.rc etc/fstab.sl8541e_1h10 ueventd.rc; do
  [ -f "$SRC/$f" ] || die "missing $SRC/$f"
done
RW=0
WORK=$(mktemp -d)
# a failed run must not leave /vendor read-write: RW stays 1 until read-only is
# verified in /proc/mounts, so any exit before that retries it once here
cleanup() {
  if [ "$RW" -ne 0 ]; then
    remount ro || echo "WARNING: /vendor is still read-write (remount ro failed): reboot the watch" >&2
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# --- device access -----------------------------------------------------------
adb_() { "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }
# single-quote a string for the device shell
sq() { local s=${1//\'/\'\\\'\'}; printf "'%s'" "$s"; }
# the command line that runs $1 as root in the detected mode
wrap() {
  case $SU in
    adbd) printf '%s' "$1" ;;
    magisk) printf 'su -c %s' "$(sq "$1")" ;;
    aosp) printf 'su 0 sh -c %s' "$(sq "$1")" ;;
  esac
}
rsh() { adb_ shell "$(wrap "$1")" </dev/null | tr -d '\r'; }
# run $1, success only if it exits 0 (old adb does not pass the exit code on, so
# the shell echoes it); output in RSH_OUT
rsh_ok() {
  local out rc
  out=$(rsh "($1) 2>&1; echo __RC=\$?") || true
  rc=$(printf '%s\n' "$out" | sed -n 's/^__RC=\([0-9]*\)$/\1/p' | tail -n 1)
  RSH_OUT=$(printf '%s\n' "$out" | sed '/^__RC=[0-9]*$/d')
  [ "$rc" = 0 ]
}
rexec_out() { adb_ exec-out "$(wrap "$1")" </dev/null; }
rexec_in() { adb_ exec-in "$(wrap "$1")"; }
# every write goes to a file under /vendor; nothing else is accepted
vendor_path() {
  case $1 in
    /vendor/*) ;;
    *) die "refusing to write outside /vendor: $1" ;;
  esac
  [[ $1 =~ ^/vendor/[A-Za-z0-9_./-]+$ ]] && [[ $1 != *..* ]] || die "refusing odd path: $1"
}

# the device resolves PATH (or, if it does not exist yet, its nearest existing
# ancestor) to a place under /vendor: no symlink in the way leads out of it
resolves_under_vendor() {
  local real
  real=$(rsh "p=$1; while [ ! -e \"\$p\" ] && [ ! -L \"\$p\" ]; do p=\$(dirname \"\$p\"); done; readlink -f \"\$p\"" | tail -n 1)
  case $real in
    /vendor|/vendor/*) ;;
    *) die "$1 resolves to '$real', outside /vendor: refusing" ;;
  esac
}
# before writing DEST: parent stays under /vendor; DEST, DEST.new and
# DEST.pregsi are not symlinks; DEST.new does not exist
target_ok() {
  resolves_under_vendor "$(dirname "$1")"
  rsh_ok "[ ! -L $1 ] && [ ! -L $1.pregsi ] && [ ! -e $1.new ] && [ ! -L $1.new ]" \
    || die "$1, $1.new or $1.pregsi is a symlink, or $1.new exists: refusing"
}

pick_serial() {
  [ -z "$SERIAL" ] || return 0
  local list n
  list=$("$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="device"{print $1}')
  n=$(printf '%s\n' "$list" | sed '/^$/d' | wc -l)
  [ "$n" -eq 1 ] || die "$n adb devices attached, pass --serial"
  SERIAL=$list
}

detect_su() {
  local m order
  case $DEVICE in
    dw100u-silver) order="adbd magisk aosp" ;;
    *) order="adbd aosp magisk" ;;
  esac
  for m in $order; do
    SU=$m
    [ "$m" = adbd ] || echo "probing root via $m (approve the su prompt on the watch if shown)"
    case $(rsh id 2>/dev/null || true) in *uid=0*) echo "root: $SU"; return 0 ;; esac
  done
  die "no root: tried adbd root, Magisk su -c and vendor su 0 sh -c"
}

# --- SELinux labels ----------------------------------------------------------
FC=
dev_label() { rsh "ls -Zd $1 2>/dev/null" | awk 'NR==1{print $1}' || true; }
# label for $1 from the device's own vendor file_contexts (regex, optional file
# type, context; the longest matching regex wins, a later one on a tie), else $2
fc_label() {
  local path=$1 best= bl=-1 re t ctx pat
  while read -r re t ctx _; do
    case $re in ''|'#'*) continue ;; esac
    if [ -z "$ctx" ]; then ctx=$t; t=; fi
    case $t in ''|--) ;; *) continue ;; esac
    [[ $ctx =~ ^u:object_r:[a-z0-9_]+:s0$ ]] || continue
    pat="^($re)\$"
    if [[ $path =~ $pat ]] && [ "${#re}" -ge "$bl" ]; then best=$ctx; bl=${#re}; fi
  done <<<"$FC"
  printf '%s' "${best:-$2}"
}
label_etc() { fc_label "$1" u:object_r:vendor_configs_file:s0; }
label_ueventd() {
  local fb
  fb=$(dev_label /vendor/build.prop)
  fc_label "$1" "${fb:-u:object_r:vendor_file:s0}"
}
label_other() {
  local fb
  fb=$(dev_label "$(dirname "$1")")
  fc_label "$1" "${fb:-u:object_r:vendor_file:s0}"
}

# --- plan --------------------------------------------------------------------
PLAN=()  # kind|host file|device path|mode
MODS=(sprdbt_tty sprd_fm sprdwl_ng mali)
build_plan() {
  cp "$SRC/etc/init/hw/init.sl8541e_1h10.rc" "$WORK/init.rc"
  cp "$SRC/etc/fstab.sl8541e_1h10" "$WORK/fstab"
  cp "$SRC/ueventd.rc" "$WORK/ueventd.rc"
  local m
  MOD_FOUND=()
  if [ -n "$MODULES_DIR" ]; then
    [ -d "$MODULES_DIR" ] || die "no such dir: $MODULES_DIR"
    for m in "${MODS[@]}"; do
      [ -f "$MODULES_DIR/$m.ko" ] || continue
      [ -s "$MODULES_DIR/$m.ko" ] || die "$m.ko is empty"
      [ "$(head -c 4 "$MODULES_DIR/$m.ko" | od -An -tx1 | tr -d ' ')" = 7f454c46 ] || die "$m.ko is not an ELF file"
      MOD_FOUND+=("$m")
      echo "module $m.ko: $(grep -a -o 'vermagic=[^[:cntrl:]]*' "$MODULES_DIR/$m.ko" | head -n 1)"
    done
  fi
  # mali.ko installed: switch on the commented insmod
  case " ${MOD_FOUND[*]:-} " in
    *" mali "*) sed -i 's|^    #mali-module: |    |' "$WORK/init.rc" ;;
  esac
  PLAN=("etc|$WORK/init.rc|/vendor/etc/init/hw/init.sl8541e_1h10.rc|0644"
        "etc|$WORK/fstab|/vendor/etc/fstab.sl8541e_1h10|0644"
        "ueventd|$WORK/ueventd.rc|/vendor/ueventd.rc|0644")
  for m in "${MOD_FOUND[@]:-}"; do
    [ -n "$m" ] || continue
    PLAN+=("other|$MODULES_DIR/$m.ko|/vendor/lib/modules/$m.ko|0644")
  done
  for m in sprdbt_tty sprd_fm sprdwl_ng; do
    case " ${MOD_FOUND[*]:-} " in
      *" $m "*) ;;
      *) echo "WARNING: $m.ko not supplied (--modules-dir): its insmod in the rc will fail unless it is already in /vendor/lib/modules" ;;
    esac
  done
}

# --- preflight (read only) ---------------------------------------------------
VENDOR_SRC= VENDOR_FS= VENDOR_OPTS= SIZE=
vendor_mount() {
  local l
  l=$(rsh 'cat /proc/mounts' | awk '$2=="/vendor"{s=$1; t=$3; o=$4} END{if (s) print s, t, o}')
  [ -n "$l" ] || return 1
  read -r VENDOR_SRC VENDOR_FS VENDOR_OPTS <<<"$l"
}
vendor_is_rw() { vendor_mount && case ",$VENDOR_OPTS," in *,rw,*) return 0 ;; esac; return 1; }

preflight() {
  local hw cand need avail
  say "preflight (read only)"
  hw=$(rsh 'getprop ro.hardware' | head -n 1)
  [ "$hw" = sl8541e_1h10 ] || die "ro.hardware is '$hw', expected sl8541e_1h10: wrong device?"
  vendor_mount || die "/vendor is not mounted"
  echo "/vendor: $VENDOR_SRC $VENDOR_FS ($VENDOR_OPTS)"

  if [ -n "$VENDOR_BLK" ]; then cand=$VENDOR_BLK; else
    cand=$(rsh 'for p in /dev/block/platform/*/*/*/by-name/vendor /dev/block/platform/*/*/by-name/vendor /dev/block/platform/*/by-name/vendor /dev/block/by-name/vendor; do [ -e "$p" ] && { echo "$p"; break; }; done' | head -n 1)
  fi
  [[ $cand =~ ^/dev/block/[A-Za-z0-9_.:/-]+/by-name/vendor$ ]] && [[ $cand != *..* ]] \
    || die "vendor partition not found or not a .../by-name/vendor path ('$cand')"
  VENDOR_BLK=$cand
  case $VENDOR_SRC in
    /dev/block/dm-*)
      blocker "/vendor is a dm device ($VENDOR_SRC): dm-verity is on, so it cannot be remounted read-write. Options: flash a vendor image without verity, or 'adb disable-verity' + reboot (this script never does it: it rewrites verity metadata on the verified partitions, not only vendor)" ;;
    *)
      [ "$(rsh "readlink -f $VENDOR_SRC")" = "$(rsh "readlink -f $VENDOR_BLK")" ] \
        || die "mounted $VENDOR_SRC is not $VENDOR_BLK: refusing" ;;
  esac
  SIZE=$(rsh "blockdev --getsize64 $VENDOR_BLK 2>/dev/null || echo \$((\$(cat /sys/class/block/\$(basename \$(readlink -f $VENDOR_BLK))/size) * 512))" | tail -n 1)
  [[ $SIZE =~ ^[0-9]+$ ]] && [ "$SIZE" -gt 0 ] || die "cannot read the size of $VENDOR_BLK"
  echo "vendor partition: $VENDOR_BLK, $SIZE bytes"

  mkdir -p "$BACKUP_DIR"
  avail=$(df -Pk "$BACKUP_DIR" | awk 'NR==2{print $4}')
  [ "$avail" -ge $((SIZE / 1024 + 1024)) ] || die "not enough host space in $BACKUP_DIR for the backup"

  need=0
  for cand in "${PLAN[@]}"; do need=$((need + $(stat -c %s "$(cut -d'|' -f2 <<<"$cand")") + 4096)); done
  avail=$(rsh 'df -k /vendor' | tail -n 1 | awk '{print $(NF-2)}')
  [[ $avail =~ ^[0-9]+$ ]] || die "cannot read free space of /vendor"
  [ $((avail * 1024)) -ge $((need * 2 + 262144)) ] || blocker "/vendor has $avail KiB free, the patch needs about $((need * 2 / 1024 + 256)) KiB"

  FC=$(rsh 'cat /vendor/etc/selinux/nonplat_file_contexts' 2>/dev/null || true)
  if [ -z "$FC" ]; then echo "WARNING: /vendor/etc/selinux/nonplat_file_contexts not readable: using the built-in fallback labels"; fi
}

show_plan() {
  local e kind host dest mode lbl old
  say "plan"
  for e in "${PLAN[@]}"; do
    IFS='|' read -r kind host dest mode <<<"$e"
    case $kind in
      etc) lbl=$(label_etc "$dest") ;;
      ueventd) lbl=$(label_ueventd "$dest") ;;
      *) lbl=$(label_other "$dest") ;;
    esac
    old=$(rsh "sha256sum $dest 2>/dev/null" | awk '{print $1}' || true)
    echo "$dest  mode $mode root:root  label $lbl  ($([ -n "$old" ] && echo "replaces sha256 ${old:0:12}, kept as $dest.pregsi" || echo "new file"))"
  done
}

# --- apply -------------------------------------------------------------------
IMG=
backup_vendor() {
  local stamp part h1 h2 n
  stamp=$(date +%Y%m%d-%H%M%S)
  IMG=$BACKUP_DIR/vendor-${SERIAL//[^A-Za-z0-9._-]/_}-$stamp.img
  part=$IMG.part
  say "backup: $VENDOR_BLK ($SIZE bytes) -> $IMG"
  rexec_out "dd if=$VENDOR_BLK bs=1048576 2>/dev/null" >"$part" || { rm -f "$part"; return 1; }
  n=$(stat -c %s "$part") || { rm -f "$part"; return 1; }
  [ "$n" = "$SIZE" ] || { echo "backup size $n, partition $SIZE"; rm -f "$part"; return 1; }
  h1=$(sha256sum "$part" | cut -d' ' -f1) || { rm -f "$part"; return 1; }
  h2=$(rexec_out "dd if=$VENDOR_BLK bs=1048576 2>/dev/null" | sha256sum | cut -d' ' -f1) || { rm -f "$part"; return 1; }
  [ -n "$h1" ] && [ "$h1" = "$h2" ] || { echo "backup sha256 $h1, re-read $h2"; rm -f "$part"; return 1; }
  # no errexit inside a function called as 'f || die': every step is checked
  mv -- "$part" "$IMG" || { echo "cannot move $part to $IMG"; rm -f "$part"; return 1; }
  printf '%s  %s\n' "$h1" "$(basename "$IMG")" >"$IMG.sha256" || { echo "cannot write $IMG.sha256"; rm -f "$IMG.sha256" "$IMG"; return 1; }
  echo "backup ok: $n bytes, sha256 $h1 (re-read matches)"
}

remount() { # $1 rw|ro
  rsh_ok "mount -o remount,$1 /vendor" || echo "mount: $RSH_OUT"
  if [ "$1" = rw ] && ! vendor_is_rw; then
    # a read-only block device, not verity: only the vendor device is touched
    rsh_ok "blockdev --setrw $VENDOR_BLK" || true
    rsh_ok "mount -o remount,rw /vendor" || echo "mount: $RSH_OUT"
  fi
  vendor_mount || return 1
  case $1:",$VENDOR_OPTS," in
    rw:*,rw,*|ro:*,ro,*) return 0 ;;
  esac
  return 1
}

dev_stat() { rsh "stat -c '%a %u %g' $1 2>/dev/null"; }

install_file() { # kind host dest mode
  local kind=$1 host=$2 dest=$3 mode=$4 lbl h
  vendor_path "$dest"
  target_ok "$dest"
  case $kind in
    etc) lbl=$(label_etc "$dest") ;;
    ueventd) lbl=$(label_ueventd "$dest") ;;
    *) lbl=$(label_other "$dest") ;;
  esac
  h=$(sha256sum "$host" | cut -d' ' -f1)
  rsh_ok "if [ -e $dest ] && [ ! -e $dest.pregsi ]; then cp -p $dest $dest.pregsi; fi" || die "cannot keep the old $dest: $RSH_OUT"
  # staging file: absent (checked in target_ok), created exclusively (noclobber
  # opens with O_EXCL), so whatever is there afterwards is ours to remove
  rexec_in "set -C; cat > $dest.new" <"$host" \
    || { rsh "[ -L $dest.new ] || rm -f $dest.new" >/dev/null || true; die "streaming $dest failed"; }
  rsh_ok "chown 0:0 $dest.new && chmod $mode $dest.new && chcon $lbl $dest.new && mv $dest.new $dest" \
    || { rsh "[ -L $dest.new ] || rm -f $dest.new" >/dev/null || true; die "chown/chmod/chcon/mv of $dest failed: $RSH_OUT"; }
  [ "$(rsh "sha256sum $dest" | awk '{print $1}')" = "$h" ] || die "sha256 mismatch after writing $dest"
  [ "$(dev_stat "$dest")" = "${mode#0} 0 0" ] || die "owner/mode of $dest wrong: $(dev_stat "$dest")"
  [ "$(dev_label "$dest")" = "$lbl" ] || die "label of $dest is $(dev_label "$dest"), expected $lbl"
  echo "installed $dest ($mode root:root $lbl)"
}

make_dir() { # dir: created root:root 0755 with its rule label, else the parent's
  local d=$1 pl lbl
  vendor_path "$d/x"
  resolves_under_vendor "$d"
  rsh_ok "[ -d $d ]" && return 0
  pl=$(dev_label "$(dirname "$d")")
  lbl=$(fc_label "$d" "${pl:-u:object_r:vendor_file:s0}")
  rsh_ok "mkdir -p $d && chown 0:0 $d && chmod 0755 $d && chcon $lbl $d" || die "mkdir $d failed: $RSH_OUT"
  echo "created $d ($lbl)"
}

rollback_cmd() {
  printf 'adb %s exec-in %s < %s\n' "${SERIAL:+-s $SERIAL}" "$(sq "$(wrap "dd of=$VENDOR_BLK bs=1048576")")" "$(sq "$IMG")"
}

apply() {
  local e kind host dest mode
  backup_vendor || die "vendor backup failed: nothing was written to the device"
  say "remount /vendor read-write"
  remount rw || die "cannot remount /vendor rw (verity?): the device is unchanged, the backup is kept: $IMG"
  RW=1
  make_dir /vendor/etc/init/hw
  [ ${#MOD_FOUND[@]} -eq 0 ] || make_dir /vendor/lib/modules
  for e in "${PLAN[@]}"; do
    IFS='|' read -r kind host dest mode <<<"$e"
    install_file "$kind" "$host" "$dest" "$mode"
  done
  rsh sync >/dev/null
  say "remount /vendor read-only"
  # RW stays armed until /proc/mounts shows /vendor ro; one retry here, one more
  # in the exit trap
  remount ro || { echo "remount ro failed, retrying"; rsh sync >/dev/null; remount ro; } \
    || die "/vendor is still read-write after the install (files written, backup kept: $IMG): reboot the watch"
  RW=0
  say "done. Reboot the watch to use the files."
  echo "rollback (whole partition; /vendor is read-only now, run it before anything remounts it rw, reboot at once, verify the image with $IMG.sha256 first):"
  rollback_cmd
  echo "or per file: restore <file>.pregsi over <file> (new files have none: delete them)"
}

# --- main --------------------------------------------------------------------
pick_serial
adb_ get-state >/dev/null 2>&1 || die "no adb device in state 'device'"
detect_su
build_plan
preflight
show_plan
if [ "$APPLY" -eq 0 ]; then
  say "DRY RUN: nothing written, no backup taken. Re-run with --apply."
  echo "would: dump $VENDOR_BLK to $BACKUP_DIR and verify size and sha256 against a re-read, remount /vendor rw, install the files above, sync, remount ro, print the rollback command."
  [ ${#BLOCKERS[@]} -eq 0 ] || { echo "dry run found ${#BLOCKERS[@]} blocker(s): --apply would stop"; exit 3; }
  exit 0
fi
[ ${#BLOCKERS[@]} -eq 0 ] || die "blockers above, not applying"
apply
