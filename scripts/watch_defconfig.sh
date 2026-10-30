#!/usr/bin/env bash
# Generate or check the standalone arch/arm64/configs/<device>_defconfig files.
# Usage: scripts/watch_defconfig.sh [--check] DEVICE|all
#   DEVICE: dm63 | dw100u | mt107 | mt108 | dm82  (dw100u-silver, dw100u-v13m: alias of dw100u)
#   (default)  assemble exactly as watch_build.sh (watch_lib.sh), make savedefconfig
#              in a temporary O= outside the repo, write the defconfig
#   --check    regenerate and diff against the committed file; exit 1 on drift
# The fragments stay the source of truth: edit them, then rerun this script.
set -euo pipefail

usage() { echo "usage: $0 [--check] DEVICE|all  (dm63|dw100u|mt107|mt108|dm82)" >&2; exit 2; }
CHECK=0
if [ "${1:-}" = "--check" ]; then CHECK=1; shift; fi
[ $# -eq 1 ] || usage

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${ARCH:-arm64}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
export ARCH CROSS_COMPILE
. "$(dirname "${BASH_SOURCE[0]}")/watch_lib.sh"
cd "$SRC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MK() { make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" O="$O" "$@"; }

one() {
  watch_select "$1"
  O=$TMP/obj-$DEFCONFIG
  mkdir -p "$O"
  watch_assemble
  watch_verify || return 1
  MK savedefconfig >/dev/null
  if [ "$CHECK" -eq 1 ]; then
    if ! diff -u "$CFG/$DEFCONFIG" "$O/defconfig" >"$TMP/diff"; then
      echo "DRIFT: $DEFCONFIG differs from its fragments (run scripts/watch_defconfig.sh $1)" >&2
      head -n 40 "$TMP/diff" >&2
      return 1
    fi
    echo "OK: $DEFCONFIG up to date"
  else
    cp "$O/defconfig" "$CFG/$DEFCONFIG"
    echo "wrote $CFG/$DEFCONFIG"
  fi
}

if [ "$1" = all ]; then
  rc=0
  for d in dm63 dw100u mt107 mt108 dm82; do one "$d" || rc=1; done
  exit $rc
fi
one "$1"
