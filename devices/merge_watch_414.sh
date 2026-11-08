#!/bin/sh
# Usage: devices/merge_watch_414.sh <build-dir>
# Writes <build-dir>/.config = sprd_sharkle_defconfig + watch_414.fragment.
# Optional stock heart-rate code (see devices/HRS3XXX_INTERFACE.md): set
#   HRS_STOCK_BOOT_IMG=<this device's own stock boot image> and
#   HRS_PROFILE=hx3918|hrs3300   (hrs3300 for MT108, hx3918 for the others)
# to carve the blob into <build-dir>/hrsblob (stock bytes: never commit or ship
# it) and enable CONFIG_HRS3XXX_TYHX_BLOB with the matching variant.
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
if [ -n "${HRS_STOCK_BOOT_IMG:-}" ]; then
	case ${HRS_PROFILE:-} in
	hx3918) variant=HX3918 ;;
	hrs3300) variant=HRS3300 ;;
	*) echo "HRS_PROFILE must be hx3918 or hrs3300" >&2; exit 1 ;;
	esac
	[ -r "$HRS_STOCK_BOOT_IMG" ] || { echo "cannot read HRS_STOCK_BOOT_IMG" >&2; exit 1; }
	python3 -I tools/blobcarve/blobcarve.py --boot-img "$HRS_STOCK_BOOT_IMG" \
		--out "$out/hrsblob" --cross "$CROSS_COMPILE" --profile "$HRS_PROFILE"
	scripts/config --file "$out/.config" -e HRS3XXX_TYHX_BLOB \
		-e "HRS3XXX_TYHX_BLOB_$variant" \
		--set-str HRS3XXX_TYHX_BLOB_S "$out/hrsblob/hrsblob.S"
fi
make -C "$top" O="$out" olddefconfig
if [ -n "${HRS_STOCK_BOOT_IMG:-}" ]; then
	grep -qx "CONFIG_HRS3XXX_TYHX_BLOB_$variant=y" "$out/.config" || \
		{ echo "FAIL: HRS3XXX_TYHX_BLOB_$variant not enabled" >&2; exit 1; }
fi
