#!/usr/bin/env bash
# Add the /productinfo mount point to a COPY of a raw ext4 GSI system image.
# Rationale: devices/VENDOR_GSI_PATCH.md ("Root directories a GSI lacks").
# Usage: scripts/gsi_add_mountpoints.sh --label CONTEXT --output OUT.img IN.img
#   --label CONTEXT   SELinux label of /productinfo, u:object_r:TYPE:s0: the
#                     /productinfo entry of /vendor/etc/selinux/nonplat_file_contexts
#                     of the watch (adb shell grep productinfo ...)
#   --output OUT.img  the modified copy (must not exist, must not be IN.img)
#   IN.img            raw ext4 (run simg2img first on an Android sparse image);
#                     opened read-only, never modified
# Adds an empty root directory /productinfo, root:root 0755 (what fs_mgr made in
# the stock ramdisk root; the rc sets system 0771 on the mounted prodnv), with
# debugfs -w on the copy; then checks it with debugfs stat/ea_get and e2fsck -fn.
set -euo pipefail

LABEL= OUT= IN=

die() { echo "FAIL: $*" >&2; exit 1; }
say() { echo "== $*"; }
usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --label) [ $# -ge 2 ] || usage; LABEL=$2; shift ;;
    --output) [ $# -ge 2 ] || usage; OUT=$2; shift ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) [ -z "$IN" ] || usage; IN=$1 ;;
  esac
  shift
done
[ -n "$LABEL" ] && [ -n "$OUT" ] && [ -n "$IN" ] || usage
printf '%s' "$LABEL" | grep -Eq '^u:object_r:[A-Za-z0-9_]+:s0$' ||
  die "label must look like u:object_r:TYPE:s0, got: $LABEL"
for t in debugfs e2fsck; do command -v "$t" >/dev/null || die "$t not found (e2fsprogs)"; done

[ -f "$IN" ] || die "no such file: $IN"
[ ! -e "$OUT" ] || die "output exists: $OUT"
[ ! "$IN" -ef "$OUT" ] || die "output is the input"

# bytes at OFFSET as lowercase hex
hexat() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }
[ "$(hexat "$IN" 0 4)" != 3aff26ed ] ||
  die "$IN is an Android sparse image: convert it with simg2img first"
[ "$(hexat "$IN" 1080 2)" = 53ef ] || die "$IN is not a raw ext4 image (no ext magic)"

WORK=$(mktemp -d)
DONE=0
# a failed run leaves no half-made output
cleanup() {
  [ "$DONE" -eq 1 ] || rm -f "$OUT"
  rm -rf "$WORK"
}
trap cleanup EXIT

cp -- "$IN" "$OUT"

dbg() { debugfs "$@" "$OUT" 2>&1; }

# the copy must open (debugfs refuses -w on a shared_blocks image below)
dbg -R 'stat /' | grep -q 'Inode:' || die "cannot read the root of $OUT"
dbg -R 'stat /productinfo' | grep -q 'Inode:' &&
  die "/productinfo already exists in $IN"

printf '%s\0' "$LABEL" >"$WORK/label"
cat >"$WORK/cmds" <<EOC
mkdir /productinfo
set_inode_field /productinfo mode 040755
set_inode_field /productinfo uid 0
set_inode_field /productinfo gid 0
ea_set -f $WORK/label /productinfo security.selinux
EOC
say "debugfs -w on the copy"
if ! out=$(debugfs -w -f "$WORK/cmds" "$OUT" 2>&1); then
  echo "$out" >&2
  die "debugfs failed (shared_blocks image or no free space/inodes?)"
fi
# debugfs exits 0 on a failed command: look at what it printed
case $out in
  *"rror"*|*"ailed"*|*"nsupported"*|*"not found"*) echo "$out" >&2; die "debugfs reported an error" ;;
esac

say "verify"
st=$(dbg -R 'stat /productinfo')
echo "$st" | head -3
echo "$st" | grep -Eq 'Type: directory +Mode: +0755' || die "not a directory with mode 0755"
echo "$st" | grep -Eq 'User: +0 +Group: +0' || die "owner is not 0:0"
ea=$(dbg -R 'ea_get /productinfo security.selinux')
echo "$ea"
case $ea in *"$LABEL"*) ;; *) die "security.selinux is not $LABEL" ;; esac
[ -z "$(dbg -R 'ls -p /productinfo' | grep -Ev '^debugfs |^/[0-9]+/[0-9]+/[0-9]+/[0-9]+/\.\.?/' | grep -v '^$')" ] ||
  die "/productinfo is not empty"
e2fsck -fn "$OUT" >"$WORK/fsck" 2>&1 || { cat "$WORK/fsck" >&2; die "e2fsck -fn failed"; }
tail -1 "$WORK/fsck"

DONE=1
say "done: $OUT (the input $IN was not touched)"
