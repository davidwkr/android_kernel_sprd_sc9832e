#!/usr/bin/env bash
# Build kernel Image + ramdisk modules for one watch.
# Usage: scripts/watch_build.sh DEVICE [OUTDIR]
#   DEVICE: dm63 | dw100u-silver | dw100u-v13m | mt107 | mt108 | dm82 | k18
#   OUTDIR: default ../watch_out (outside the repo); results in OUTDIR/DEVICE/
# Env: ARCH (arm64), CROSS_COMPILE (aarch64-linux-gnu-), JOBS (nproc)
set -euo pipefail

usage() { echo "usage: $0 DEVICE [OUTDIR]  (dm63|dw100u-silver|dw100u-v13m|mt107|mt108|dm82|k18)" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || usage

DEVICE=$1
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTBASE=${2:-$SRC/../watch_out}
mkdir -p "$OUTBASE"
OUTBASE=$(cd "$OUTBASE" && pwd)
RES=$OUTBASE/$DEVICE
O=$OUTBASE/obj-$DEVICE
ARCH=${ARCH:-arm64}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
JOBS=${JOBS:-$(nproc)}
export ARCH CROSS_COMPILE

. "$(dirname "${BASH_SOURCE[0]}")/watch_lib.sh"
[ "$DEVICE" != dw100u ] || usage   # the shared name is only for watch_defconfig.sh
watch_select "$DEVICE"
cd "$SRC"
mkdir -p "$O" "$RES"
MK() { make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" O="$O" "$@"; }

watch_assemble
watch_verify

echo "== build Image + modules (-j$JOBS)"
MK -j"$JOBS" Image modules

MALI=$SRC/drivers/gpu/arm/midgard/mali
BT=$SRC/drivers/misc/sprdwcn_bsp/sipc_modules/bluetooth/driver
FM=$SRC/drivers/misc/sprdwcn_bsp/sipc_modules/fm/driver
# The vendor module Kbuilds only build in their source dirs. Remove only the
# files this build created there: snapshot the untracked/ignored files before
# and delete the new ones afterwards (also on failure), so untracked work in
# those dirs is never touched. Skip a dir that is not inside this repo.
SNAP=$(mktemp -d)
others() { git -C "$1" ls-files --others -z -- . 2>/dev/null; }
for i in 0 1 2; do
  d=$(printf '%s\n' "$MALI" "$BT" "$FM" | sed -n "$((i+1))p")
  others "$d" > "$SNAP/$i"
  [ -d "$d/.tmp_versions" ] && touch "$SNAP/$i.tmpv"
  if tr '\0' '\n' < "$SNAP/$i" | grep -qE '\.(ko|o)$'; then
    echo "warning: $d already has untracked build artifacts (from an earlier" \
         "interrupted run?); they are kept, remove them by hand" >&2
  fi
done
clean_dirs() {
  local i d f
  for i in 0 1 2; do
    d=$(printf '%s\n' "$MALI" "$BT" "$FM" | sed -n "$((i+1))p")
    [ "$(git -C "$d" rev-parse --show-toplevel 2>/dev/null)" = "$SRC" ] || continue
    others "$d" | python3 -I -c '
import os, sys
before = set(open(sys.argv[1], "rb").read().split(b"\0"))
base = sys.argv[2].encode()
for p in sys.stdin.buffer.read().split(b"\0"):
    if p and p not in before and b".." not in p.split(b"/"):
        try:
            os.remove(os.path.join(base, p))
        except OSError:
            pass
' "$SNAP/$i" "$d" || true
    [ -e "$SNAP/$i.tmpv" ] || rmdir "$d/.tmp_versions" 2>/dev/null || true
  done
  rm -rf "$SNAP"
}

echo "== external modules"
# build in the source dirs; copy first, clean afterwards (also on failure)
trap clean_dirs EXIT
MK M="$MALI" CONFIG_MALI_MIDGARD=m CONFIG_MALI_PLATFORM_NAME=sharkle CONFIG_MALI_BOARD_PLATFORM=sharkle modules
MK M="$BT" BSP_BOARD_UNISOC_WCN_SOCKET=sipc modules
MK M="$FM" BSP_BOARD_UNISOC_WCN_SOCKET=sipc modules

echo "== collect"
rm -f "${RES:?}/Image" "${RES:?}/sprdwl_ng.ko" "${RES:?}/mali.ko" "${RES:?}/sprdbt_tty.ko" \
      "${RES:?}/sprd_fm.ko" "${RES:?}/sha256sum.txt"
cp "$O/arch/$ARCH/boot/Image" "$RES/"
cp "$(find "$O/drivers" -name sprdwl_ng.ko | head -n1)" "$RES/"
cp "$MALI/mali.ko" "$BT/sprdbt_tty.ko" "$FM/sprd_fm.ko" "$RES/"
# strip debug info only (never --strip-all: modules need their symbol table)
for k in sprdwl_ng mali sprdbt_tty sprd_fm; do
  before=$(stat -c %s "$RES/$k.ko")
  "${CROSS_COMPILE}strip" --strip-debug "$RES/$k.ko"
  echo "strip $k.ko: $before -> $(stat -c %s "$RES/$k.ko") bytes"
done
(cd "$RES" && sha256sum Image sprdwl_ng.ko mali.ko sprdbt_tty.ko sprd_fm.ko > sha256sum.txt)
cat "$RES/sha256sum.txt"
echo "OK: $RES"
