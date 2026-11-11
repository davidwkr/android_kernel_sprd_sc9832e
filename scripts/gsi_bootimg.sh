#!/bin/bash
# Pack a boot image for an Android 10+ GSI: kernel Image + DTB + the
# first-stage ramdisk, on the SPRD header and DT table of the stock boot
# image (see devices/GSI_BOOT.md).
#
# usage: scripts/gsi_bootimg.sh KERNEL_IMAGE DTB STOCK_BOOT OUT \
#		--init FILE --init-sha256 HEX [--fstab FILE] [--hardware NAME]
#
# The kernel command line is not set here: the header keeps the stock
# cmdline and the DT bootargs in DTB (root=/dev/ram0 rw init=/init
# androidboot.hardware=...) stay valid with a ramdisk. The stock boot image
# supplies the header, the id recipe and the DT table layout. DTB must not
# carry a firmware/android/fstab node, or first-stage init uses that and not
# the ramdisk fstab.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$HERE/../tools
FSTAB=$HERE/../devices/common/gsi/fstab.sl8541e_1h10
HW=sl8541e_1h10

[ $# -ge 4 ] || { sed -n '2,/^$/p' "$0" | sed 's/^# \?//' >&2; exit 2; }
KERNEL=$1 DTB=$2 STOCK=$3 OUT=$4
shift 4
INIT= SHA=
while [ $# -gt 0 ]; do
	case $1 in
	--init) INIT=${2:?}; shift 2 ;;
	--init-sha256) SHA=${2:?}; shift 2 ;;
	--fstab) FSTAB=${2:?}; shift 2 ;;
	--hardware) HW=${2:?}; shift 2 ;;
	*) echo "error: unknown argument $1" >&2; exit 2 ;;
	esac
done
[ -n "$INIT" ] && [ -n "$SHA" ] || { echo "error: --init and --init-sha256 are required" >&2; exit 2; }
for f in "$KERNEL" "$DTB" "$STOCK" "$INIT" "$FSTAB"; do
	[ -f "$f" ] || { echo "error: $f: no such file" >&2; exit 1; }
done
[ ! -e "$OUT" ] || [ -f "$OUT" ] || { echo "error: $OUT is not a regular file" >&2; exit 1; }

W=$(mktemp -d "${TMPDIR:-/tmp}/gsi-bootimg.XXXXXX")
trap 'rm -rf "$W"' EXIT

python3 -I "$TOOLS/sprd_bootimg.py" unpack "$STOCK" "$W/dir" >/dev/null
python3 -I "$TOOLS/sprd_bootimg.py" dt-table "$W/dir/dt" "$DTB" "$W/dt"
python3 -I "$TOOLS/mk_firststage_ramdisk.py" --init "$INIT" --init-sha256 "$SHA" \
	--fstab "$FSTAB" --hardware "$HW" -o "$W/rd.gz" --manifest "$W/manifest" >/dev/null
mkdir -p "$(dirname "$OUT")"
python3 -I "$TOOLS/sprd_bootimg.py" pack "$W/dir" "$OUT" --kernel "$KERNEL" \
	--dt "$W/dt" --ramdisk "$W/rd.gz"
python3 -I "$TOOLS/sprd_bootimg.py" info "$OUT"
sha256sum "$OUT"
