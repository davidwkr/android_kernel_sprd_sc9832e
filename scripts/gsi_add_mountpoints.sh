#!/usr/bin/env bash
# Add root mount-point directories to a COPY of a raw ext4 GSI system image.
# Rationale: devices/VENDOR_GSI_PATCH.md ("Root directories a GSI lacks").
# Usage: scripts/gsi_add_mountpoints.sh --dir /NAME --label CONTEXT
#          [--dir /NAME --label CONTEXT ...] --output OUT.img IN.img
#   --dir /NAME       a root-level directory to add: one component, matching
#                     ^/[A-Za-z0-9_.-]+$ (not / . ..), checked on the whole
#                     argument (newlines refused); repeatable, no duplicates
#   --label CONTEXT   SELinux label of the preceding --dir, u:object_r:TYPE:s0;
#                     exactly one per --dir (see the doc for where to read it)
#   --output OUT.img  the modified copy (must not exist, must not be IN.img)
#   IN.img            raw ext4 (run simg2img first on an Android sparse image);
#                     opened read-only, never modified
# Adds each directory empty, root:root 0755, with debugfs -w on the copy; refuses
# if any of them already exists (before writing); then checks each with debugfs
# stat/ea_get and the image with e2fsck -fn.
set -euo pipefail

OUT= IN=
DIRS=() LABELS=()
PENDING=0 # a --dir still waits for its --label

die() { echo "FAIL: $*" >&2; exit 1; }
say() { echo "== $*"; }
usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --dir)
      [ $# -ge 2 ] && [ "$PENDING" -eq 0 ] || usage
      [[ $2 =~ ^/[A-Za-z0-9_.-]+$ ]] ||
        die "--dir must be one root-level name like /NAME, got: $2"
      case $2 in /.|/..) die "--dir must not be $2" ;; esac
      for d in ${DIRS[@]+"${DIRS[@]}"}; do [ "$d" != "$2" ] || die "duplicate --dir $2"; done
      DIRS+=("$2"); PENDING=1; shift ;;
    --label)
      [ $# -ge 2 ] && [ "$PENDING" -eq 1 ] || usage
      [[ $2 =~ ^u:object_r:[A-Za-z0-9_]+:s0$ ]] ||
        die "label must look like u:object_r:TYPE:s0, got: $2"
      LABELS+=("$2"); PENDING=0; shift ;;
    --output) [ $# -ge 2 ] || usage; OUT=$2; shift ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) [ -z "$IN" ] || usage; IN=$1 ;;
  esac
  shift
done
[ "${#DIRS[@]}" -gt 0 ] && [ "$PENDING" -eq 0 ] && [ -n "$OUT" ] && [ -n "$IN" ] || usage
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
for d in "${DIRS[@]}"; do
  ! dbg -R "stat $d" | grep -q 'Inode:' || die "$d already exists in $IN"
done

: >"$WORK/cmds"
for i in "${!DIRS[@]}"; do
  d=${DIRS[$i]}
  printf '%s\0' "${LABELS[$i]}" >"$WORK/label$i"
  cat >>"$WORK/cmds" <<EOC
mkdir "$d"
set_inode_field "$d" mode 040755
set_inode_field "$d" uid 0
set_inode_field "$d" gid 0
ea_set -f "$WORK/label$i" "$d" security.selinux
EOC
done
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
for i in "${!DIRS[@]}"; do
  d=${DIRS[$i]} l=${LABELS[$i]}
  st=$(dbg -R "stat $d")
  echo "$st" | head -3
  echo "$st" | grep -Eq 'Type: directory +Mode: +0755' || die "$d: not a directory with mode 0755"
  echo "$st" | grep -Eq 'User: +0 +Group: +0' || die "$d: owner is not 0:0"
  ea=$(dbg -R "ea_get $d security.selinux")
  echo "$ea"
  case $ea in *"$l"*) ;; *) die "$d: security.selinux is not $l" ;; esac
  [ -z "$(dbg -R "ls -p $d" | grep -Ev '^debugfs |^/[0-9]+/[0-9]+/[0-9]+/[0-9]+/\.\.?/' | grep -v '^$')" ] ||
    die "$d is not empty"
done
e2fsck -fn "$OUT" >"$WORK/fsck" 2>&1 || { cat "$WORK/fsck" >&2; die "e2fsck -fn failed"; }
tail -1 "$WORK/fsck"

DONE=1
say "done: $OUT (the input $IN was not touched)"
