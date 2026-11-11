#!/usr/bin/env bash
# Test for scripts/gsi_add_mountpoints.sh on a small synthetic ext4 image
# (mkfs.ext4 -d on a temp dir); no real GSI needed.
# Usage: scripts/tests/gsi_add_mountpoints_test.sh
# Env: TMPDIR for the images (removed at the end).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../gsi_add_mountpoints.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
LABEL=u:object_r:productinfo_file:s0
PASS=0 FAIL=0

check() {
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then PASS=$((PASS + 1)); echo "ok   $what"
  else FAIL=$((FAIL + 1)); echo "FAIL $what"; fi
}
run() { OUTLOG=$("$SCRIPT" "$@" 2>&1); RC=$?; }
dbg() { debugfs -R "$1" "$IMG" 2>&1; }

echo "-- syntax"
check "bash -n" bash -n "$SCRIPT"
if command -v shellcheck >/dev/null; then check "shellcheck" shellcheck "$SCRIPT"
else echo "skip shellcheck (not installed)"; fi

mkdir -p "$T/root/system" "$T/root/vendor"
IMG=$T/gsi.img
truncate -s 8M "$IMG"
mkfs.ext4 -q -d "$T/root" "$IMG"
before=$(sha256sum <"$IMG")

echo "-- add /productinfo"
run --dir /productinfo --label "$LABEL" --output "$T/out.img" "$IMG"
check "exit 0" test "$RC" = 0
check "input untouched" test "$(sha256sum <"$IMG")" = "$before"
IMG=$T/out.img
check "output differs from input" test "$(sha256sum <"$IMG")" != "$before"
st=$(dbg 'stat /productinfo')
check "is a directory" grep -Eq 'Type: directory' <<<"$st"
check "mode 0755" grep -Eq 'Mode: +0755' <<<"$st"
check "owner 0:0" grep -Eq 'User: +0 +Group: +0' <<<"$st"
check "security.selinux set, NUL terminated" grep -qF "\"$LABEL\\000\"" <<<"$(dbg 'ea_get /productinfo security.selinux')"
check "empty" test "$(dbg 'ls -p /productinfo' | grep -c '^/')" = 2
check "existing files kept" grep -q 'Inode:' <<<"$(dbg 'stat /system')"
check "e2fsck -fn clean" e2fsck -fn "$IMG"
IMG=$T/gsi.img

echo "-- several directories"
L2=u:object_r:rootfs:s0
run --dir /productinfo --label "$LABEL" --dir /second_stage_resources --label "$L2" --output "$T/two.img" "$IMG"
check "two dirs: exit 0" test "$RC" = 0
check "two dirs: input untouched" test "$(sha256sum <"$IMG")" = "$before"
IMG=$T/two.img
for pair in "productinfo $LABEL" "second_stage_resources $L2"; do
  set -- $pair
  st=$(dbg "stat /$1")
  check "/$1 directory 0755 0:0" grep -Eqz 'Type: directory +Mode: +0755.*User: +0 +Group: +0' <<<"$st"
  check "/$1 label $2" grep -qF "\"$2\\000\"" <<<"$(dbg "ea_get /$1 security.selinux")"
done
check "two dirs: e2fsck -fn clean" e2fsck -fn "$IMG"
IMG=$T/gsi.img

echo "-- argument errors"
run --label "$LABEL" --dir /productinfo --output "$T/a1.img" "$IMG"
check "label before any --dir: usage" test "$RC" = 2
run --dir /productinfo --dir /foo --label "$LABEL" --output "$T/a2.img" "$IMG"
check "--dir without label: usage" test "$RC" = 2
run --dir /productinfo --label "$LABEL" --label "$L2" --output "$T/a3.img" "$IMG"
check "two labels for one --dir: usage" test "$RC" = 2
run --dir /foo --label "$LABEL" --dir /foo --label "$L2" --output "$T/a4.img" "$IMG"
check "duplicate --dir refused" test "$RC" != 0
for bad in / /. /.. foo /a/b /a/ '/a b' '/a;b' '//a' ''; do
  run --dir "$bad" --label "$LABEL" --output "$T/a5.img" "$IMG"
  check "bad name '$bad' refused" test "$RC" != 0
done
check "no output from argument errors" bash -c "! ls $T/a?.img"
run --dir /foo --label "$LABEL" --dir /system --label "$L2" --output "$T/one.img" "$IMG"
check "one of two exists: refused" test "$RC" != 0
check "...says which" grep -q '/system already exists' <<<"$OUTLOG"
check "...no output" test ! -e "$T/one.img"
check "...input untouched" test "$(sha256sum <"$IMG")" = "$before"

echo "-- refusals"
run --dir /productinfo --label "$LABEL" --output "$T/out.img" "$IMG"
check "existing output refused" test "$RC" != 0
check "refused input still untouched" test "$(sha256sum <"$IMG")" = "$before"
run --dir /productinfo --label "$LABEL" --output "$IMG" "$IMG"
check "output = input refused" test "$RC" != 0
run --dir /productinfo --label "$LABEL" --output "$T/o2.img" "$T/out.img"
check "image that has /productinfo refused" test "$RC" != 0
check "...and leaves no output" test ! -e "$T/o2.img"
run --dir /productinfo --label 'u:object_r:x:s0;rm' --output "$T/o3.img" "$IMG"
check "malformed label refused" test "$RC" != 0
check "...and leaves no output" test ! -e "$T/o3.img"
run --dir /productinfo --output "$T/o4.img" "$IMG"
check "missing --label: usage" test "$RC" = 2
run --dir /productinfo --label "$LABEL" "$IMG"
check "missing --output: usage" test "$RC" = 2
printf '\072\377\046\355' >"$T/sparse.img"
head -c 4096 /dev/zero >>"$T/sparse.img"
run --dir /productinfo --label "$LABEL" --output "$T/o5.img" "$T/sparse.img"
check "sparse image refused" test "$RC" != 0
check "...says simg2img" grep -q simg2img <<<"$OUTLOG"
head -c 8192 /dev/zero >"$T/zero.img"
run --dir /productinfo --label "$LABEL" --output "$T/o6.img" "$T/zero.img"
check "non-ext4 refused" test "$RC" != 0
check "refused input still untouched" test "$(sha256sum <"$IMG")" = "$before"

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
