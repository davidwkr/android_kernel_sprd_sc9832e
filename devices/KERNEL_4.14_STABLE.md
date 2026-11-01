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
| 4.14.155 | conflict | fs/f2fs/recovery.c: stable recover_inode uid/gid/projid + mark_dirty change already present in vendor f2fs (newer backport); 3 hunks skipped as already applied |
| 4.14.156 | clean | - |
| 4.14.157 | conflict | fs/f2fs/data.c, segment.c: clear_cold_data() fixes already present in vendor (kept vendor f2fs_clear_page_private); tools/power/acpi/tools/acpidump/apmain.c absent in vendor; 6 skipped |
| 4.14.158 | conflict | drivers/hid/hid-core.c: 5 hunks skipped, vendor HID parser predates usage_size / hid_concatenate_usage_page infrastructure the fix builds on |
| 4.14.159 | conflict | configfs.c (kept vendor slock init), f2fs/gc.c (stable change already in vendor), scripts/mod/modpost.c (vendor CFI pattern 6 kept, stable .L pattern added as pattern 7); 3 skipped |
| 4.14.160 | clean | - |
| 4.14.161 | clean | - |
| 4.14.162 | clean | - |
| 4.14.163 | clean | - |
| 4.14.164 | conflict | drivers/mmc/core/block.c: took stable RPMB character-device rework, dropping vendor mmc_access_rpmb() queue bypass (JUDGEMENT CALL, see commit); 0 skipped |
| 4.14.165 | conflict | drivers/usb/musb/musb_core.c: vendor has no musb_pm_runtime_check_session(); 2 hunks skipped (DISCONNECT_99 quirk define + case), MUSB_POWER=0 init write applied |
| 4.14.166 | clean | - |
| 4.14.167 | clean | - |
| 4.14.168 | clean | - |
| 4.14.169 | clean | - |
| 4.14.170 | clean | - |
| 4.14.171 | clean | - |
| 4.14.172 | clean | - |
| 4.14.173 | conflict | drivers/tty/sysrq.c: 3 hunks skipped (vendor never raises console_loglevel in __handle_sysrq); u_serial.c: gs_start_tx() call added, vendor tty guard kept |
| 4.14.174 | clean | - |
| 4.14.175 | clean | net/xfrm/xfrm_device.c: vendor's no-op xfrm_dev_unregister() stub and its NETDEV_UNREGISTER case dropped in favour of stable's UNREGISTER -> xfrm_dev_down (fixes duplicate case value; 4.14.140 skip left the stub); 0 skipped |
| 4.14.176 | clean | - |
| 4.14.177 | conflict | arch/arm64/kernel/traps.c: took stable revert of exception-stack dump, kept vendor frame_fp_saved loop guard; mm/slub.c: swab() applied around vendor kasan_reset_tag(); 0 skipped |
| 4.14.178 | conflict | include/linux/overflow.h: kept vendor array_size/array3_size/struct_size, added check_shl_overflow() after them; 0 skipped |
| 4.14.179 | clean | - |
| 4.14.180 | clean | - |
| 4.14.181 | conflict | fs/f2fs/xattr.c: vendor f2fs API names kept in 4 hunks (f2fs_get_node_page, inline_xattr_addr(inode,page), f2fs_kzalloc); stable bounds checks applied; 0 skipped |
| 4.14.182 | clean | - |
| 4.14.183 | conflict | include/uapi/linux/xfrm.h: kept vendor XFRMA_* enum (SET_MARK/IF_ID), took stable comment fix on XFRMA_OFFLOAD_DEV; 0 skipped |
| 4.14.184 | clean | - |
| 4.14.185 | conflict | drivers/mmc/core/sdio.c: stable 'return err' replacing 'goto remove' applied inside the vendor CONFIG_MMC_EMBEDDED_SDIO block; 0 skipped |
| 4.14.186 | conflict | drivers/usb/host/xhci-plat.c: 1 hunk skipped, vendor xhci_plat_resume() is an empty stub (no xhci_resume call to extend) |
| 4.14.187 | clean | - |
| 4.14.188 | clean | - |
| 4.14.189 | conflict | arch/arm64/kernel/vmlinux.lds.S: dropped .altinstr_replacement output section (stable moves replacements to .text subsections), vendor KEEP() on .altinstructions kept; xhci-plat.c: 1 hunk skipped (reverts the 4.14.186 hunk skipped earlier) |
| 4.14.190 | clean | - |
| 4.14.191 | clean | - |
| 4.14.192 | conflict | fs/f2fs/dir.c: corrupted-namelen boundary check already present in vendor f2fs; kept vendor (file identical to HEAD), 2 hunks skipped as already applied |
| 4.14.193 | clean | - |
| 4.14.194 | clean | - |
| 4.14.195 | clean | - |
| 4.14.196 | clean | include/linux/overflow.h: stable re-adds array_size/array3_size/__ab_c_size/struct_size after check_shl_overflow; vendor already has them (kept at 4.14.178), duplicate block dropped (redefinition build error); 0 skipped |
| 4.14.197 | conflict | drivers/block/zram/zram_drv.h: SECTOR_SHIFT define removed per stable (now in blkdev.h); vendor already lacks max_zpage_size block; 0 skipped |
| 4.14.198 | clean | - |
| 4.14.199 | clean | - |
| 4.14.200 | clean | - |
| 4.14.201 | conflict | mm/page_alloc.c: include-list adjacency (vendor linux/psi.h vs stable linux/khugepaged.h); kept both; 0 skipped |
| 4.14.202 | clean | - |
| 4.14.203 | clean | - |
| 4.14.204 | conflict | fs/ext4/namei.c, fs/f2fs/namei.c: kept vendor (already uses fscrypt_prepare_link/rename helpers returning -EXDEV, so the EPERM->EXDEV hunks are redundant); drivers/staging/typec/{pd.h,tcpm.c} hunks applied to vendor drivers/usb/typec/tcpm/tcpm.c and include/linux/usb/pd.h; 0 skipped |
| 4.14.205 | clean | - |
| 4.14.206 | clean | - |
| 4.14.207 | clean | - |
| 4.14.208 | clean | - |
| 4.14.209 | clean | - |
| 4.14.210 | clean | - |
| 4.14.211 | clean | - |
| 4.14.212 | clean | - |
| 4.14.213 | clean | - |
| 4.14.214 | clean | - |
| 4.14.215 | clean | - |

Builds: sprd_sharkle_defconfig Image + dtbs built OK at 4.14.147, at 4.14.150
(merged with the watch_414 fragment) and at 4.14.151. Checked with
sprd_sharkle_defconfig + watch_414 fragment at 4.14.171, 182, 192 and 200 (all
OK; breaks found at 182 and 200 were fixed in 4.14.175 and 4.14.196).

Follow-up: the 4.14.164 mmc RPMB character-device conversion (and the 4.14.183
chardev release-order fix) is reverted to the vendor block-device RPMB
interface (mmc_access_rpmb), because the Android 8.1 vendor userspace uses
/dev/block/mmcblk0rpmb. No other mmc fix depends on it.
