#!/usr/bin/env bash
# Test harness for scripts/vendor_gsi_patch.sh with a stub adb (a shell function)
# over a fake device tree: no device, no adb binary needed.
# Usage: scripts/tests/vendor_gsi_patch_test.sh
# Env: TMPDIR for the fake device and the backups (removed at the end).
# The stub maps the device paths /vendor, /dev/block and /proc/mounts into the
# fake tree and shims su, id, getprop, mount, chcon, chown, stat, ls -Zd, df,
# blockdev and dd. Knobs: FAKE_SU=adbd|magisk|aosp, FAKE_VERITY=1,
# FAKE_SHORT_READ=1 (truncated dump), FAKE_FLAKY=1 (second read differs),
# FAKE_RO_FAILS=1 (remount ro fails). HOSTBIN: dir of host-side stubs put first
# in PATH for the script (mv, date).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../vendor_gsi_patch.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
BLK_REL='dev/block/platform/soc/soc:ap-ahb/20600000.sdio/by-name'

adb_mock() {
  [ "${1:-}" != -s ] || shift 2
  local sub=${1:-}
  shift || true
  case $sub in
    get-state) echo device ;;
    devices) printf 'List of devices attached\nFAKE0001\tdevice\n' ;;
    shell|exec-out|exec-in)
      local cmd
      cmd=$(printf '%s' "$*" | sed -E \
        -e "s#(^|[ '=>(<])/vendor#\1$FAKE/vendor#g" \
        -e "s#(^|[ '=>(<])/dev/block#\1$FAKE/dev/block#g" \
        -e "s#(^|[ '=>(<])/proc/mounts#\1$FAKE/proc/mounts#g")
      echo "$sub $*" >>"$FAKE/adb.log"
      # text output must not show the fake root; exec-out/exec-in are raw bytes
      if [ "$sub" = shell ]; then
        PATH=$FAKE/bin:$PATH bash -c "$cmd" | sed -u "s#$FAKE##g"
      else
        PATH=$FAKE/bin:$PATH bash -c "$cmd"
      fi ;;
    *) echo "adb_mock: unsupported $sub" >&2; return 1 ;;
  esac
}
export -f adb_mock

new_fake() {
  FAKE=$(mktemp -d "$T/fake.XXXXXX")
  export FAKE
  mkdir -p "$FAKE/bin" "$FAKE/proc" "$FAKE/vendor/etc/selinux" "$FAKE/$BLK_REL" "$FAKE/system" "$FAKE/data"
  head -c 262144 /dev/urandom >"$FAKE/$BLK_REL/vendor"
  head -c 4096 /dev/urandom >"$FAKE/$BLK_REL/system"
  echo system >"$FAKE/system/build.prop"
  echo userdata >"$FAKE/data/sentinel"
  echo 'ro.vendor=1' >"$FAKE/vendor/build.prop"
  cat >"$FAKE/vendor/etc/selinux/nonplat_file_contexts" <<'EOF'
# test contexts
/vendor/etc(/.*)?            u:object_r:vendor_configs_file:s0
/vendor/ueventd\.rc          u:object_r:vendor_ueventd_file:s0
/vendor/lib/modules(/.*)?    u:object_r:vendor_kmod_file:s0
/vendor/bad(/.*)?            u:object_r:bad label:s0
EOF
  : >"$FAKE/labels"
  mount_line ro
  export FAKE_SU=${FAKE_SU:-magisk} FAKE_VERITY=${FAKE_VERITY:-0} FAKE_SHORT_READ=${FAKE_SHORT_READ:-0} FAKE_FLAKY=${FAKE_FLAKY:-0}
  write_shims
}
mount_line() { # ro|rw
  local src=/$BLK_REL/vendor
  [ "${FAKE_VERITY:-0}" != 1 ] || src=/dev/block/dm-1
  echo "$src /vendor ext4 $1,seclabel,relatime 0 0" >"$FAKE/proc/mounts"
}

