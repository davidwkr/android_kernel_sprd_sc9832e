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

## Stopped at 4.14.151

4.14.151 is a large arm64 backport (SSBS/Spectre-v4 rework, cpufeature.c and
cpu_errata.c rewrite, sysreg.h/hwcap/ptrace/proc.S/head.S changes, ~107 files).
It conflicts in arch/arm64 cpufeature.c (kpti_install_ng_mappings with the
vendor __nocfi, unmap_kernel_at_el0), sysreg.h, hwcap.h, cpuinfo.c and
process.c (vendor sprd_update_cpu_usage), plus cpu_errata.c. This is arch/arm64
core code; not resolved here, needs a deliberate decision (the vendor tree
already carries partial Android/ARM backports in these files).

Builds: sprd_sharkle_defconfig Image + dtbs built OK at 4.14.147 (no failures).
Not rebuilt at 4.14.150.
