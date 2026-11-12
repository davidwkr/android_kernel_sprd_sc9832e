#!/bin/bash
# Builds the DM63 and DW100U 4.14 ADF boot images for the stock Android 8.1 userspace
# (devices/dm63/BOOT_414.md, devices/dw100u/BOOT_414.md). Inputs: K (this tree, default), TOOLS (a 4.4 tree
# with tools/sprd_bootimg.py and sprd_ramdisk.py, default ../android_kernel_sprd_sc9832e), SFD (directory with
# the stock dm63/boot.img and dw100u-silver/boot.bin, required), O (build dir).
# The images use the normal ADF DTBs (stock bootargs, loglevel=1) and watch_414_ramoops.fragment (pstore/ramoops
# on). loglevel=7 sends every kernel message to the 115200-baud serial console synchronously and overheated the
# DM63 bench boots: LOGLEVEL7=1 selects the -ramoops DTBs (loglevel=7, devices/RAMOOPS_414.md) for a short attended
# debug boot only, and names the images *_ramoops_*. With
# BENCH_DEBUG=1 and ADB_PUBKEY=<path>/adbkey.pub (the bench host's adb PUBLIC key) each watch also gets a
# *_BENCHDEBUG.img with early authenticated adb (devices/BENCH_DEBUG.md); TOOLS must then hold
# tools/sprd_adbkey_check.py and tools/benchdebug/ (branch tools/ramdisk-benchdebug). The build tree is deleted
# once the images are packed (KEEP_BUILD=1 keeps it).
set -euo pipefail
K=${K:-$(cd "$(dirname "$0")/.." && pwd)}
T=${TOOLS:-$K/../android_kernel_sprd_sc9832e}
SFD=${SFD:?set SFD to the directory with the stock boot images}
if [ "${BENCH_DEBUG:-0}" = 1 ]; then
  python3 -I $T/tools/sprd_adbkey_check.py "${ADB_PUBKEY:-}" || { echo "BENCH_DEBUG=1 needs ADB_PUBKEY=<...>/adbkey.pub"; exit 1; }
fi
cd $K; echo "source $(git log --oneline -1)"
# stock loglevel=1 by default; LOGLEVEL7=1 uses the -ramoops DTBs (loglevel=7)
if [ "${LOGLEVEL7:-0}" = 1 ]; then SUF=-ramoops; IMGSUF=_ramoops; else SUF=; IMGSUF=; fi
M() { make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$O "$@"; }
# ADF config
O=${O:-$HOME/.cache/k414adf-full}; rm -rf $O; mkdir -p $O
export TMPDIR=${TMPDIR:-$HOME/.cache/tmp-build}; mkdir -p $TMPDIR
M sprd_sharkle_defconfig >/dev/null 2>&1
scripts/kconfig/merge_config.sh -m -O $O $O/.config arch/arm64/configs/watch_414.fragment arch/arm64/configs/watch_414_adf.fragment arch/arm64/configs/watch_414_stock81.fragment arch/arm64/configs/watch_414_ramoops.fragment >/dev/null 2>&1
M olddefconfig >/dev/null 2>&1
grep -E '^CONFIG_(ADF_SPRD|MALI_MIDGARD|MALI_DEVFREQ|NETFILTER_XT_MATCH_QTAGUID|ANDROID_LOW_MEMORY_KILLER|SPRD_VIBRATOR|ANDROID_TIMED_OUTPUT|SPRD_GSP)=' $O/.config
M -j16 Image modules sprd/sp9832e-1h10-dm63-adf$SUF.dtb sprd/sp9832e-1h10-dw100u-silver-adf$SUF.dtb > /tmp/b414-adf.log 2>&1 || { grep error /tmp/b414-adf.log | head; exit 1; }
echo "adf build ok, warnings: $(grep -c 'warning:' /tmp/b414-adf.log || true)"
pack() { # dev stock dtb out
  local W; W=$(mktemp -d $TMPDIR/c414-$1.XXXX)
  cp "$2" $W/stock.img
  cd $T
  python3 -I tools/sprd_bootimg.py unpack $W/stock.img $W/dir >/dev/null
  python3 -I tools/sprd_bootimg.py dt-table $W/dir/dt $O/arch/arm64/boot/dts/sprd/$3 $W/dt414
  W14=$O/drivers/misc/sprdwcn/clients
  mkdir -p $W/ko
  for k in wlan/sprdwl_ng bt/sprdbt_tty fm/sprd_fm; do
    aarch64-linux-gnu-strip --strip-debug -o $W/ko/$(basename $k).ko $W14/$k.ko
  done
  python3 -I tools/sprd_ramdisk.py replace-modules $W/dir/ramdisk $W/rd.gz --kernel $O/arch/arm64/boot/Image --symvers $O/Module.symvers \
    --module $W/ko/sprdwl_ng.ko --module $W/ko/sprdbt_tty.ko --module $W/ko/sprd_fm.ko | grep -E 'vermagic|replaced'
  # Mali is built in on 4.14: drop the stock 4.4 mali.ko and its insmod line
  python3 -I tools/sprd_ramdisk.py remove-module $W/rd.gz $W/rd2.gz --module mali.ko | grep removed
  mv $W/rd2.gz $W/rd.gz
  mkdir -p "$(dirname $4)"
  packimg() { # ramdisk out
    python3 -I tools/sprd_bootimg.py pack $W/dir $2 --kernel $O/arch/arm64/boot/Image --dt $W/dt414 --ramdisk $1 2>&1 | grep -v '^warning'
    python3 -I tools/sprd_bootimg.py info $2 | grep -E 'kernel |dt table|\[0\]|id check|avb hash'
    sha256sum $2
  }
  packimg $W/rd.gz $4
  if [ "${BENCH_DEBUG:-0}" = 1 ]; then
    python3 -I tools/sprd_ramdisk.py add-file $W/rd.gz $W/rd3.gz --path /adb_keys --file "$ADB_PUBKEY" --mode 0644 --uid 0 --gid 0 | cut -c1-60
    python3 -I tools/sprd_ramdisk.py add-rc $W/rd3.gz $W/rd4.gz --fragment tools/benchdebug/init.benchdebug.rc --name init.benchdebug.rc | cut -c1-60
    packimg $W/rd4.gz ${4%.img}_BENCHDEBUG.img
  fi
  rm -rf $W
  cd $K
}
pack dm63 $SFD/dm63/boot.img sp9832e-1h10-dm63-adf$SUF.dtb ${OUT:-$HOME/.cache}/dm63-414-candidate/dm63_414_adf${IMGSUF}_candidate.img
pack dw100u-silver $SFD/dw100u-silver/boot.bin sp9832e-1h10-dw100u-silver-adf$SUF.dtb ${OUT:-$HOME/.cache}/dw100u-silver-414-candidate/dw100u-silver_414_adf${IMGSUF}_candidate.img
[ "${KEEP_BUILD:-0}" = 1 ] || rm -rf $O
echo ALLDONE
