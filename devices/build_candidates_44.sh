#!/bin/bash
# Builds the DM63 and DW100U 4.4 candidate boot images (devices/CANDIDATES_44.md). Inputs: SRC (this tree,
# default), SFD (directory with the stock dm63/boot.img and dw100u-silver/boot.bin, required), OUT (output base,
# default ~/.cache).
set -euo pipefail
SRC=${SRC:-$(cd "$(dirname "$0")/.." && pwd)}
SFD=${SFD:?set SFD to the directory with the stock boot images}
OUT=${OUT:-$HOME/.cache}
mkdir -p $OUT/tmp-build
cd $SRC
echo "source $(git log --oneline -1)"
build() { # device outbase stock_img out_img
  local D=$1 OB=$2 STOCK=$3 IMG=$4
  local W; W=$(mktemp -d $OUT/tmp-build/cand-$D.XXXX)
  cp "$STOCK" $W/stock.img
  HRS_STOCK_BOOT_IMG=$W/stock.img scripts/watch_build.sh $D $OB > $W/build.log 2>&1 || { tail -20 $W/build.log; return 1; }
  python3 -I tools/sprd_bootimg.py unpack $W/stock.img $W/dir >/dev/null
  python3 -I tools/sprd_ramdisk.py replace-modules $W/dir/ramdisk $W/rd1.gz --kernel $OB/$D/Image --symvers $OB/obj-$D/Module.symvers \
    --module $OB/$D/mali.ko --module $OB/$D/sprdwl_ng.ko --module $OB/$D/sprdbt_tty.ko --module $OB/$D/sprd_fm.ko
  python3 -I tools/sprd_ramdisk.py add-rc $W/rd1.gz $W/rd2.gz --fragment devices/common/init.watch_tweaks.rc
  mkdir -p "$(dirname $IMG)"
  python3 -I tools/sprd_bootimg.py pack $W/dir $IMG --kernel $OB/$D/Image --ramdisk $W/rd2.gz
  python3 -I tools/sprd_bootimg.py info $IMG | grep -E 'kernel|ramdisk|dt table|avb|id check'
  aarch64-linux-gnu-nm $OB/$D/mali.ko | grep -c ' U regulator_get_voltage' | sed "s/^/$D mali regulator_get_voltage imports: /"
  sha256sum $IMG; rm -rf $W
}
build dm63 $OUT/watch_out-dm63cand $SFD/dm63/boot.img $OUT/dm63-candidate/dm63_mali_candidate.img
build dw100u-silver $OUT/watch_out-dw100u-silver-cand $SFD/dw100u-silver/boot.bin $OUT/dw100u-silver-candidate/dw100u-silver_mali_candidate.img
echo ALLDONE
