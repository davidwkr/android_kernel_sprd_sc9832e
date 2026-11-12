#!/bin/bash
# Builds the DM63 and DW100U 4.4 candidate boot images (devices/CANDIDATES_44.md). Inputs: SRC (this tree,
# default), SFD (directory with the stock dm63/boot.img and dw100u-silver/boot.bin, required), OUT (output base,
# default ~/.cache). Each device gets the normal image (DT bootargs loglevel=7) and, with BENCH_DEBUG=1 and
# ADB_PUBKEY=<path>/adbkey.pub (the bench host's adb PUBLIC key), a *_BENCHDEBUG.img with early authenticated adb
# (devices/BENCH_DEBUG.md). The kernel build tree is deleted once both images are packed (KEEP_BUILD=1 keeps it).
set -euo pipefail
SRC=${SRC:-$(cd "$(dirname "$0")/.." && pwd)}
SFD=${SFD:?set SFD to the directory with the stock boot images}
OUT=${OUT:-$HOME/.cache}
mkdir -p $OUT/tmp-build
if [ "${BENCH_DEBUG:-0}" = 1 ]; then
  python3 -I "$SRC/tools/sprd_adbkey_check.py" "${ADB_PUBKEY:-}" || { echo "BENCH_DEBUG=1 needs ADB_PUBKEY=<...>/adbkey.pub"; exit 1; }
fi
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
  # the stock DT with loglevel=7 instead of loglevel=1, nothing else changed
  python3 -I tools/sprd_bootimg.py dt-bootargs $W/dir/dt $W/dt7 --replace loglevel=1 loglevel=7 | tail -1
  mkdir -p "$(dirname $IMG)"
  pack() { # ramdisk out
    python3 -I tools/sprd_bootimg.py pack $W/dir $2 --kernel $OB/$D/Image --ramdisk $1 --dt $W/dt7
    python3 -I tools/sprd_bootimg.py info $2 | grep -E 'kernel|ramdisk|dt table|avb|id check'
    sha256sum $2
  }
  pack $W/rd2.gz $IMG
  if [ "${BENCH_DEBUG:-0}" = 1 ]; then
    python3 -I tools/sprd_ramdisk.py add-file $W/rd2.gz $W/rd3.gz --path /adb_keys --file "$ADB_PUBKEY" --mode 0644 --uid 0 --gid 0 | cut -c1-60
    python3 -I tools/sprd_ramdisk.py add-rc $W/rd3.gz $W/rd4.gz --fragment tools/benchdebug/init.benchdebug.rc --name init.benchdebug.rc | cut -c1-60
    pack $W/rd4.gz ${IMG%.img}_BENCHDEBUG.img
  fi
  aarch64-linux-gnu-nm $OB/$D/mali.ko | grep -c ' U regulator_get_voltage' | sed "s/^/$D mali regulator_get_voltage imports: /"
  rm -rf $W
  [ "${KEEP_BUILD:-0}" = 1 ] || rm -rf $OB
}
build dm63 $OUT/watch_out-dm63cand $SFD/dm63/boot.img $OUT/dm63-candidate/dm63_mali_candidate.img
build dw100u-silver $OUT/watch_out-dw100u-silver-cand $SFD/dw100u-silver/boot.bin $OUT/dw100u-silver-candidate/dw100u-silver_mali_candidate.img
echo ALLDONE