write_shims() {
  local b=$FAKE/bin
  cat >"$b/su" <<'EOF'
#!/bin/bash
# magisk: su -c CMD; aosp: su 0 sh -c CMD
case $FAKE_SU:$1 in
  magisk:-c) shift; exec bash -c "$1" ;;
  aosp:0) shift 3; exec bash -c "$1" ;;
esac
echo "su: not allowed" >&2; exit 1
EOF
  cat >"$b/id" <<'EOF'
#!/bin/bash
if [ "$FAKE_SU" = adbd ] || [ -n "${FAKE_IN_SU:-}" ]; then echo 'uid=0(root) gid=0(root)'; else echo 'uid=2000(shell) gid=2000(shell)'; fi
EOF
  # inside su the shell is root: the su shim marks it
  sed -i 's|^case \$FAKE_SU:\$1 in|export FAKE_IN_SU=1\ncase $FAKE_SU:$1 in|' "$b/su"
  cat >"$b/getprop" <<'EOF'
#!/bin/bash
[ "$1" = ro.hardware ] && echo "${FAKE_HW:-sl8541e_1h10}"
EOF
  cat >"$b/blockdev" <<'EOF'
#!/bin/bash
case $1 in
  --getsize64) stat -c %s "$2" ;;
  --setrw) echo "setrw $2" >>"$FAKE/ops.log" ;;
esac
EOF
  cat >"$b/mount" <<'EOF'
