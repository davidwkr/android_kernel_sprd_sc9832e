#!/bin/sh
# Usage: devices/merge_watch_414.sh <build-dir>
# Writes <build-dir>/.config = sprd_sharkle_defconfig + watch_414.fragment.
# Then build with: make O=<build-dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs
set -e
[ -n "$1" ] || { echo "usage: $0 <build-dir>" >&2; exit 1; }
top=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-} KCONFIG_CONFIG="$out/.config"
make -C "$top" O="$out" sprd_sharkle_defconfig
cd "$top"
scripts/kconfig/merge_config.sh -m -O "$out" \
	"$out/.config" arch/arm64/configs/watch_414.fragment
make -C "$top" O="$out" olddefconfig
