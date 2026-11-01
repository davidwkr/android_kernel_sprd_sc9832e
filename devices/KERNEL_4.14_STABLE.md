# Linux 4.14.y stable merge log

Base: 4.14.133 (UNISOC downstream). Each release applied as
`git diff v4.14.(N-1) v4.14.N | git apply --3way --index`, one commit per release.

| Release | Result | Conflicts / skipped hunks |
|---|---|---|
| 4.14.134 | conflict | drivers/usb/gadget/function/u_ether.c: 2 hunks skipped (vendor rx_submit already unlocks after size calc) |
| 4.14.135 | conflict | include/linux/compiler.h: __no_kasan_or_inline kept vendor 'notrace' variant; 0 skipped |
| 4.14.136 | clean | - |
| 4.14.137 | conflict | Makefile: clang --target block kept vendor CLANG_TRIPLE variant, switched to += ; 0 skipped |
| 4.14.138 | clean | - |
| 4.14.139 | clean | - |
| 4.14.140 | conflict | net/xfrm/xfrm_policy.c (8 hunks), xfrm_device.c + xfrm_state.c (4 hunks): skipped, 'remove pcpu policy cache' targets code absent in vendor (flow-cache xfrm retained) |
| 4.14.141 | conflict | fs/userfaultfd.c: still_valid guard merged with vendor vma_merge anon_name arg; skipped file absent in vendor: nouveau i2c/aux.c |
| 4.14.142 | clean | - |
| 4.14.143 | clean | - |
| 4.14.144 | clean | - |
| 4.14.145 | clean | - |
| 4.14.146 | conflict | arch/arm/mm/init.c (took stable pfn_valid), drivers/tty/serial/sprd_serial.c (whitespace, kept vendor); 0 skipped |
| 4.14.147 | conflict | fs/f2fs/{checkpoint,data,f2fs.h,inode,segment}.c: vendor f2fs is newer; error-code conversion re-applied on vendor code; 1 hunk skipped (segment.c f2fs_trim_fs kept vendor -EIO) |
| 4.14.148 | clean | - |
| 4.14.149 | conflict | kernel/sched/core.c: stable early dest_cpu applied in the !SPRD_CORE_CTL branch, vendor core_ctl branch unchanged; 0 skipped |
| 4.14.150 | clean | - |
| 4.14.151 | conflict | arm64 SSBS/Spectre-v4 backport: hwcap.h, cpuinfo.c, sysreg.h, ISAR0 ftr table and docs take stable (context-only conflicts); cpufeature.c kpti_install_ng_mappings takes the new cpu_enable signature and keeps vendor __nocfi; process.c keeps sprd_update_cpu_usage() after ssbs_thread_switch() |
| 4.14.152 | conflict | drivers/block/zram/zram_drv.c (took stable backing_dev_show), fs/f2fs/super.c (vendor already had sync_filesystem in f2fs_quota_off_umount; kept vendor); 0 skipped |
| 4.14.153 | clean | - |
| 4.14.154 | conflict | drivers/usb/gadget/configfs.c: vendor already had gi->slock (Android unbind/setup guard); ported the stable unbind-flag fix onto slock instead of adding a second spinlock; 0 skipped |

Builds: sprd_sharkle_defconfig Image + dtbs built OK at 4.14.147, at 4.14.150
(merged with the watch_414 fragment) and at 4.14.151.