#!/bin/bash
# mount -o remount,rw|ro /vendor
mode=${2#remount,}
echo "mount $*" >>"$FAKE/ops.log"
if [ "$mode" = ro ] && [ "${FAKE_RO_FAILS:-0}" = 1 ]; then echo "mount: '/vendor' busy" >&2; exit 1; fi
if [ "$mode" = rw ] && [ "$FAKE_VERITY" = 1 ]; then echo "mount: '/vendor' not user mountable / read-only device" >&2; exit 1; fi
sed -i -E "s#(/vendor ext4 )(ro|rw),#\1$mode,#" "$FAKE/proc/mounts"
EOF
  cat >"$b/chown" <<'EOF'
#!/bin/bash
echo "chown $*" >>"$FAKE/ops.log"
EOF
  cat >"$b/chcon" <<'EOF'
#!/bin/bash
case $1 in *' '*|*bad*) echo "chcon: invalid context" >&2; exit 1 ;; esac
f=${2#"$FAKE"}
grep -v "^$f " "$FAKE/labels" >"$FAKE/labels.n"; echo "$f $1" >>"$FAKE/labels.n"; mv "$FAKE/labels.n" "$FAKE/labels"
echo "chcon $1 $f" >>"$FAKE/ops.log"
EOF
  cat >"$b/mv" <<'EOF'
#!/bin/bash
/bin/mv "$@" || exit 1
from=${1#"$FAKE"} to=${2#"$FAKE"}
l=$(grep "^$from " "$FAKE/labels" | cut -d' ' -f2)
if [ -n "$l" ]; then grep -v "^$from " "$FAKE/labels" | grep -v "^$to " >"$FAKE/labels.n"; echo "$to $l" >>"$FAKE/labels.n"; /bin/mv "$FAKE/labels.n" "$FAKE/labels"; fi
EOF
  cat >"$b/stat" <<'EOF'
#!/bin/bash
# stat -c '%a %u %g' FILE: owner is always root in the fake
if [ "$1" = -c ] && [ "$2" = '%a %u %g' ]; then echo "$(/usr/bin/stat -c %a "$3") 0 0"; else /usr/bin/stat "$@"; fi
EOF
  cat >"$b/ls" <<'EOF'
#!/bin/bash
if [ "$1" = -Zd ]; then
  f=${2#"$FAKE"}
  l=$(grep "^$f " "$FAKE/labels" | cut -d' ' -f2)
  [ -e "$2" ] || exit 1
  echo "${l:-u:object_r:vendor_file:s0} $f"
else /bin/ls "$@"; fi
EOF
  cat >"$b/df" <<'EOF'
#!/bin/bash
printf 'Filesystem 1K-blocks Used Available Use%% Mounted on\n/dev/block/dm-1 100000 1000 %s 1%% /vendor\n' "${FAKE_FREE_KB:-90000}"
EOF
  cat >"$b/sync" <<'EOF'
#!/bin/bash
echo sync >>"$FAKE/ops.log"
EOF
  cat >"$b/dd" <<'EOF'
#!/bin/bash
n=$(( $(cat "$FAKE/reads" 2>/dev/null || echo 0) + 1 )); echo $n >"$FAKE/reads"
if [ "$FAKE_SHORT_READ" = 1 ]; then /bin/dd "$@" | head -c 1000
elif [ "$FAKE_FLAKY" = 1 ] && [ $n -ge 2 ]; then /bin/dd "$@"; printf x
else /bin/dd "$@"; fi
EOF
  chmod +x "$b"/*
}

PASS=0 FAIL=0
check() { # description, condition command...
  local d=$1; shift
  if "$@"; then PASS=$((PASS + 1)); echo "ok   $d"; else FAIL=$((FAIL + 1)); echo "FAIL $d"; fi
}
tree_hash() { (cd "$FAKE" && find system data dev -type f | sort | xargs sha256sum | sha256sum); }
vendor_hash() { (cd "$FAKE/vendor" && find . -type f | sort | xargs sha256sum | sha256sum); }
run() {
  OUT=$(PATH=${HOSTBIN:+$HOSTBIN:}$PATH ADB=adb_mock TMPDIR=$T bash ${VGP_TRACE:+-x} "$SCRIPT" "$@" 2>&1); RC=$?
  [ -z "${VGP_DEBUG:-}" ] || { echo "--- run $*: rc $RC"; echo "$OUT" | tail -n 30; }
}
has() { grep -q -- "$1" <<<"$OUT"; }
lacks() { ! has "$1"; }
label_of() { grep "^$1 " "$FAKE/labels" | cut -d' ' -f2; }
backups() { find "$BK" -name '*.img' 2>/dev/null | wc -l; }

BK=$T/backup

echo "-- syntax"
check "bash -n" bash -n "$SCRIPT"
if command -v shellcheck >/dev/null; then check "shellcheck" shellcheck "$SCRIPT"; else echo "skip shellcheck (not installed)"; fi

echo "-- shipped fstabs and rc files"
for d in "$HERE"/../../devices/*/vendor_gsi; do
  n=$(basename "$(dirname "$d")")
  check "$n fstab: no encryptable/forceencrypt/fileencryption" bash -c "! grep -v '^#' '$d/etc/fstab.sl8541e_1h10' | grep -Eq 'encryptable|forceencrypt|fileencryption'"
  check "$n rc: no mknod (not an init command)" bash -c "! grep -Eq '^[[:space:]]*mknod ' '$d/etc/init/hw/init.sl8541e_1h10.rc'"
done

echo "-- dry run is the default"
new_fake; unset FAKE_SU; FAKE_SU=magisk; export FAKE_SU
before=$(tree_hash); vb=$(vendor_hash)
run --device dw100u-silver --backup-dir "$BK"
check "exit 0" test "$RC" = 0
check "says DRY RUN" has 'DRY RUN'
check "no backup written" test "$(backups)" = 0
check "vendor tree unchanged" test "$(vendor_hash)" = "$vb"
check "no write op on the device" bash -c "! grep -qE 'chown|chcon|remount,rw|setrw' '$FAKE/ops.log' 2>/dev/null"
check "no exec-in used" bash -c "! grep -q '^exec-in' '$FAKE/adb.log'"
check "ueventd label read from the device file_contexts" has 'label u:object_r:vendor_ueventd_file:s0'
check "etc label from file_contexts" has 'vendor_configs_file'
run --device dw100u-silver --apply --dry-run --backup-dir "$BK"
check "explicit --dry-run after --apply wins" has 'DRY RUN'

echo "-- apply, Magisk (DW100U)"
FAKE_SU=magisk; export FAKE_SU
new_fake; before=$(tree_hash)
run --device dw100u-silver --apply --backup-dir "$BK" --serial FAKE0001
check "exit 0" test "$RC" = 0
check "root mode magisk" has 'root: magisk'
check "backup image exists" test "$(backups)" = 1
img=$(find "$BK" -name '*.img')
check "backup equals the partition" cmp -s "$img" "$FAKE/$BLK_REL/vendor"
check "sha256 file written" test -s "$img.sha256"
check "rc installed" cmp -s "$FAKE/vendor/etc/init/hw/init.sl8541e_1h10.rc" "$HERE/../../devices/dw100u-silver/vendor_gsi/etc/init/hw/init.sl8541e_1h10.rc"
check "fstab installed" cmp -s "$FAKE/vendor/etc/fstab.sl8541e_1h10" "$HERE/../../devices/dw100u-silver/vendor_gsi/etc/fstab.sl8541e_1h10"
check "ueventd.rc installed" cmp -s "$FAKE/vendor/ueventd.rc" "$HERE/../../devices/dw100u-silver/vendor_gsi/ueventd.rc"
check "mode 644" test "$(stat -c %a "$FAKE/vendor/ueventd.rc")" = 644
check "ueventd label" test "$(label_of /vendor/ueventd.rc)" = u:object_r:vendor_ueventd_file:s0
check "etc label" test "$(label_of /vendor/etc/fstab.sl8541e_1h10)" = u:object_r:vendor_configs_file:s0
check "owner root:root requested" grep -q 'chown 0:0' "$FAKE/ops.log"
check "remounted rw then ro" bash -c "grep -n 'mount' '$FAKE/ops.log' | grep -q 'remount,rw' && tail -n 3 '$FAKE/ops.log' | grep -q 'remount,ro' || grep -q 'remount,ro' '$FAKE/ops.log'"
check "vendor ends ro" grep -q ' ro,' "$FAKE/proc/mounts"
check "rollback command printed" has 'exec-in .*dd of=.*by-name/vendor'
check "other partitions and /data untouched" test "$(tree_hash)" = "$before"
check "no write outside /vendor in the adb log" bash -c "! grep -E '> *(/data|/system|/sdcard)' '$FAKE/adb.log'"
echo old >"$FAKE/vendor/ueventd.rc"
run --device dw100u-silver --apply --backup-dir "$T/backup2"
check "old file kept as .pregsi" test "$(cat "$FAKE/vendor/ueventd.rc.pregsi")" = old
echo older >"$FAKE/vendor/ueventd.rc"
run --device dw100u-silver --apply --backup-dir "$T/backup3"
check "an existing .pregsi is not overwritten" test "$(cat "$FAKE/vendor/ueventd.rc.pregsi")" = old

echo "-- apply, vendor su (DM63), modules, mali marker"
FAKE_SU=aosp; export FAKE_SU
new_fake; before=$(tree_hash)
mkdir "$T/mods"; for m in sprdbt_tty sprd_fm sprdwl_ng mali; do printf '\177ELF vermagic=4.4.302 SMP' >"$T/mods/$m.ko"; done
run --device dm63 --apply --backup-dir "$T/b3" --modules-dir "$T/mods"
check "exit 0" test "$RC" = 0
check "root mode aosp" has 'root: aosp'
check "modules installed" test -f "$FAKE/vendor/lib/modules/sprdwl_ng.ko" -a -f "$FAKE/vendor/lib/modules/mali.ko"
check "module label from file_contexts" test "$(label_of /vendor/lib/modules/mali.ko)" = u:object_r:vendor_kmod_file:s0
check "mali insmod switched on" grep -q '^    insmod /vendor/lib/modules/mali.ko' "$FAKE/vendor/etc/init/hw/init.sl8541e_1h10.rc"
check "device partitions untouched" test "$(tree_hash)" = "$before"
rm "$T/mods/mali.ko" "$T/mods/sprd_fm.ko"
new_fake
run --device dm63 --apply --backup-dir "$T/b4" --modules-dir "$T/mods"
check "no mali.ko: the insmod stays a comment" test "$(grep -c 'insmod /vendor/lib/modules/mali.ko' "$FAKE/vendor/etc/init/hw/init.sl8541e_1h10.rc")" = 1 -a "$(grep -c '^    insmod /vendor/lib/modules/mali.ko' "$FAKE/vendor/etc/init/hw/init.sl8541e_1h10.rc")" = 0
check "missing module is a warning" has 'sprd_fm.ko not supplied'
check "bad ELF refused" bash -c "printf 'MZ' >'$T/mods/sprdwl_ng.ko'; ! ADB=adb_mock TMPDIR='$T' bash '$SCRIPT' --device dm63 --modules-dir '$T/mods' >/dev/null 2>&1"

echo "-- backup failures stop everything"
FAKE_SU=magisk; FAKE_SHORT_READ=1; export FAKE_SU FAKE_SHORT_READ
new_fake; vb=$(vendor_hash)
run --device dw100u-silver --apply --backup-dir "$T/b5"
check "short dump: exit non-zero" test "$RC" != 0
check "short dump: refuses" has 'nothing was written'
check "short dump: no image kept" test "$(find "$T/b5" -name '*.img' | wc -l)" = 0
check "short dump: vendor unchanged" test "$(vendor_hash)" = "$vb"
check "short dump: never remounted" bash -c "! grep -q 'remount' '$FAKE/ops.log' 2>/dev/null"
FAKE_SHORT_READ=0; FAKE_FLAKY=1; export FAKE_SHORT_READ FAKE_FLAKY
new_fake; vb=$(vendor_hash)
run --device dw100u-silver --apply --backup-dir "$T/b6"
check "re-read mismatch: exit non-zero" test "$RC" != 0
check "re-read mismatch: vendor unchanged" test "$(vendor_hash)" = "$vb"
check "re-read mismatch: never remounted" bash -c "! grep -q 'remount' '$FAKE/ops.log' 2>/dev/null"
FAKE_FLAKY=0; export FAKE_FLAKY

echo "-- backup: the last steps are checked too"
HOSTBIN=$T/hostbin; export HOSTBIN
mkdir -p "$HOSTBIN"
printf '#!/bin/bash\n[ "$1" = -- ] && shift\ncase $1 in *.part) exit 1 ;; esac\nexec /bin/mv "$@"\n' >"$HOSTBIN/mv"
printf '#!/bin/bash\ncase $1 in +%%Y*) echo 20000101-000000 ;; *) exec /bin/date "$@" ;; esac\n' >"$HOSTBIN/date"
chmod +x "$HOSTBIN/mv" "$HOSTBIN/date"
new_fake; vb=$(vendor_hash)
run --device dw100u-silver --apply --backup-dir "$T/b11"
check "mv of the backup fails: exit non-zero" test "$RC" != 0
check "...no 'backup ok'" lacks 'backup ok'
check "...says nothing was written" has 'nothing was written'
check "...vendor unchanged, never remounted" bash -c "test '$(vendor_hash)' = '$vb' && ! grep -q remount '$FAKE/ops.log' 2>/dev/null"
rm "$HOSTBIN/mv"
new_fake; vb=$(vendor_hash)
mkdir -p "$T/b12/vendor-FAKE0001-20000101-000000.img.sha256"
run --device dw100u-silver --apply --backup-dir "$T/b12"
check "sidecar write fails: exit non-zero" test "$RC" != 0
check "...no 'backup ok'" lacks 'backup ok'
check "...vendor unchanged, never remounted" bash -c "test '$(vendor_hash)' = '$vb' && ! grep -q remount '$FAKE/ops.log' 2>/dev/null"
rm "$HOSTBIN/date"; unset HOSTBIN

echo "-- symlinks under /vendor"
new_fake; before=$(tree_hash)
mkdir -p "$FAKE/vendor/etc/init" && rm -rf "$FAKE/vendor/etc/init" && ln -s "$FAKE/data" "$FAKE/vendor/etc/init"
run --device dw100u-silver --apply --backup-dir "$T/b13"
check "symlinked parent: refused" test "$RC" != 0
check "...says outside /vendor" has 'outside /vendor'
check "...nothing written through the link" test "$(tree_hash)" = "$before"
check "...nothing created in /data" test "$(ls "$FAKE/data")" = sentinel
check "...vendor back to ro" grep -q ' ro,' "$FAKE/proc/mounts"
new_fake; before=$(tree_hash)
mkdir -p "$FAKE/vendor/lib"; ln -s "$FAKE/data" "$FAKE/vendor/lib/modules"
mkdir -p "$T/mods2"; printf '\177ELF' >"$T/mods2/sprdwl_ng.ko"
run --device dw100u-silver --apply --backup-dir "$T/b14" --modules-dir "$T/mods2"
check "symlinked modules dir: refused" test "$RC" != 0
check "...nothing created in /data" test "$(ls "$FAKE/data")" = sentinel
new_fake; before=$(tree_hash)
mkdir -p "$FAKE/vendor/etc/init/hw"
echo precious >"$FAKE/data/target"; ln -s "$FAKE/data/target" "$FAKE/vendor/ueventd.rc.new"
run --device dw100u-silver --apply --backup-dir "$T/b15"
check "pre-existing .new symlink: refused" test "$RC" != 0
check "...says symlink" has 'symlink'
check "...the link target is untouched" test "$(cat "$FAKE/data/target")" = precious
check "...the link is still there" test -L "$FAKE/vendor/ueventd.rc.new"
new_fake; before=$(tree_hash)
echo precious >"$FAKE/data/target"; ln -s "$FAKE/data/target" "$FAKE/vendor/ueventd.rc"
run --device dw100u-silver --apply --backup-dir "$T/b16"
check "target that is a symlink: refused" test "$RC" != 0
check "...the link target is untouched" test "$(cat "$FAKE/data/target")" = precious

echo "-- remount ro must be verified"
new_fake
FAKE_RO_FAILS=1; export FAKE_RO_FAILS
run --device dw100u-silver --apply --backup-dir "$T/b17"
check "ro stays rw: exit non-zero" test "$RC" != 0
check "...says still read-write" has 'still read-write'
check "...does not claim done" lacks '^== done'
check "...remount ro tried at least 3 times (call, retry, trap)" test "$(grep -c 'remount,ro' "$FAKE/ops.log")" -ge 3
unset FAKE_RO_FAILS
new_fake
run --device dw100u-silver --apply --backup-dir "$T/b18"
check "ro ok: exit 0 and ro in /proc/mounts" bash -c "test '$RC' = 0 && grep -q ' ro,' '$FAKE/proc/mounts'"

echo "-- verity"
FAKE_VERITY=1; export FAKE_VERITY
new_fake; vb=$(vendor_hash)
run --device dw100u-silver --apply --backup-dir "$T/b7"
check "verity: exit non-zero" test "$RC" != 0
check "verity: explained" has 'dm-verity'
check "verity: nothing written" test "$(vendor_hash)" = "$vb"
run --device dw100u-silver --backup-dir "$T/b7"
check "verity: dry run reports the blocker with exit 3" test "$RC" = 3
FAKE_VERITY=0; export FAKE_VERITY

echo "-- guards"
new_fake
FAKE_HW=other; export FAKE_HW
run --device dw100u-silver --apply --backup-dir "$T/b8"
check "wrong ro.hardware refused" test "$RC" != 0
unset FAKE_HW
run --device dw100u-silver --apply --backup-dir "$T/b9" --vendor-blk "/$BLK_REL/system"
check "non by-name/vendor block device refused" test "$RC" != 0
check "refusal wrote no backup" test "$(find "$T/b9" -name '*.img' 2>/dev/null | wc -l)" = 0
run --device k18
check "unknown device: usage" test "$RC" = 2
FAKE_FREE_KB=10; export FAKE_FREE_KB
run --device dw100u-silver --apply --backup-dir "$T/b10"
check "no space on /vendor: refused before writing" test "$RC" != 0
unset FAKE_FREE_KB

echo "-- no partition other than vendor is named in a write"
check "script runs no fastboot, push, sideload, reboot or disable-verity" bash -c "! grep -nE '^[^#]*(fastboot|adb_? (push|sideload|reboot)|\\\$ADB (push|sideload|reboot))|^[^#]*(adb_ |rsh[_a-z]* .)disable-verity' '$SCRIPT'"

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
