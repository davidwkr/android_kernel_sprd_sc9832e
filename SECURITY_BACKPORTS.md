# Security backports

This tree is Linux **4.4.147** (August 2018) with Android 10 / UNISOC changes on top. The 4.4 LTS line ended at **4.4.302** (February 2022), and the Civil Infrastructure Platform (CIP) keeps a security-only 4.4 branch after that. Everything fixed upstream after 4.4.147 is missing unless the vendor or this fork picked it individually.

The watches' stock state, for comparison: kernel 4.4.83, Android security patch level **2019-11-05**, Mali GPU driver **r21p0** (kernel and userspace).

## Spot checks in this tree (2026-10-29)

Grep-level checks for well-known fixes. They indicate, they don't prove: a fix can be present in another form.

| Issue | Impact | In this tree? |
|---|---|---|
| CVE-2019-2215, binder UAF ("Bad Binder", exploited in the wild) | local root from any app | **looks fixed** (`POLLFREE` handling in `binder.c`) |
| CVE-2019-11477/11478/11479, TCP SACK panic / slowness | **remote** DoS over any network | missing in the base; **backported** (see Progress) |
| CVE-2021-0920, `af_unix` garbage collector UAF (exploited in the wild on Android) | local root from any app | missing in the base; **backported** |
| CVE-2021-33909, `seq_file` size overflow ("Sequoia") | local root | missing in the base; **backported** |
| CVE-2021-22555, netfilter `x_tables` heap write | local root (needs user namespaces) | looks fixed |
| CVE-2020-0466 / CVE-2021-1048, epoll UAF (1048 exploited in the wild) | local root | missing in the base; **backported** |
| Meltdown (KPTI) | – | present (`UNMAP_KERNEL_AT_EL0`) |
| Spectre v2 / SSBD | – | absent, **not needed**: Cortex-A53 is not affected by variants 2, 3 and 4 |

## Progress (branch `security-backports`)

Applied from the 4.4 stable versions (github.com/gregkh/linux, tags v4.4.182–v4.4.294). Each commit keeps the original author and its `commit <sha> upstream.` line. Hand-adapted ones say `[ backport: ... ]` in the message. All of it builds with GCC 16.2 (see the build note below); nothing has run on a watch. Current state of all security work: [`STATUS.md`](STATUS.md).

| Fix | CVE | Commits | Adapted? |
|---|---|---|---|
| TCP SACK payload limit, `tcp_fragment` limits, `tcp_min_snd_mss` (+ 2 follow-ups) | CVE-2019-11477/11478/11479 | 6 | `tcp: be more careful in tcp_fragment()`: `tcp.h` context |
| epoll reference + regression fix, insert after sanity checks | CVE-2020-0466, CVE-2021-1048 | 3 | no |
| `seq_file` large allocation limit | CVE-2021-33909 | 1 | no |
| `af_unix` GC vs `MSG_PEEK` (+ prerequisite: split inflight fd functions to `net/unix/scm.c`) | CVE-2021-0920 | 2 | prerequisite: `af_unix.c` part by hand (vendor `unix_sock_count` leftover) |
| `fget` fd recheck | CVE-2021-4083 | 1 | yes: no `fget_many` in this tree, `fput()` instead of `fput_many()` |

| Mali: reject `MEM_FLAGS_CHANGE` on JIT regions | CVE-2022-38181 | 1 | local fix modelled on Arm r40p0 (`KBASE_REG_JIT` check) |
| Mali: `process_mm` reference held for the context lifetime, tracking page no longer sets it | CVE-2023-4211 | 1 | port of the ChromiumOS Bifrost fix to r28p0 (4.4: `mm_count` instead of `mmgrab`) |

The core-kernel commits and `mali.ko` compile with GCC 16.2 (`sprd_sharkle_defconfig`, Mali built as an external module). Nothing has run on a watch yet. The two Mali fixes need review and a GPU test with the vendor r21p0 user space.

### Bugs found during review and fixed in this tree

Not upstream fixes; found while reviewing the merges above.

| Commit | Area | Bug |
|---|---|---|
| `470aacefb` | USB RNDIS gadget (`rndis_rm_hdr()`) | 32-bit wrap of `data_offset + data_len + 8`: a USB host sending `MessageLength=0, DataOffset=0xfffffff8` makes the receive loop clone skbs forever (DoS while RNDIS tethering is on). **Present in the stock firmware**; to be reported to the security team. |
| `f1c3dc049` | musb host (`musb_advance_schedule()`) | `qh->is_ready` written through a possibly freed `qh` after `musb_giveback()`; also left over in the upstream fix 33d7e3723. |
| `9b8774cc4` | `read_pnet_rcu()` / `write_pnet()` | Follow-up to the `dev_net_rcu()` backport: RCU load and publish semantics (only with `CONFIG_NET_NS=y`). |
| `fb18c4753` | SIPC sbuf | Peer notification decided on fresh pointers; a snapshot-only test could lose a wakeup and hang a modem channel. |

### Stable merge 4.4.148 → 4.4.302 (done)

All 155 stable releases are applied, one commit per release (`stable: Linux 4.4.N`), each listing the upstream commits it brings and every manual conflict resolution. The tree builds after 4.4.302 (`sprd_sharkle_defconfig`, GCC 16.2). Not run on a watch yet.

How it was done: there is no shared git history with upstream, so each release is the diff `v4.4.(N-1)..v4.4.N` applied with a 3-way merge. Rules:
- Conflicts in architectures this tree is not built for (x86, mips, powerpc, ...) take the stable file; listed in the commit.
- Files the vendor removed are skipped; listed in the commit.
- **f2fs is not taken from stable** from 4.4.172 on. This tree's f2fs is a much newer vendor backport (about +18,000 lines against 4.4.170) that already contains the 4.4 stable f2fs fixes (checked for 4.4.172: block address verification, superblock/SIT/NAT/inline/node-footer sanity checks, roll-forward recovery, encrypted reads). Update f2fs from the f2fs-stable `linux-4.4` branch instead.
- Fixes already in the tree (vendor or Android backports, or the targeted backports above) keep the tree's form; nothing duplicated.

Follow-up commits on top of the merge:
- `binder: prevent transactions into own process` (second half of 4.4.233, missing from the vendor binder).
- `binder: use euid from cred instead of using task` (part of 4.4.293 the merge missed).
- `binder: use cred instead of task for getsecid` (mainline fix for this tree's binder secctx feature; adds the `security_cred_getsecid()` LSM hook).
- Build fixes after 4.4.168, 4.4.177/178, 4.4.251, 4.4.223 (vendor call sites adjusted to new signatures, restored `struct global_attr` for the vendor cpufreq governors, `USB_SPEED_SUPER_PLUS`).

Not applied (none are security fixes for hardware these watches have):
- USB device-ID / quirk table additions (modems, serial adapters, keyboards) where the vendor table layout conflicts.
- 4.4.249 `option.c` interface sanity checks (older table structure), 4.4.269 `dummy_hcd` fix (driver much older, not built), 4.4.294 USB `hub.c` address0 mutex fixes (needs `hcd->address0_mutex`, which this tree declares but never allocates; enumeration race, not a security issue).
- 4.4.229/231 `xhci-plat` PM change and its revert.

Known inaccuracies in commit messages (history not rewritten): the 4.4.257 and 4.4.269 notes say a USB ID entry was not applied although it was auto-merged; the 4.4.185 note says a duplicate Makefile flag was removed (it was, in a later commit).

Older drivers: some USB drivers in this tree (usbip, xhci-ring, io_edgeport, keyspan_pda, dummy_hcd) were behind upstream before the merge; a few were replaced with the upstream 4.4 file. None of these are enabled in the watch configs.

New Kconfig option: `LDISC_AUTOLOAD` (default y). Consider `n` in the hardening pass.

Next: Mali CVE-2021-28663/28664 and CVE-2022-36449 (need Arm's exact fixes); post-4.4.302 fixes from CIP; UNISOC bulletins; hardening config; first boot on a watch.

## CIP linux-4.4.y-st after 4.4.302 (done on branch `security/cip-4.4-st`)

Source: `https://gitlab.com/cip-project/cip-kernel/linux-cip.git`, branch `linux-4.4.y-st` (security-only 4.4, kept in step with 4.19-st). Range taken: `v4.4.302..6096735c8` ("tree is up-to-date with 4.19-st20"), 6162 commits, linear history, no merges. The branch is one long series of single fixes, there are no CIP release merges to follow.

Method: for every commit the touched files were matched against the objects of a build of `sharkle_full.config` (a `.c`/`.S` file counts only if its `.o` exists), plus the drivers that only the `devices/*/stock.config` files enable (`kernel/audit*`, `ipc/mqueue`, cdc_ncm / cdc_ether / usbnet / ax88179 / net1080 / cdc_subset, xpad, 9p, ext2/3). Then `git cherry-pick -x` in upstream order, one commit per fix, original author kept, no other attribution. Header-only commits were reviewed by hand: those needed by built code (or fixing inline code that built code uses) were taken, the rest skipped. Built after every ~50 commits (`Image modules`); the final build has no errors and no new warnings in touched files (the remaining ones, `free_map` label in `kernel/bpf/syscall.c` and `dpu_lite_r1p0.c` indentation, were there before).

Counts: 6162 CIP commits; **1257 taken** (1225 applied by `cherry-pick` incl. 2 header prerequisites, 24 header-only fixes, 8 hand-resolved after a conflict; 12 carry a `[ backport ... ]` note); 38 conflicts left out (below, 3 of them already present); about 4700 not built; 12 touch `fs/f2fs` (skipped, vendor f2fs); 44 CIP-internal (`localversion-st`, version bumps, CI).

Skipped because the code is not built (about 4700 commits, not listed one by one): other architectures (about 375 x86/powerpc/mips/s390/... and most arm), drivers not in the config (about 1150 in drivers/net, media, scsi, gpu, plus bluetooth, nfc, can, xen, atm, ...), filesystems not built (btrfs, xfs, nilfs2, ocfs2, jfs, nfs, ...), netfilter parts not built (nf_tables, ipset), sctp, dccp, tipc, vsock.

New Kconfig option from the series: `BPF_UNPRIV_DEFAULT_OFF` (default n here, from "bpf: Add kconfig knob for disabling unpriv bpf by default"); set to `y` in `watch_hardening.fragment` (unprivileged `bpf()` off by default; root can re-enable it).

Hand-resolved or adapted (all carry a `[ backport: ... ]` note):
- `net: add dev_net_rcu() helper` (prerequisite of the ipv4/ipv6/arp/ndisc RCU fixes): `possible_net_t.net` is not `__rcu` here, so `read_pnet_rcu()` uses `lockless_dereference()` (the load `rcu_dereference()` does) and `write_pnet()` publishes with `smp_store_release()`, like `rcu_assign_pointer()`. Only matters with `CONFIG_NET_NS=y`.
- `rtnetlink: Reject negative ifindexes in RTM_NEWLINK`: dropped the `NL_SET_ERR_MSG()` (no extack in 4.4); the CIP `Revert` that follows was applied by hand (the setlink variant stays, as in CIP).
- `audit: fix potential integer overflow in audit_log_n_string()` and `lib/ts_kmp: fix integer overflow ...`: operand types made `size_t` (4.4 `check_*_overflow()` needs identical types, otherwise a compiler warning).
- `bpf: fix overflow in prog accounting`: `bpf.h` stubs merged with the vendor `bpf_prog_get_type_path()` stub.
- `mm/slub: avoid accessing metadata when pointer is invalid in object_err()`: vendor `panic()` after the report kept.
- `ext4: fix wrong unit use in ext4_mb_clear_bb`: vendor `ext4_issue_discard()` flags argument kept.
- `usb: musb: Get the musb_qh pointer after musb_giveback`: the `musb_urb_dequeue()` half was already in the vendor driver, only the `musb_advance_schedule()` half is new.
- `USB: usbcore: Introduce usb_bulk_msg_killable()`: vendor `dev_info()` kept.
- `mmc: core: Fix kernel panic when remove non-standard SDIO card`: quirk test nested in the vendor `CONFIG_MMC_EMBEDDED_SDIO` test.
- `usb: gadget: f_fs: initialize reset_work at allocation time`: both INIT_WORK and the vendor `wait_gadget` init kept.

Left out after a conflict (38; none is a missed security fix as far as reviewed):
- `950f5723` fget: clarify and improve __fget_files() implementation: ALREADY PRESENT: cleanup of the CVE-2021-4083 fix; the recheck loop is already in fs/file.c (earlier backport)
- NOT APPLIED `160f6f91` mmc: host: Return an error when ->enable_sdio_irq() ops is missing: NOT APPLIED: mmc host error return for a missing ->enable_sdio_irq(); robustness only, vendor mmc/core/host.c heavily changed
- `9416f9d6` mm: page_alloc: fix building error on -Werror=array-compare: skipped: compiler-warning fix (-Werror=array-compare)
- `61c21e35` USB: quirks: add a Realtek card reader: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `2bc504b4` USB: quirks: add STRING quirk for VCOM device: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `c133859a` x86/speculation: Include unprivileged eBPF status in Spectre v2 mitigation reporting: skipped: x86 only (Spectre v2 reporting)
- NOT APPLIED `ba55a387` mmc: core: Default to generic_cmd6_time as timeout in __mmc_switch(): NOT APPLIED: mmc CMD6 default timeout; not a security fix, vendor __mmc_switch() rewritten
- `b7678719` USB: new quirk for Dell Gen 2 devices: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- NOT APPLIED `71874aeb` fs-writeback: writeback_sb_inodes：Recalculate 'wrote' according skipped pages: NOT APPLIED: writeback accounting tweak, not a security fix; vendor IODEBUG_BDI code in the same function
- `96567cbc` USB: cdc-acm: Add Icom PMR F3400 support (0c26:0020): device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `19cb1524` USB: serial: ftdi_sio: add Belimo device ids: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `64ed5abf` USB: serial: ftdi_sio: add Omron CS1W-CIF31 device id: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `10d5cf1c` xhci: Add grace period after xHC start to prevent premature runtime suspend.: skipped: xhci not built
- `1fcc2e3b` uas: ignore UAS for Thinkplus chips: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `62d483c8` usb-storage: apply IGNORE_UAS only for HIKSEMI MD202 on RTL9210: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `7d0ac8a1` usb: core: add quirk for Alcor Link AK9563 smartcard reader: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `a7735ab5` uas: Add US_FL_NO_REPORT_OPCODES for JMicron JMS583Gen 2: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `3ee3c050` kernel/extable.c: use address-of operator on section symbols: skipped: clang/GCC build warning only
- `b5a8faa6` lib/dynamic_debug.c: use address-of operator on section symbols: skipped: clang/GCC build warning only
- `8f1d3544` string: uninline memcpy_and_pad: skipped: uninline of memcpy_and_pad, size optimisation only
- `2f21405b` USB: serial: ftdi_sio: add support for GMC Z216C Adapter IR-USB: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `39ddc2e0` net: slightly optimize eth_type_trans: skipped: eth_type_trans micro-optimisation
- `f6d2aecf` usb: gadget: f_fs: Fix a race condition when processing setup packets.: ALREADY PRESENT / N/A: vendor ffs_func_setup() never returns USB_GADGET_DELAYED_STATUS, so there is no race to fix
- `6f0cfd14` USB: Add USB_QUIRK_NO_SET_INTF quirk for START BP-850k: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `85c9c73a` cdc-acm: Add DISABLE_ECHO quirk for GE HealthCare UI Controller: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `0b3e511b` USB: Add USB_QUIRK_NO_LPM quirk for sony xperia xz1 smartphone: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `9e4d0841` USB: serial: ftdi_sio: add support for Altera USB Blaster 3: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `5730717d` USB: serial: ftdi_sio: add support for Abacus Electrics Optical Probe: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `8dfbfcd4` Logitech C-270 even more broken: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `2b18ab7b` USB: storage: Ignore driver CD mode for Realtek multi-mode Wi-Fi dongles: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `f0c17563` USB: serial: ftdi_sio: add support for u-blox EVK-M101: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `bcefa457` USB: serial: ftdi_sio: add support for PICAXE AXE027 cable: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `09490469` arm64: replace Sun/Solaris style flag on section directive: skipped: clang/assembler build fix only
- `5c5fedf0` cdrom, scsi: sr: propagate read-only status to block layer via set_disk_ro(): skipped: drivers/scsi/sr not built
- `3bfbffb2` usb: storage: Add quirks for PNY Elite Portable SSD: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `a7ab7779` usb: gadget: function: rndis: add length check for header: ALREADY PRESENT: vendor rndis_rm_hdr() already checks skb->len against the header and message length
- `2a5515b6` usb: cdc_acm: Add quirk for Uniden BC125AT scanner: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)
- `876d58b6` USB: serial: ftdi_sio: add support for E+H FXA291: device-ID / quirk table addition, vendor tables conflict (same policy as the stable merge)

Fixes taken (upstream CIP/stable hash, subject; `*` = adapted by hand), in application order:

- `da0f7c751d03` netfilter: nat: remove l4 protocol port rovers
- `d707a4f8965d` rtnetlink: make sure to refresh master_dev/m_ops in __rtnl_newlink()
- `43d394cd9463` af_packet: fix data-race in packet_setsockopt / packet_setsockopt
- `86ad5e66a4c5` ASoC: ops: Reject out of bounds values in snd_soc_put_volsw()
- `7a78bde1faa4` ASoC: ops: Reject out of bounds values in snd_soc_put_volsw_sx()
- `46057f6ddfdd` ASoC: ops: Reject out of bounds values in snd_soc_put_xr_sx()
- `ecf34f8f6c6d` ext4: fix error handling in ext4_restore_inline_data()
- `6d46b2a0c475` bpf: Add kconfig knob for disabling unpriv bpf by default
- `3b040cd676a7` integrity: check the return value of audit_log_start()
- `09c7ca640080` n_tty: wake up poll(POLLRDNORM) on receiving data
- `1aa0a528693e` USB: gadget: validate interface OS descriptor requests
- `e267b88f1cb2` usb: gadget: rndis: check size of RNDIS_MSG_SET command
- `e15f4029a2a0` net: usb: ax88179_178a: Fix out-of-bounds accesses in RX fixup
- `b205512f37f5` quota: make dquot_quota_sync return errors from ->sync_fs
- `4e9d3e9892e3` xfrm: Don't accidentally set RTO_ONLINK in decode_session4()
- `09965cb6bf22` taskstats: Cleanup the use of task->exit_code
- `28fababe6d61` ASoC: ops: Fix stereo change notifications in snd_soc_put_volsw()
- `8f29266ba5fc` ASoC: ops: Fix stereo change notifications in snd_soc_put_volsw_range()
- `38654404797f` EDAC: Fix calculation of returned address and next offset in edac_align_ptr()
- `f78208aabf8c` tracing: Fix tp_printk option related with tp_printk_stop_on_boot
- `925f10a0799f` USB: zaurus: support another broken Zaurus
- `ef1251dd5e75` gso: do not skip outer ip header in case of ipip and net_failover
- `a268b246a265` configfs: fix a race in configfs_{,un}register_subsystem()
- `b1e7d369abbb` tracefs: Set the group ownership in apply_options() not parse_options()
- `48deed983d3b` Input: clear BTN_RIGHT/MIDDLE on buttonpads
- `e41846dbe6f4` ASoC: ops: Shift tested values in snd_soc_put_volsw() by +min
- `7dc406d484a8` xfrm: fix MTU regression
- `779f60d01492` netfilter: nf_queue: don't assume sk is full socket
- `0bf4da4ac8cd` netfilter: nf_queue: fix possible use-after-free
- `40a5e67ecaeb` HID: add mapping for KEY_ALL_APPLICATIONS
- `e575e5e25df9` net-sysfs: add check for netdevice being present to speed_show
- `cdca39eea325` tracing: Ensure trace buffer is at least 4096 bytes large
- `05093187fc99` xfrm: Fix xfrm migrate issues when address family changes
- `760a14e25b57` nl80211: Update bss channel on channel switch for P2P_CLIENT
- `883524fff20e` tcp: make tcp_read_sock() more robust
- `79ebd397afa0` fs: sysfs_emit: Remove PAGE_SIZE alignment check
- `b63f1369cfbe` net/packet: fix slab-out-of-bounds access in packet_recvmsg()
- `debcd5bcbe8a` usb: gadget: rndis: prevent integer overflow in rndis_set_response()
- `c383c8867e93` Input: aiptek - properly check endpoint type
- `a8fe31a09784` net: ipv6: fix skb_over_panic in __ip6_append_data
- `af951e651803` ALSA: pcm: Add stream lock during PCM reset ioctl operations
- `c6f56e045330` ALSA: usb-audio: Add mute TLV for playback volumes on RODE NT-USB
- `e2db5bb14d72` arm64: Add helper to decode register from instruction
- `fcdd600eb847` af_key: add __GFP_ZERO flag for compose_sadb_supported in function pfkey_register
- `2e98922ca63c` iio: inkern: apply consumer scale on IIO_VAL_INT cases
- `24901cffb2fd` iio: inkern: make a best effort on offset calculation
- `c718b2b7667f` mm/pages_alloc.c: don't create ZONE_MOVABLE beyond the end of a node
- `a6d4311e9738` Revert "Input: clear BTN_RIGHT/MIDDLE on buttonpads"
- `642826905251` sched/debug: Remove mpol_get/put and task_lock/unlock from sched_show_numa
- `35c664e13653` video: fbdev: fbcvt.c: fix printing in fb_cvt_print_name()
- `2515147e1084` ext2: correct max file size computing
- `8fe9d21fec39` af_netlink: Fix shift out of bounds in group mask calculation
- `c8e1202c9c17` netfilter: nf_conntrack_tcp: preserve liberal flag in tcp options
- `49313bc1a393` loop: use sysfs_emit() in the sysfs xxx show()
- `c3c3877d44ee` ext4: don't BUG if someone dirty pages without asking ext4 first
- `0dd79bebb2e2` ASoC: soc-core: skip zero num_dai component in searching dai name
- `f7c7c6a6dec8` pinctrl: pinconf-generic: Print arguments for bias-pull-*
- `9b03c3f6397d` mm/mmap: return 1 from stack_guard_gap __setup() handler
- `6eb63a9c1402` dm ioctl: prevent potential spectre v1 gadget
- `549d58e28522` mmmremap.c: avoid pointless invalidate_range_start/end on mremap(old_size=0)
- `fd7bf19d393f` arm64: patch_text: Fixup last cpu should be master
- `d4ac1dd8227f` xfrm: policy: match with both mark and mask on user interfaces
- `37a52f172623` ALSA: pcm: Test for "silence" field in struct "pcm_format_data"
- `932188b6167d` smp: Fix offline cpu check in flush_smp_call_function_queue()
- `66fb468fe154` ALSA: usb-audio: Clear MIDI port active flag after draining
- `36bb65e79f30` net/packet: fix packet_sock xmit return value checking
- `b9bdfcc6b533` netlink: reset network and mac headers in netlink_dump()
- `106c18ff1916` ASoC: soc-dapm: fix two incorrect uses of list iterator
- `a05ed381ff15` ext4: limit length to bitmap_maxbytes - blocksize in punch_hole
- `ac4c3d82f99f` ext4: fix overhead calculation to account for the reserved gdt blocks
- `1725c1209298` ext4: force overhead calculation if the s_overhead_cluster makes no sense
- `228b9db3e931` block/compat_ioctl: fix range check in BLKGETSIZE
- `123be0d8167f` usb: gadget: configfs: clear deactivation flag in configfs_composite_unbind()
- `5ce298031845` hex2bin: make the function hex_to_bin constant-time
- `0877f8085c12` hex2bin: fix access beyond string end
- `84fd4105aa48` net: igmp: respect RCU rules in ip_mc_source() and ip_mc_msfilter()
- `69f31f361fec` net: sched: prevent UAF on tc_ctl_tfilter when temporarily dropping rtnl_lock
- `2c8153a9b978` dm: interlock pending dm_io and dm_wait_for_bios_completion
- `929d46ff6e73` ptrace: Check PTRACE_O_SUSPEND_SECCOMP permission on PTRACE_SEIZE
- `57e42dd4d78e` x86, modpost: Replace last remnants of RETPOLINE with CONFIG_RETPOLINE
- `4423bb812e30` arm/arm64: smccc/psci: add arm_smccc_1_1_get_conduit()
- `f1d6c5898d2c` ipv4: drop dst in multicast routing path
- `f626e0bf429a` netlink: do not reset transport header in netlink_recvmsg()
- `9c796e783c8b` ASoC: ops: Validate input values in snd_soc_put_volsw_range()
- `abdff756c0b4` ping: fix address binding wrt vrf
- `41f0ae4fd900` Input: add bounds checking to input_set_capability()
- `3b1975cbf265` mmc: block: Use generic_cmd6_time when modifying INAND_CMD38_ARG_EXT_CSD
- `81ed65304315` perf: Fix sys_perf_event_open() race against self
- `9fca68c8f5fe` net: af_key: add check for pfkey_broadcast in function pfkey_process
- `cbb8a40505e7` net: af_key: check encryption module availability consistency
- `f148a259c6c9` assoc_array: Fix BUG_ON during garbage collect
- `02bb944067f3` block-map: add __GFP_ZERO flag for alloc_page in function bio_copy_kern
- `5afe97e9f7b3` exec: Force single empty string when argv is empty
- `ac1a9349d820` dm crypt: make printing of the key constant-time
- `70d4c2285426` dm stats: add cond_resched when looping over entries
- `8c5a10bcafba` ptrace: Reimplement PTRACE_KILL by always sending SIGKILL
- `c24cfcf1d4c2` ASoC: dapm: Don't fold register value changes into notifications
- `db8856a88e8b` fat: add ratelimit to fat*_ent_bread()
- `e20476eb92e4` inotify: show inotify mask flags in proc fdinfo
- `7d010fd9c7c5` tty: fix deadlock caused by calling printk() under tty_port->lock
- `7bf550365de8` ext4: fix bug_on in ext4_writepages
- `e1924c6792f1` ext4: verify dir block before splitting it
- `af860c85c5c9` hugetlb: fix huge_pmd_unshare address update
- `9e54a6c4536c` modpost: fix removing numeric suffixes
- `a935f8577dca` tracing: Avoid adding tracer option before update_tracer_options
- `99a0d28fcc2a` net: fix nla_strcmp to handle more then one trailing null character
- `2e3d10c1c0be` net: xfrm: unexport __init-annotated xfrm4_protocol_init()
- `ec900c51e247` Revert "net: af_key: add check for pfkey_broadcast in function pfkey_process"
- `16597eb50a0a` modpost: fix undefined behavior of is_arm_mapping_symbol()
- `82ae2b380f4c` tcp: fix tcp_mtup_probe_success vs wrong snd_cwnd
- `9aa08d4ab960` virtio-mmio: fix missing put_device() when vm_cmdline_parent registration failed
- `f93cf9584179` ext4: fix bug_on ext4_mb_use_inode_pa
- `aa5b6af74d66` ext4: make variable "count" signed
- `dc918cd07cd1` ext4: add reserved GDT blocks check
- `eab96b337aed` l2tp: don't use inet_shutdown on ppp session destroy
- `b241b0c3fd3b` l2tp: fix race in pppol2tp_release with session object destroy
- `7cdc9b5de062` fuse: fix pipe buffer lifetime for direct_io
- `7234b5152242` Reinstate some of "swiotlb: rework "fix info leak with DMA_FROM_DEVICE""
- `f55c1d79bab7` tcp: change source port randomizarion at connect() time
- `10dbc9108644` tcp: add some entropy in __inet_hash_connect()
- `d21ead6d5064` secure_seq: use the 64 bits of the siphash for port offset calculation
- `18fbed4b4b42` tcp: use different parts of the port_offset for index and offset
- `033b4de3a032` tcp: add small random increments to the source port
- `d7cdb286c9ee` tcp: dynamically allocate the perturb table used by source ports
- `92decc8137fa` tcp: increase source port perturb table to 2^16
- `a5cf5180a5e3` tcp: drop the hash_32() part from the index calculation
- `5f06f7c653d3` swiotlb: skip swiotlb_bounce when orig_addr is zero
- `dbbdd68f52ee` net: usb: ax88179_178a: Fix packet receiving
- `e5c7380dbf56` usbnet: make sure no NULL pointer is passed through
- `e5895aec9a0b` usbnet: fix memory allocation in helpers
- `d793f0bb054d` sit: use min
- `5f1c7e3af96f` ipv6/sit: fix ipip6_tunnel_get_prl return value
- `0bea6c336bed` net: Rename and export copy_skb_header
- `a04efb947438` mm/slub: add missing TID updates on slab deactivation
- `bb16c917e6f7` usbnet: fix memory leak in error case
- `00a908fa08a0` ida: don't use BUG_ON() for debugging
- `fb8702899918` icmp: Fix data-races around sysctl.
- `e4238b6cb1b9` virtio_mmio: Add missing PM calls to freeze/restore
- `5daa6a36dfc8` virtio_mmio: Restore guest page size on resume
- `477df4a859b4` ASoC: ops: Fix off by one in range control validation
- `fe2aacef5b67` signal handling: don't use BUG_ON() for debugging
- `bc45c9cd7662` security,selinux,smack: kill security_task_wait hook
- `ae8471b0be05` xfrm: xfrm_policy: fix a possible double xfrm_pols_put() in xfrm_bundle_lookup()
- `f6ac83a15171` perf/core: Fix data race between perf_event_set_output() and perf_mmap_close()
- `1e6bd7d6df1b` tcp: Fix a data-race around sysctl_tcp_probe_threshold.
- `59f711abb937` bpf: Make sure mac_header was set before using it
- `35986ac245c5` ALSA: memalloc: Align buffer allocations in page size
- `fa66cbfefaa7` tty: drop tty_schedule_flip()
- `d19ba79906d6` tty: extract tty_flip_buffer_commit() from tty_flip_buffer_push()
- `f6412965114f` tty: use new tty_insert_flip_string_and_push_buffer() in pty_write()
- `2b429ea2f8f3` netfilter: nf_queue: do not allow packet truncation below transport header offset
- `ad1bc3da8e44` proc: Pass file mode to proc_pid_make_inode
- `04d09c043916` init/main: Fix double "the" in comment
- `cf915b47e1a0` init/main: properly align the multi-line comment
- `6348b678f5db` vfs: Check the truncate maximum size in inode_newsize_ok()
- `0c2a9da968f2` usbnet: Fix linkwatch use-after-free on disconnect
- `feca51416811` fuse: limit nsec
- `18cb463ae8ab` USB: HCD: Fix URB giveback issue in tasklet function
- `a4863e487764` ext4: make sure ext4_append() always allocates new block
- `c82524fb8fe2` ext4: fix use-after-free in ext4_xattr_set_entry
- `7bb6c7f4dca8` ext4: update s_overhead_clusters in the superblock during an on-line resize
- `35a8149422e8` ext4: correct max_inline_xattr_value_size computing
- `fd1f43710c48` net/9p: Initialize the iounit field during fid creation
- `f1dc37af0b6f` ALSA: info: Fix llseek return value when using callback
- `268714308b9e` ext4: avoid resizing to a partial cluster size
- `c668451bef49` ALSA: core: Add async signal helpers
- `ff8fd8ade453` ALSA: timer: Use deferred fasync helper
- `4d5886246442` xfrm: fix refcount leak in __xfrm_policy_check()
- `d12d127f530f` af_key: Do not call xfrm_probe_algs in parallel
- `e7290da677e8` net: Fix a data-race around sysctl_tstamp_allow_data.
- `ad24cc0ff846` net: Fix a data-race around sysctl_net_busy_read.
- `22f15a0f8fac` net: Fix a data-race around sysctl_somaxconn.
- `f94074ab9098` loop: Check for overflow while configuring loop
- `f4d425777135` mm/hugetlb: fix hugetlb not supporting softdirty tracking
- `cf29096b529b` mm: Force TLB flush for PFNMAP mappings before unlink_file_vma()
- `ee94e46e9728` HID: hidraw: fix memory leak in hidraw_release()
- `8fb68a753d47` ftrace: Fix NULL pointer dereference in is_ftrace_trampoline when ftrace is dead
- `63c0af50bed3` mm/rmap: Fix anon_vma->degree ambiguity leading to double-reuse
- `277f1c0cb0bf` USB: core: Prevent nested device-reset calls
- `53ed87bcec22` usb: gadget: mass_storage: Fix cdrom data transfers on MAC-OS
- `5d45c78c420a` ALSA: usb-audio: Fix an out-of-bounds bug in __snd_usb_parse_audio_interface()
- `05a345f13baf` netfilter: nf_conntrack_irc: Fix forged IP logic
- `935e2c93a2bd` mm: Fix TLB flush for not-first PFNMAP mappings in unmap_region()
- `1633d474f529` tracefs: Only clobber mode/uid/gid on remount if asked
- `81bc741a566b` net: ping6: Fix memleak in ipv6_renew_options().
- `3564fad7204e` tcp: Fix missmerge in perturb table handling
- `8fb3f2c3b466` USB: core: Fix RST error in hub.c
- `e5377a5cc97a` mm/slub: fix to return errno if kmalloc() fails
- `da1dc4938bcf` netfilter: nf_conntrack_sip: fix ct_sip_walk_headers
- `74ce39309039` netfilter: nf_conntrack_irc: Tighten matching on DCC message
- `749e5591866b` ext4: make directory inode spreading reflect flexbg size
- `243912f1ab20` mm: prevent page_frag_alloc() from corrupting the memory
- `82dc841eab7a` usbnet: Fix memory leak in usbnet_disconnect()
- `802ba69f686e` fs: fix UAF/GPF bug in nilfs_mdt_destroy
- `70956a16c123` usb: mon: make mmapped memory read only
- `6897ddd976ea` Input: xpad - add supported devices as contributed on github
- `016e57a94338` Input: xpad - fix wireless 360 controller breaking after suspend
- `ac80202e574d` ALSA: rawmidi: Drop register_mutex in snd_rawmidi_free()
- `e5f586d3d09d` ALSA: usb-audio: Fix potential memory leaks
- `eb9191b60303` ALSA: usb-audio: Fix NULL dererence at error path
- `005b98a70584` quota: Check next/prev free block number after reading from quota file
- `bb3efcf56d34` ext4: avoid crash when inline data creation follows DIO write
- `9100bfc847ac` ext4: fix null-ptr-deref in ext4_write_info
- `d3d6439dd0ea` ext4: make ext4_lazyinit_thread freezable
- `a500ce3b011a` ext4: place buffer head allocation before handle start
- `778afcbab0f0` USB: serial: ftdi_sio: add support for Brainboxes US-159/235/320
- `743390f00a4a` USB: serial: pl2303: add IBM device IDs
- `780c647d00d9` ALSA: usb-audio: Fix undefined behavior due to shift overflowing the constant
- `316f785fa141` USB: serial: pl2303: add device id for HP LM930 Display
- `f714f5717657` scsi: vmw_pvscsi: Expand vcpuHint to 16 bits
- `0c650c8d0a59` USB: serial: io_ti: add Agilent E5805A support
- `1a117d0c2cb4` ext4: add EXT4_INODE_HAS_XATTR_SPACE macro in xattr.h
- `32b29c2114fe` usb-storage: Add ignore-residue quirk for NXP PN7462AU
- `746a73a206d3` uas: add no-uas quirk for Hiksemi usb_disk
- `7c26893a76a0` usb-storage: Add Hiksemi USB3-FW to IGNORE_UAS
- `58ebfecfc288` ring-buffer: Allow splice to read previous partially read pages
- `48c3dbb087fb` ring-buffer: Check pending waiters when doing wake ups as well
- `550fb22ad4b8` ring-buffer: Fix race between reset page and reading page
- `e0f92aea3264` tcp: fix tcp_cwnd_validate() to not forget is_cwnd_limited
- `c8d8c8605bf1` iio: inkern: only release the device node when done with it
- `d18e268df352` dyndbg: let query-modname override actual module name
- `ccfe7dfa39d5` tcp: annotate data-race around tcp_md5sig_pool_populated
- `183c60e319f5` xfrm: Update ipcomp_scratches with NULL when freed
- `820c24633f01` HID: roccat: Fix use-after-free in roccat_read()
- `d805cddb60fb` usb: musb: Fix musb_gadget.c rxstate overflow bug
- `191c25829cac` Revert "usb: storage: Add quirk for Samsung Fit flash"
- `d6a12e303c8e` ext4: continue to expand file system when the target size doesn't reach
- `f3d7398a12be` inet: fully convert sk->sk_rx_dst to RCU rules
- `d75a786ec6b1` HID: magicmouse: Do not set BTN_MOUSE on double report
- `8d05827d14fe` USB: add RESET_RESUME quirk for NVIDIA Jetson devices in RCM
- `30d583eb7a4f` kernfs: fix use-after-free in __kernfs_remove
- `f9c1a649bf44` mm,hugetlb: take hugetlb_lock before decrementing h->resv_huge_pages
- `4359043c02cc` tcp: fix indefinite deferral of RTO with SACK reneging
- `91f5321f86ca` net: mdio: fix undefined behavior in bit shift for __mdiobus_register
- `b0f836038e4e` ALSA: usb-audio: Add quirks for MacroSilicon MS2100/MS2106 devices
- `60e6ba25dbf0` ext4: fix warning in 'ext4_da_release_space'
- `0f8690d89dcc` ipv6: addrlabel: fix infoleak when sending struct ifaddrlblmsg to network
- `be88e6ea5a64` ALSA: usb-audio: Add quirk entry for M-Audio Micro
- `560f70ac3dc1` dmaengine: at_hdmac: Fix at_lli struct definition
- `0b59b4f95b73` ASoC: core: Fix use-after-free in snd_soc_exit()
- `cc3d9d734f98` ASoC: soc-utils: Remove __exit for snd_soc_util_exit()
- `88649f65ab1f` pinctrl: devicetree: fix null pointer dereferencing in pinctrl_dt_to_map
- `027de7d68ca3` ftrace: Fix the possible incorrect kernel message
- `815d76cbe013` ftrace: Optimize the allocation for mcount entries
- `c5846c36ae5e` ring_buffer: Do not deactivate non-existant pages
- `ade4ca536801` ALSA: usb-audio: Drop snd_BUG_ON() from snd_usbmidi_output_open()
- `cc02cf3c4a84` usb: add NO_LPM quirk for Realforce 87U Keyboard
- `6887ff23a633` dm ioctl: fix misbehavior if list_versions races with module loading
- `0f60f61451a0` mmc: core: properly select voltage range without power cycle
- `6da7bb29ba99` 9p: trans_fd/p9_conn_cancel: drop client lock earlier
- `541cd3051873` 9p/trans_fd: always use O_NONBLOCK read/write
- `a2976051e946` mm: fs: initialize fsdata passed to write_begin/write_end interface
- `72cf3a5d0281` af_key: Fix send_acquire race with pfkey_register
- `be633e158355` 9p/fd: fix issue of list_del corruption in p9_fd_cancel()
- `543ac7a56fff` xfrm: Fix ignored return value in xfrm6_init()
- `4f7ad893a6a1` tcp: configurable source port perturb table size
- `1a4a5f088037` net/9p: Fix a potential socket leak in p9_socket_open
- `650712ec67b9` packet: do not set TP_STATUS_CSUM_VALID on CHECKSUM_COMPLETE
- `787686e7dccc` ASoC: ops: Fix bounds check for _sx controls
- `3f3fcecacfda` tcp/udp: Fix memory leak in ipv6_renew_options().
- `6c728f23edde` proc: avoid integer type confusion in get_proc_long
- `d07f6d35bd19` ASoC: soc-pcm: Add NULL check in BE reparenting
- `794c5ee392b9` HID: hid-lg4ff: Add check for empty lbuf
- `cfc5604ebce2` HID: core: fix shift-out-of-bounds in hid_report_raw_event
- `535151136987` block: unhash blkdev part inode when the part is deleted
- `acf37a15557b` ASoC: ops: Check bounds for second channel in snd_soc_put_volsw_sx()
- `0f8b4545ad0d` ASoC: ops: Correct bounds check for second channel on SX controls
- `592a019a738e` net: loopback: use NET_NAME_PREDICTABLE for name_assign_type
- `2a17effeadfd` pstore: Avoid kcore oops by vmap()ing with VM_IOREMAP
- `ef23791f618a` cpuidle: dt: Return the correct numbers of parsed idle states
- `c42e23be84f8` fs: don't audit the capability check in simple_xattr_list()
- `39edf4431ccd` perf: Fix possible memleak in pmu_dev_alloc()
- `0780d970c926` libfs: add DEFINE_SIMPLE_ATTRIBUTE_SIGNED for signed value
- `88f16859e74a` regulator: core: fix unbalanced of node refcount in regulator_dev_lookup()
- `da1704bcea06` pinctrl: pinconf-generic: add missing of_node_put()
- `27621133cd61` regulator: core: fix module refcount leak in set_supply()
- `551677b6661d` blktrace: Fix output non-blktrace event when blk_classic option enabled
- `9b99fe011dd0` net/tunnel: wait until all sk_user_data reader finish before releasing the sock
- `f158b59aeda1` chardev: fix error handling in cdev_device_add()
- `2e93b44a6cfb` power: supply: fix residue sysfs file in error handle route of __power_supply_register()
- `d51739208ff5` skbuff: Account for tail adjustment during pull operations
- `27a822aebf16` net_sched: reject TCF_EM_SIMPLE case for complex ematch module
- `a7aa8a465309` net: stream: purge sk_error_queue in sk_stream_kill_queues()
- `6d20a24c402a` ppp: associate skb with a device at tx
- `4f4f698f1cc4` blk-mq: fix possible memleak when register 'hctx' failed
- `581940883384` HID: plantronics: Additional PIDs for double volume key presses quirk
- `5f42cabfcb67` pnode: terminate at peers of source
- `5e7f0d792dd3` mmc: sdhci: Fix voltage switch delay
- `ddb403d288bb` HID: wacom: Ensure bootloader PID is usable in hidraw mode
- `c0ff79400101` net: introduce lockdep_is_held and update various places to use it
- `9ede6665075e` tracing: Fix infinite loop in tracing_read_pipe on overflowed print_trace_line
- `32319bc337a7` device_cgroup: Roll back to original exceptions after copy failure
- `be9611743197` ext4: add inode table check in __ext4_get_inode_loc to aovid possible infinite loop
- `5afdf818e11a` ext4: fix undefined behavior in bit shift for ext4_check_flag_values
- `91c683d64f43` ext4: fix bug_on in __es_tree_search caused by bad boot loader inode
- `716c50d63379` ext4: init quota for 'old.inode' in 'ext4_rename'
- `0ffd5a4077bd` ext4: fix error code return to user-space in ext4_get_branch()
- `7bb7947767be` ext4: avoid BUG_ON when creating xattrs
- `39bdf0444b23` mm, oom: do not trigger out_of_memory from the #PF
- `0c7ccf7e18c7` usb: musb: remove extra check in musb_gadget_vbus_draw
- `b66a419c08f2` class: fix possible memory leak in __class_register()
- `72806fdf56b8` ipv6: raw: Deduct extension header length in rawv6_push_pending_frames
- `88cd729efce0` prlimit: do_prlimit needs to have a speculation check
- `3caa738a85c7` HID: check empty report_list in hid_validate_values()
- `2ec791e9db4d` usb: gadget: f_fs: Prevent race during ffs_ep0_queue_wait
- `8c3e46b8a69f` usb: gadget: f_fs: Ensure ep0req is dequeued before free_request
- `5ebe61e75bdd` dmaengine: Fix double increment of client_count in dma_chan_get()
- `502e9124f076` module: Don't wait for GOING modules
- `118d749e8f7e` netlink: annotate data races around dst_portid and dst_group
- `829c0e072d13` netlink: annotate data races around sk_state
- `5009c5168d95` netfilter: conntrack: fix vtag checks for ABORT/SHUTDOWN_COMPLETE
- `c6f1f0202a36` sysctl: add a new register_sysctl_init() interface
- `02fbefecb364` panic: unset panic_on_warn inside panic()
- `9197645eaf37` usb: gadget: f_fs: Fix unbalanced spinlock in __ffs_ep0_queue_wait
- `9a977a3c8fab` mm: hugetlb: proc: check for hugetlb shared PMD in /proc/PID/smaps
- `1cb8592e28e1` aio: fix mremap after fork null-deref
- `e500cfce9b53` mmc: sdio: fix possible resource leaks in some error paths
- `e6f2bae11168` net: sched: disallow noqueue for qdisc classes
- `75c70c70f237` USB: hub: Fix the broken detection of USB3 device in SMSC hub
- `2781cc1d16ff` usb: core: hub: disable autosuspend for TI TUSB8041
- `2d0444bd77aa` usb: gadget: f_ncm: fix potential NULL ptr deref in ncm_bitrate()
- `ee158ecf8c76` EDAC/device: Respect any driver-supplied workqueue polling value
- `a7d5e6209ff7` ipv6: ensure sane device mtu in tunnels
- `e407c5d86016` mm/swapfile: add cond_resched() in get_swap_pages()
- `b87df962ebdb` net: Remove WARN_ON_ONCE(sk->sk_forward_alloc) from sk_stream_kill_queues().
- `c59c4baa49e4` bpf: fix subprog verifier bypass by div/mod by 0 exception
- `572efb3c45b5` USB: core: Don't hold device lock while reading the "descriptors" sysfs file
- `5df18176cd3c` crypto: seqiv - Handle EBUSY correctly
- `01c576fedd80` dm: remove flush_scheduled_work() during local_exit()
- `a3f90f806278` netfilter: ctnetlink: fix possible refcount leak in ctnetlink_create_conntrack()
- `87f66d6ffb60` net: fix __dev_kfree_skb_any() vs drop monitor
- `b8361681beac` fs: prevent out-of-bounds array speculation when closing a file descriptor
- `655f86503eb8` ext4: fix RENAME_WHITEOUT handling for inline directories
- `51267c9c9cf8` ext4: fix WARNING in ext4_update_inline_data
- `a02f7f99c465` ext4: zero i_disksize when initializing the bootloader inode
- `965dd7eb9453` ext4: fix cgroup writeback accounting with fs-layer encryption
- `eac9b0d9a3e5` fs: sysfs_emit_at: Remove PAGE_SIZE alignment check
- `4da5917691bc` ipv4: Fix incorrect table ID in IOCTL path
- `b34049e10b56` ext4: fail ext4_iget if special inode unallocated
- `41df3950b79f` ftrace: Fix invalid address access in lookup_rec() when index is 0
- `6b56050dc7c5` HID: core: Provide new max_buffer_size attribute to over-ride the default
- `4068e9a8b3c9` HID: uhid: Over-ride the default maximum data buffer value with our own
- `5d2796a2ff49` ima: Align ima_file_mmap() parameters with mmap_file LSM hook
- `0b7a172baa90` irqdomain: Fix association race
- `45e00f167c59` irqdomain: Fix disassociation race
- `dc8a535c5b3a` cfg80211: allow connect keys only with default (TX) key
- `5c397a78da8a` tty: fix out-of-bounds access in tty_driver_lookup_tty()
- `144f0127e335` ALSA: control: code refactoring for ELEM_READ/ELEM_WRITE operations
- `1248384e3902` ALSA: control: Fix memory corruption risk in snd_ctl_elem_read
- `52d843097a43` ALSA: control: use counting semaphore as write lock for ELEM_WRITE operation
- `b7f1dc1571dc` scsi: core: Add BLIST_SKIP_VPD_PAGES for SKhynix H28U74301AMR
- `184e00822661` dm stats: check for and propagate alloc_percpu failure
- `e6c1ace92853` dm crypt: add cond_resched() to dmcrypt_write()
- `c19e642490e8` sched_getaffinity: don't assume 'cpumask_size()' is fully initialized
- `a9eb033f2a92` ALSA: usb-audio: Fix regression on detection of Roland VS-100
- `086ba7083de5` ext4: fix kernel BUG in 'ext4_write_inline_data_end()'
- `6065e599045a` icmp: guard against too small mtu
- `5639b9e625cc` ipv6: Fix an uninit variable access bug in __ip6_make_skb()
- `a4d67276db05` perf/core: Fix the same task check in perf_event_set_output
- `c2efa6b49cd8` ring-buffer: Fix race while reader and writer are on the same page
- `419e98bceb8b` cgroup/cpuset: Wake up cpuset_attach_wq tasks in cpuset_cancel_attach()
- `4c050ed8b326` scsi: core: Improve scsi_vpd_inquiry() checks
- `4fef5b937916` udp: Call inet6_destroy_sock() in setsockopt(IPV6_ADDRFORM).
- `2e499c569858` coresight-etm4: Fix for() loop drvdata->nr_addr_cmp range bug
- `e725bd5800f4` Revert "ext4: fix use-after-free in ext4_xattr_set_entry"
- `0f3209283ad4` ext4: remove duplicate definition of ext4_xattr_ibody_inline_set()
- `9345a7b3657c` ext4: fix use-after-free in ext4_xattr_set_entry
- `824b640de960` USB: core: remove device lock left behind by mismerge
- `254442105d9d` ring-buffer: Sync IRQ works before buffer destruction
- `85468a44d640` arm64: kgdb: Set PSTATE.SS to 1 to re-enable single-step
- `1aaca6730c10` net/packet: convert po->origdev to an atomic flag
- `22126501cc60` net/packet: convert po->auxdata to an atomic flag
- `9b7ad0726e2e` ipv4: Fix potential uninit variable access bug in __ip_make_skb()
- `3322045fd92b` perf/core: Fix hardlockup failure caused by perf throttle
- `3e012ae67cf3` clk: add missing of_node_put() in "assigned-clocks" property parsing
- `dc1e299cce0a` dm ioctl: fix nested locking in table_clear() to remove deadlock concern
- `fc347f3dc858` sit: update dev->needed_headroom in ipip6_tunnel_bind_dev()
- `6b21a35b9adb` writeback: fix call of incorrect macro
- `c60a63315034` af_packet: Don't send zero-byte data in packet_sendmsg_spkt().
- `4fd95b1cfdfe` ext4: avoid a potential slab-out-of-bounds in ext4_group_desc_csum
- `801ba552e954` ext4: improve error recovery code paths in __ext4_remount()
- `c2ecc6e73e64` ext4: bail out of ext4_xattr_ibody_get() fails for any reason
- `22a0589c40d7` ext4: remove a BUG_ON in ext4_mb_release_group_pa()
- `094fb49a2d0d` tty: Prevent writing chars during tcsetattr TCSADRAIN/FLUSH
- `cd55612b0c74` net: annotate sk->sk_err write from do_recvmmsg()
- `0e03069711ba` af_unix: Fix a data race of sk->sk_receive_queue->qlen.
- `504f789acc97` regmap: cache: Return error in cache sync operations for REGCACHE_NONE
- `290dd6be1ae8` ext2: Check block size validity during mount
- `bc0e034c90d7` lib: cpu_rmap: Avoid use after free on rmap->obj array entries
- `3882e90d425f` ext4: set goal start correctly in ext4_mb_normalize_request
- `9abf86f54834` ext4: Fix best extent lstart adjustment logic in ext4_mb_new_inode_pa()
- `d3bbf6c1e43c` Input: xpad - add constants for GIP interface numbers
- `2b9d3e54bee1` af_key: Reject optional tunnel/BEET mode templates in outbound policies
- `81805cde47e4` statfs: enforce statfs[64] structure initialization
- `c3ac22e75689` ext4: add bounds checking in get_max_inline_xattr_value_size()
- `865113f152bd` netlink: annotate accesses to nlk->cb_running
- `c71ef5c765ed` usb-storage: fix deadlock when a scsi command timeouts more than once
- `ca31acf449fc` ipv6: Fix out-of-bounds access in ipv6_find_tlv()
- `91c18ce8c30b` af_packet: Fix data-races of pkt_sk(sk)->num.
- `c4fd3c7ad0fe` af_packet: do not use READ_ONCE() in packet_bind()
- `e739fec678a3` tcp: Return user_mss for TCP_MAXSEG in CLOSE/LISTEN state if user_mss set
- `cc76da919aa5` udp6: Fix race condition in udp6_sendmsg & connect
- `935a7854d1d6` fbdev: modedb: Add 1920x1080 at 60 Hz video mode
- `ddc59c7572d8` scsi: core: Decrease scsi_device's iorequest_cnt if dispatch failed
- `31afa8bf6819` netfilter: conntrack: define variables exp_nat_nla_policy and any_addr with CONFIG_NF_NAT
- `1e39831dc5a5` HID: wacom: avoid integer overflow in wacom_intuos_inout()
- `2c5cd6c40743` rfs: annotate lockless accesses to RFS sock flow table
- `f8dad51199d0` lib: cpu_rmap: Fix potential use-after-free in irq_cpu_rmap_release()
- `c5d1eaa478bd` Input: xpad - delete a Razer DeathAdder mouse VID/PID entry
- `778aba138020` power: supply: Ratelimit no data debug output
- `b7aede14353c` regulator: Fix error checking for debugfs_create_dir
- `79cd7ad46773` power: supply: Fix logic checking if system is running from battery
- `88c0a72d107c` USB: core: Add routines for endpoint checks in old drivers
- `45de3288df73` usb: gadget: f_fs: Add unbind event before functionfs_unbind
- `b8c4dee7fc8f` usb: gadget: f_ncm: Add OS descriptor support
- `cbb3da2604b7` usb: gadget: f_ncm: Fix NTP-32 support
- `80ab09081ecc` netlink: do not hard code device address lenth in fdb dumps
- `aa94853ce1f6` lib/ts_bm: reset initial match offset for every block of text
- `643313a86b8c` netfilter: nf_conntrack_sip: fix the ct_sip_parse_numerical_param() return value.
- `19a807e22d9b` netlink: Add __sock_i_ino() for __netlink_diag_dump().
- `936bfa8f21b1` modpost: fix section mismatch message for R_ARM_ABS32
- `418c7fdbd52d` modpost: fix section mismatch message for R_ARM_{PC24,CALL,JUMP24}
- `19a80ba643e3` modpost: fix off by one in is_executable_section()
- `cd0612194e32` integrity: Fix possible multiple allocation in integrity_inode_get()
- `3f85be5e7cef` workqueue: clean up WORK_* constant types, clarify masking
- `b315a1b51d86` udp6: fix udp6_ehashfn() typo
- `4e56e28c606b` ipv6/addrconf: fix a potential refcount underflow for idev
- `b1f4fddb2a87` ext4: only update i_reserved_data_blocks on successful block allocation
- `e1e1022a56a4` ring-buffer: Fix deadloop issue on reading trace_pipe
- `b60e5676424b` fuse: revalidate: don't invalidate if interrupted
- `530deb7e9e66` tcp: annotate data-races around rskq_defer_accept
- `00467796173d` tcp: annotate data-races around fastopenq.max_qlen
- `9bf2d2bb3db7` ipv6 addrconf: fix bug where deleting a mngtmpaddr can create a new temporary address
- `1ba34289b9c5` ring-buffer: Fix wrong stat of cpu_buffer->read
- `86dd505a85a6` net/sched: cls_fw: Fix improper refcount update leads to use-after-free
- `6d822296832d` perf: Fix function pointer case
- `8e0a49dbe5df` net: add missing data-race annotation for sk_ll_usec
- `30a3f6ab9a15` tcp_metrics: fix addr_same() helper
- `fa7b9452bda7` tcp_metrics: annotate data-races around tm->tcpm_lock
- `f84e495e89cd` tcp_metrics: annotate data-races around tm->tcpm_net
- `22e5b4d07461` tcp_metrics: fix data-race in tcpm_suck_dst() vs fastopen
- `daaa721814d2` USB: zaurus: Add ID for A-300/B-500/C-700
- `991f263c6951` net: usbnet: Fix WARNING in usbnet_start_xmit/usb_submit_urb
- `218d2f08d704` lib/mpi: Eliminate unused umul_ppmm definitions for MIPS
- `f5973308eb3a` net: xfrm: Fix xfrm_address_filter OOB read
- `51fc34d6e409` net: af_key: fix sadb_x_filter validation
- `e2a0c217a014` ip6_vti: fix slab-use-after-free in decode_session6
- `340c57d268b4` ip_vti: fix potential slab-use-after-free in decode_session6
- `813abdcb87be` xfrm: add NULL check in xfrm_update_ae_params
- `cff483fb31ad` ALSA: usb-audio: Add support for Mythware XA001AU capture and playback interfaces.
- `96f80fb774af` netfilter: set default timeout to 3 secs for sctp shutdown send and recv state
- `3ac35d01c826` af_unix: Fix null-ptr-deref in unix_stream_sendpage().
- `5ab2dc03ce9c` net: fix the RTO timer retransmitting skb every 1ms if linear option is enabled
- `8f3025234d45` net: xfrm: Amend XFRMA_SEC_CTX nla_policy structure
- `232958f9ac5f` posix-timers: Ensure timer ID search-loop limit is valid
- `8c1fd52234a0` tcp: annotate data-races around tp->notsent_lowat
- `e11876eead5c` net/sched: cls_u32: No longer copy tcf_result on update to avoid use-after-free
- `02ce84a3439c` tcp_metrics: annotate data-races around tm->tcpm_stamp
- `ed2aa896280a` tcp_metrics: annotate data-races around tm->tcpm_vals[]
- `867ad10f047c` ext2: Drop fragment support
- `fe182776b33c` ipv6: adjust ndisc_is_useropt() to also return true for PIO
- `fac0da5187eb` net/packet: annotate data-races around tp->status
- `e16796783a3b` quota: fix warning in dqgrab()
- `b204c8c41802` tracing: Fix memleak due to race between current_tracer and trace
- `9a90a88ac9c6` lib/clz_ctz.c: Fix __clzdi2() and __ctzdi2() for 32-bit kernels
- `e87d0bb7347a` rtnetlink: Reject negative ifindexes in RTM_NEWLINK
- `5f1347e4e992` 9p: virtio: make sure 'offs' is initialized in zc_request
- `857aefa2d216` fs/nls: make load_nls() take a const parameter
- `276db8df1dab` security: keys: perform capable check only on privileged operations
- `66e8006fd4b5` audit: fix possible soft lockup in __audit_inode_child()
- `bb868e8e9e95` USB: gadget: f_mass_storage: Fix unused variable warning
- `5e4b9d4078c4` scsi: core: Use 32-bit hostnum in scsi_host_lookup()
- `8e6b7e85b722` amba: bus: fix refcount leak
- `4d7af218c21c` HID: multitouch: Correct devm device reference for hidinput input_dev name
- `89554d276e9f` igmp: limit igmpv3_newpack() packet size to IP_MAX_MTU
- `023311531a6a` netfilter: xt_u32: validate user space input
- `ad14bd8357a2` netfilter: xt_sctp: validate the flag_info count
- `f3ae052ae597` procfs: block chmod on /proc/thread-self/comm
- `b123e6a8ac14` pstore/ram: Check start of empty przs during init
- `29151b0b343c` net: read sk->sk_family once in sk_mc_loop()
- `de65f95276f9` af_unix: Fix data-races around user->unix_inflight.
- `54f4ac580fe0` af_unix: Fix data-race around unix_tot_inflight.
- `95a6c8570090` af_unix: Fix data race around sk->sk_err.
- `74290dd01600` crypto: lib/mpi - avoid null pointer deref in mpi_cmp_ui()
- `7e2ddfc570d7` ext2: fix datatype of block number in ext2_xattr_set2()
- `c8ff559e3562` iio: core: Use min() instead of min_t() to make code more robust
- `fc85988d6607` kobject: Add sanity check for kset->kobj.ktype in kset_register()
- `d4e58a2006fc` attr: block mode changes of symlinks
- `0230f87638b3` net/sched: cls_fw: No longer copy tcf_result on update to avoid use-after-free
- `1890d9d7f151` regmap: rbtree: Use alloc_flags for memory allocations
- `d4dccc15764f` ipv4: fix null-deref in ipv4_link_failure
- `e09c1d16abe0` ring-buffer: Avoid softlockup in ring_buffer_resize()
- `8ac0f82dbe17` ext4: fix rec_len verify error
- `c720e1cec8df` regmap: rbtree: Fix wrong register marked as in-cache when creating new node
- `7a29b1125764` rtnetlink: Reject negative ifindexes in RTM_NEWLINK *
- `ee37c4ca92a9` workqueue: Override implicit ordered attribute in workqueue_apply_unbound_cpumask()
- `9acb0b068bd5` usb: musb: Modify the "HWVers" register address
- `8c2c00d6a48b` Input: xpad - add PXN V900 support
- `30b28e74db39` pinctrl: avoid unsafe code pattern in find_pinctrl()
- `a4cfa6bde1b2` regmap: fix NULL deref on lookup
- `e2524c5b7b44` xfrm: fix a data-race in xfrm_gen_index()
- `64e2b22786c9` tracing: relax trace_event_eval_update() execution with cond_resched()
- `60a30bceee9a` HID: holtek: fix slab-out-of-bounds Write in holtek_kbd_input_event
- `00f8bb41ccc6` wifi: cfg80211: avoid leaking stack data into trace
- `b67eac42cb5a` mmc: core: Capture correct oemid-bits for eMMC cards
- `a8117a5e6fb1` Revert "pinctrl: avoid unsafe code pattern in find_pinctrl()"
- `60b2e00632da` HID: logitech-hidpp: Fix kernel crash on receiver USB disconnect
- `83a11f92a2d3` usb: gadget: ncm: Handle decoding of multiple NTB's in unwrap call
- `184fb71d8137` perf: Disallow mis-matched inherited group reads
- `110faf32c2fa` Revert "usb: gadget: f_ncm: Add OS descriptor support"
- `7a70e8876019` perf/core: Fix potential NULL deref
- `afd1e26bac41` kobject: Fix slab-out-of-bounds in fill_kobj_path()
- `2cfb9074c41b` driver: platform: Add helper for safer setting of driver_override
- `4e7167a86b63` usb: storage: set 1.50 as the lower bcdDevice for older "Super Top" compatibility
- `cf10fd1d912d` tcp_metrics: do not create an entry from tcp_init_metrics()
- `e4da583b3328` thermal: core: prevent potential string overflow
- `b7e4a3b66fa4` ext4: move 'ix' sanity check to corrent position
- `5652880d51ad` dccp/tcp: Call security_inet_conn_request() after setting IPv6 addresses.
- `b5a556092a55` Revert "mmc: core: Capture correct oemid-bits for eMMC cards"
- `07d9cb4329cf` audit: don't take task_lock() in audit_exe_compare() code path
- `3cb85c981649` audit: don't WARN_ON_ONCE(!current->mm) in audit_exe_compare()
- `5219a8930cf4` ALSA: info: Fix potential deadlock at disconnection
- `24aadf1f96e5` ext4: apply umask if ACL support is disabled
- `c5628ab1c02c` ext4: correct offset of gdb backup in non meta_bg group to update_backups
- `9bc6be3b0a9b` ext4: correct return value of ext4_convert_meta_bg
- `b08590f386ee` ext4: remove gdb backup copy for meta bg in setup_new_flex_group_blocks
- `d88a12f20362` net: sched: fix race condition in qdisc_graft()
- `51339a67c546` ipv4: Correct/silence an endian warning in __ip_do_redirect
- `4f12fbb7396e` net: usb: ax88179_178a: fix failed operations during ax88179_reset
- `b3421d246265` pinctrl: avoid reload of p state in list iteration
- `60dadf03bd4e` tcp_metrics: properly set tp->snd_ssthresh in tcp_init_metrics()
- `ac30c08ffa7b` tcp: do not accept ACK of bytes we never sent
- `9d1eeb6694bf` ALSA: pcm: fix out-of-bounds in snd_pcm_state_names
- `5ffac381db74` tracing: Always update snapshot buffer size
- `ce33133de51c` packet: Move reference count in packet_sock to atomic_long_t
- `0bb59ff4da36` net: Remove acked SYN flag from packet in the transmit queue correctly
- `c6f52cdea0b5` cred: switch to using atomic_long_t
- `8886cdde0b08` HID: multitouch: Add quirk for HONOR GLO-GXXX touchpad
- `97471254a3b0` ext4: prevent the normalized size from exceeding EXT_MAX_BLOCKS
- `e72670a18126` USB: serial: ftdi_sio: update Actisense PIDs constant names
- `9d0cc098bfc5` net: 9p: avoid freeing uninit memory in p9pdu_vreadf
- `d755d7c697b8` mmc: core: Cancel delayed work before releasing host
- `7d40cbefe6fa` net: add a route cache full diagnostic message
- `5a8d351e5a49` tracing: Have large events show up as '[LINE TOO BIG]' instead of nothing
- `c5b6e73a1f0a` tracing: Add size check when printing trace_marker output
- `7392e89048dc` ring-buffer: Do not record in NMI if the arch does not support cmpxchg in NMI
- `7e54661b3db9` Input: xpad - add Razer Wolverine V2 support
- `15cbc45e86a1` pstore: ram_core: fix possible overflow in persistent_ram_init_ecc()
- `2a1d5c8380e8` ip6_tunnel: fix NEXTHDR_FRAGMENT handling in ip6_tnl_parse_tlv_enc_lim()
- `4c4c4a5284c0` tick-sched: Fix idle and iowait sleeptime accounting vs CPU hotplug
- `3bed4c2ecee0` usb: mon: Fix atomicity violation in mon_bin_vma_fault
- `8744b4fdd4c9` driver core: add device probe log helper
- `24a1889c64f8` crypto: api - Disallow identical driver names
- `b49a09cbc30b` driver code: print symbolic error code
- `029edfbeb560` drivers: core: fix kernel-doc markup for dev_err_probe()
- `417c4199f913` tick/sched: Preserve number of idle sleeps across CPU hotplug events
- `028da65cfc49` wifi: cfg80211: free beacon_ies when overridden from hidden BSS
- `5d424bc726e0` usb: hub: Replace hardcoded quirk value with BIT() macro
- `fc5ff1f06d3f` wifi: cfg80211: fix RCU dereference in __cfg80211_bss_update
- `044dd7eb7c0f` ipv6: Ensure natural alignment of const ipv6 loopback and router addresses
- `85df68b20ead` af_unix: fix lockdep positive in sk_diag_dump_icons()
- `2fceda46ad68` HID: apple: Swap the Fn and Left Control keys on Apple keyboards
- `bb8d857ec2d2` inet: read sk->sk_family once in inet_recv_error()
- `37a53d8baa59` ppp_async: limit MRU to 64K
- `c6db6ae566da` mm/writeback: fix possible divide-by-zero in wb_dirty_limits(), again
- `55ae6fb7c1a9` USB: hub: check for alternate port before enabling A_ALT_HNP_SUPPORT
- `052905e830d7` ext4: fix double-free of blocks due to wrong extents moved_len
- `5082e3a0e7ee` memcg: add refcnt for pcpu stock to avoid UAF problem in drain_all_stock()
- `1836ab0c9fc6` sched/rt: Fix sysctl_sched_rr_timeslice intial value
- `344471218850` wifi: cfg80211: fix missing interfaces when dumping
- `24f29b76aab9` ext4: avoid allocating blocks from corrupted group in ext4_mb_try_best_found()
- `d545ccbb16d4` ext4: avoid allocating blocks from corrupted group in ext4_mb_find_by_goal()
- `abc80c3cad53` usb: gadget: ncm: Avoid dropping datagrams of properly parsed NTBs
- `9efdb964ee1c` mm: fix unmap_mapping_range high bits shift bug
- `b3c60c0f37f2` ext4: unify the type of flexbg_size to unsigned int
- `34a75a8e42a6` virtio_net: Fix "‘%d’ directive writing between 1 and 11 bytes into a region of size 10" warnings
- `138526a3167b` netfilter: nf_log: replace BUG_ON by WARN_ON_ONCE when putting logger
- `9d5bb3ab4b1c` net: ipv4: fix a memleak in ip_setup_cork
- `a906faff6936` tracing/trigger: Fix to return error if failed to alloc snapshot
- `0769a39f41a1` packet: move from strlcpy with unused retval to strscpy
- `c6df29bb34c0` netlink: Fix kernel-infoleak-after-free in __skb_datagram_iter
- `3715d24caf8c` wifi: nl80211: reject iftype change with mesh ID change
- `002ee4711268` mmc: core: Fix eMMC initialization with 1-bit bus connection
- `50c5c92e77b7` getrusage: add the "signal_struct *sig" local variable
- `63365a01c890` getrusage: use __for_each_thread()
- `230a65643348` md: switch to ->check_events for media change notifications
- `fb3b08d2dcf9` block: add a new set_read_only method
- `6bd4e7adaa72` af_unix: Annotate data-race of gc_in_progress in wait_for_unix_gc().
- `8c8d9d2b34e6` ipv6: fib6_rules: flush route cache when rule is changed
- `7618413f127c` tcp: fix incorrect parameter validation in the do_tcp_getsockopt() function
- `2e062c12f434` l2tp: fix incorrect parameter validation in the pppol2tp_getsockopt() function
- `63c339f060a1` udp: fix incorrect parameter validation in the udp_lib_getsockopt() function
- `5b751acdbca0` dm: call the resume method on internal suspend
- `6c8c98b094f1` fs/quota: erase unused but set variable warning
- `58c4e75204bc` quota: simplify drop_dquot_ref()
- `3fbb9b7b8052` mfd: syscon: Call of_node_put() only when of_parse_phandle() takes a ref
- `587c5f0f4055` ALSA: usb-audio: Stop parsing channels bits when all channels are found.
- `3dee18ecb83d` fat: fix uninitialized field in nostale filehandles
- `a8a53d1a9767` ext4: fix corruption during on-line resize
- `665924521700` xfrm: Avoid clang fortify warning in copy_to_user_tmpl()
- `804a04c65180` usb: gadget: ncm: Fix handling of zero block length packets
- `bb326e9821f4` usb: port: Don't try to peer unused USB ports based on location
- `75fe0731c89b` Revert "loop: Check for overflow while configuring loop"
- `07bef12c0d8c` loop: Call loop_config_discard() only after new config is applied
- `b135d56fd7ad` netfilter: nf_conntrack_h323: Add protection for bmp length out of range
- `b3ad04c5a4af` getrusage: use sig->stats_lock rather than lock_task_sighand()
- `5a1c186e3c63` sock_diag: annotate data-races around sock_diag_handlers[family]
- `35bba80263c3` perf/core: Fix reentry problem in perf_output_read_group()
- `5c3a3dcaabc1` Revert "crypto: api - Disallow identical driver names"
- `e7b917a31112` USB: core: Fix deadlock in usb_deauthorize_interface()
- `645ed90aad17` ipv6: Fix infinite recursion in fib6_dump_done().
- `bb4dc029c9da` ASoC: ops: Fix wraparound for mask in snd_soc_get_volsw
- `91d0326797ae` fbmon: prevent division by zero in fb_videomode_from_videomode()
- `acca9d4b0fff` virtio: reenable config if freezing device failed
- `c853c986ec59` x86/mm/pat: fix VM_PAT handling in COW mappings
- `4d588e361f3c` ipv4/route: avoid unused-but-set-variable warning
- `648afb4faf34` ipv6: fix race condition between ipv6_get_ifaddr and ipv6_del_addr
- `50a3068f7717` tracing: hide unused ftrace_event_id_fops
- `f3ae929f687a` fs: sysfs: Fix reference leak in sysfs_break_active_protection()
- `246405668388` net: usb: ax88179_178a: stop lying about skb->truesize
- `e4a9d1868aa5` Revert "tracing/trigger: Fix to return error if failed to alloc snapshot"
- `87478afa3b56` tun: limit printing rate when illegal packet received by tun dev
- `c7a2e13b1fb1` wifi: nl80211: don't free NULL coalescing rule
- `87410552f9f1` net: mark racy access on sk->sk_rcvbuf
- `51fdba566ffe` af_unix: Do not use atomic ops for unix_sk(sk)->inflight.
- `586aecad504f` af_unix: Fix garbage collector racing against connect()
- `a0201e4e0b06` usb: gadget: composite: fix OS descriptors w_value logic
- `52762aae0167` dyndbg: fix old BUG_ON in >control parser
- `fca035967818` af_unix: Suppress false-positive lockdep splat for spin_lock() in __unix_gc().
- `dfa8036705fa` ring-buffer: Fix a race between readers and resize checks
- `6caa3826c8fb` wifi: cfg80211: fix the order of arguments for trace events of the tx_rx_evt class
- `9c730b3586f0` new helper: memdup_user_nul()
- `a45a095e51ea` ext4: avoid excessive credit estimate in ext4_tmpfile()
- `abcda5d16f56` ipv6: fib6_rules: avoid possible NULL dereference in fib6_rule_action()
- `06d93568398a` params: lift param_set_uint_minmax to common code
- `af7e57e43db7` netfilter: nfnetlink_queue: acquire rcu_read_lock() in instance_destroy_rcu()
- `da4d665c6221` spi: Don't mark message DMA mapped when no transfer in it is
- `c9fb2e84ea6a` net/9p: fix uninit-value in p9_client_rpc()
- `3473817258fa` tcp: count CLOSE-WAIT sockets for TCP_MIB_CURRESTAB
- `979826a60928` af_unix: Annotate data-race of sk->sk_state in unix_inq_len().
- `d53da08e5e33` af_unix: Annotate data-races around sk->sk_state in sendmsg() and recvmsg().
- `2ac1828a9c69` af_unix: Annotate data-race of net->unx.sysctl_max_dgram_qlen.
- `3e81dbc5a4e4` usb: gadget: f_fs: Fix race between aio_cancel() and AIO request complete
- `7637be7c4033` Input: try trimming too long modalias strings
- `09a0d00e2010` tcp: fix race in tcp_v6_syn_recv_sock()
- `e863732b64f7` ipv6/route: Add a missing check on proc_dointvec
- `1e68639c34b4` net/ipv6: Fix the RT cache flush via sysctl using a previous delay
- `3aab177a1bec` drivers: core: synchronize really_probe() and dev_uevent()
- `b6e894e8ebf7` xfrm6: check ip6_dst_idev() return value in xfrm6_get_saddr()
- `000256bb2ec9` virtio_net: checksum offloading handling fix
- `90f253eddf6e` regulator: core: Fix modpost error "regulator_get_regmap" undefined
- `4a51a2ca90eb` pinctrl: fix deadlock in create_pinctrl() when handling -EPROBE_DEFER
- `f80ed0d310a9` net: usb: ax88179_178a: improve link status logs
- `ed7daeb3df73` Input: ff-core - prefer struct_size over open coded arithmetic
- `b8617719d306` tcp_metrics: validate source addr length
- `9b6e6ce15514` inet_diag: Initialize pad field in struct inet_diag_req_v2
- `7ed769cc0270` mm: optimize the redundant loop of mm_update_owner_next()
- `087c0e236cfc` Revert "mm/writeback: fix possible divide-by-zero in wb_dirty_limits(), again"
- `c10a3b631da6` tcp: fix incorrect undo caused by DSACK of TLP retransmit
- `7a6b9c818d25` ppp: reject claimed-as-LCP but actually malformed packets
- `635be4297250` usb: gadget: configfs: Prevent OOB read/write in usb_string_copy()
- `c9b70ed4ae1f` USB: core: Fix duplicate endpoint bug by clearing reserved bits in the descriptor
- `de86b5cd4732` kconfig: remove wrong expr_trans_bool()
- `3f4641034c5b` wifi: cfg80211: wext: add extra SIOCSIWSCAN data check
- `f45b899adc6b` filelock: Remove locks reliably when fcntl/close race is detected
- `c7ec2b3b2888` net: relax socket state check at accept time.
- `6ebd8367c352` filelock: Fix fcntl/close race recovery compat path
- `dff699deb339` HID: core: remove unnecessary WARN_ON() in implement()
- `9f3440427ed7` ftruncate: pass a signed offset
- `da3cfe9f5833` ipv6: annotate some data-races around sk->sk_prot
- `5c12cc6581e3` ipv6: Fix data races around sk->sk_prot.
- `3f54a6a7702d` tcp: Fix data races around icsk->icsk_af_ops.
- `47c75e1b7073` UPSTREAM: tcp: fix DSACK undo in fast recovery to call tcp_try_to_open()
- `c921dfb5c279` fs/file: fix the check in find_next_fd()
- `a6ae5ec7dfcc` fs: better handle deep ancestor chains in is_subdir()
- `bf9da8f60610` perf: Fix perf_aux_size() for greater-than 32-bit size
- `f09027650c74` perf: Prevent passing zero nr_pages to rb_alloc_aux()
- `0b0ff680bfcb` netfilter: ctnetlink: use helper function to calculate expect ID
- `eb9fa1f48177` tick/broadcast: Make takeover of broadcast hrtimer reliable
- `288371badca6` ext4: check dot and dotdot of dx_root before making dir indexed
- `bec2d0c31b2d` decompress_bunzip2: fix rare decompression failure
- `2a03b7bd989c` net: ip_rt_get_source() - use new style struct initializer instead of memset
- `6edb7d913ab6` ipv4: Fix incorrect source address in Record Route option
- `311ee7400297` mm: avoid overflows in dirty throttling logic
- `40f3e5b0f39c` protect the fetch of ->fd[fd] in do_dup2() from mispredictions
- `9c929c00d88f` ALSA: usb-audio: Correct surround channels in UAC1 channel map
- `90dd1dbfca95` net: linkwatch: use system_unbound_wq
- `e3e4c9f6347a` wifi: nl80211: don't give key data to userspace
- `82593ee6411f` jbd2: avoid memleak in jbd2_journal_write_metadata_buffer
- `684276624fab` ext4: fix wrong unit use in ext4_mb_find_by_goal
- `7882bc96c696` tick/broadcast: Move per CPU pointer access into the atomic section
- `056748bde23d` ntp: Clamp maxerror and esterror to operating range
- `d9e51e117d18` ntp: Safeguard against time_constant overflow
- `5f00eb3fc53d` serial: core: check uartclk for zero to avoid divide by zero
- `dc94c7ed18b0` exec: Fix ToCToU between perm check and set-uid/gid usage
- `532d7abd79b2` ALSA: usb-audio: Support Yamaha P-125 quirk entry
- `ebe498752622` dm resume: don't return EINVAL when signalled
- `dea95807c6c9` selinux: fix potential counting error in avc_add_xperms_decision()
- `0365522dbeb4` memcg_write_event_control(): fix a user-triggerable oops
- `995fde777693` ipv6: prevent UAF in ip6_send_skb()
- `9dc44573ef38` Input: MT - limit max slots
- `7dada25c2db1` filelock: Correct the filelock owner in fcntl_setlk/fcntl_setlk64
- `8a9a79c57385` fbmem: Check virtual screen sizes in fb_set_var()
- `a020210ca5b0` usb: core: sysfs: Unmerge @usb3_hardware_lpm_attr_group in remove_power_attributes()
- `4123adb729c1` af_unix: Remove put_pid()/put_cred() in copy_peercred().
- `e5dbc4fc4849` drivers/net/usb: Remove all strcpy() uses
- `02869ddf313f` rfkill: fix spelling mistake contidion to condition
- `b245fb8e7100` devres: Initialize an uninitialized struct member
- `36428b164517` iio: fix scale application in iio_convert_raw_to_processed_unlocked
- `545220d990b0` ring-buffer: Rename ring_buffer_read() to read_buffer_iter_advance()
- `183077d517b0` tracing: Avoid possible softlockup in tracing_iter_reset()
- `1f43ccdf08b6` fuse: Initialize beyond-EOF page contents before setting uptodate
- `2d0dbd414270` dm suspend: return -ERESTARTSYS instead of -EINTR
- `f74557da4d93` cgroup: Protect css->cgroup write under css_set_lock
- `81da31d1a088` of/irq: Prevent device address out-of-bounds read in interrupt map walk
- `e19c8dfdf7f6` rtmutex: Drop rt_mutex::wait_lock before scheduling
- `7c4bd759d48d` fbcon: Prevent that screen size is smaller than font size
- `1183a9208e86` USB: serial: pl2303: add device id for Macrosilicon MS3020
- `d64872b569d8` ext4: clear EXT4_GROUP_INFO_WAS_TRIMMED_BIT even mount with discard
- `152f9754e553` ext4: avoid negative min_clusters in find_group_orlov()
- `987eaa8dcbeb` ext4: return error on ext4_find_inline_entry
- `4e6894550cc8` crypto: aead,cipher - zeroize key buffer after use
- `e251cc1334d8` vfs: fix race between evice_inodes() and find_inode()&iput()
- `771fade6d194` netfilter: nf_tables: prevent nf_skb_duplicated corruption
- `a9329d6176bb` net: add more sanity checks to qdisc_pkt_len_init()
- `d2247e02dcb0` ipv4: Check !in_dev earlier for ioctl(SIOCSIFADDR).
- `6eabaa8a105c` ipv4: Mask upper DSCP bits and ECN bits in NETLINK_FIB_LOOKUP family
- `0052e2c4750a` signal: Replace BUG_ON()s
- `7118d4505146` of/irq: Refer to actual buffer size in of_irq_parse_one()
- `cbd5b19b7227` ext4: ext4_search_dir should return a proper error
- `e77c86177ad2` perf/core: Fix small negative period being ignored
- `6fca66525234` ALSA: core: add isascii() check to card ID generator
- `061218255e07` ext4: no need to continue when the number of entries is 1
- `e0e536b77487` ext4: propagate errors from ext4_find_extent() in ext4_insert_range()
- `13a4f1660017` ext4: fix incorrect tid assumption in __jbd2_log_wait_for_space()
- `300f7ca09bfa` ext4: aovid use-after-free in ext4_ext_insert_extent()
- `cf1d04c92bd6` ext4: fix double brelse() the buffer of the extents path
- `a004fce75ca7` ext4: fix incorrect tid assumption in ext4_wait_for_tail_page_commit()
- `e490634cf52c` jbd2: stop waiting for space when jbd2_cleanup_journal_tail() returns error
- `1d0b8e286a84` ext4: fix inode tree inconsistency caused by ENOMEM
- `13af051b8d7d` tracing: Remove precision vsnprintf() check from print event
- `2e20599622af` driver core: bus: Return -EIO instead of 0 when show/store invalid bus attribute
- `b06a74a22bd1` ppp: fix ppp_async_encode() illegal access
- `4d1afd263888` HID: plantronics: Workaround for an unexcepted opposite volume key
- `f02971f0aa16` usb: storage: ignore bogus device raised by JieLi BR21 USB sound chip
- `76b30b4d941d` net: Fix an unsafe loop on the list
- `40ec979c6d01` posix-clock: Fix missing timespec64 check in pc_clock_settime()
- `1117fdfc66a6` fat: fix uninitialized variable
- `ae0be6f84897` xhci: Fix incorrect stream context type macro
- `5dbf6ab080c7` net: usb: usbnet: fix name regression
- `562c33e0ce88` posix-clock: posix-clock: Fix unbalanced locking in pc_clock_settime()
- `6681620a3692` xfrm: validate new SA's prefixlen using SA family when sel.family is unset
- `5e8c5ca9cca2` net/sched: stop qdisc_tree_reduce_backlog on TC_H_ROOT
- `7d327c7465b8` usb: phy: Fix API devm_usb_put_phy() can not release the phy
- `65dc79e55730` gpio: prevent potential speculation leaks in gpio_device_get_desc()
- `0994d75c1648` kthread: add kthread_work tracepoints
- `89278fdddd51` kthread: fix task state in kthread worker if being frozen
- `6832f76bf7d4` ext4: avoid OOB when system.data xattr changes underneath the filesystem
- `58779420cf48` netfilter: nf_reject_ipv6: fix nf_reject_ip6_tcphdr_put()
- `43c8bfae4c2a` fs: Fix file_set_fowner LSM hook inconsistencies
- `bfe7a9a5b48c` ext4: fix i_data_sem unlock order in ext4_ind_migrate()
- `e01afdabeaff` ext4: avoid ext4_error()'s caused by ENOMEM in the truncate path
- `e6bc59e0c89e` ext4: fix slab-use-after-free in ext4_split_extent_at()
- `bec0bfe9b401` ext4: update orig_path in ext4_find_extent()
- `0544747431c0` tcp: fix tcp_enter_recovery() to zero retrans_stamp when it's safe
- `56c693c3da94` net: ipv6: ensure we call ipv6_mc_down() at most once
- `f39491c638d2` CDC-NCM: avoid overflow in sanity checking
- `514f5ac9c166` selinux: improve error checking in sel_write_load()
- `d9db27345278` printk: Export is_console_locked
- `bab4b117f740` HID: core: zero-initialize the report buffer
- `35ecfdcc8fa2` security/keys: fix slab-out-of-bounds in key_task_permission
- `d5a7f90b783f` irqchip/gic-v3: Force propagation of the active state with a read-back
- `c5fa05319f34` HID: multitouch: Add quirk for HONOR MagicBook Art 14 touchpad
- `bc3ea9e04fe5` netlink: terminate outstanding dump on socket close
- `5f49ceb065de` initramfs: avoid filename buffer overrun
- `f931a9bc6831` time: Fix references to _msecs_to_jiffies() handling of values
- `25e540fa23e2` power: supply: core: Remove might_sleep() from power_supply_put()
- `05d93bf127b8` ext4: supress data-race warnings in ext4_free_inodes_{count,set}()
- `620203e7447a` tty: ldsic: fix tty_ldisc_autoload sysctl's proc_handler
- `e3de0e90daf1` ALSA: usb-audio: Fix potential out-of-bound accesses for Extigy and Mbox devices
- `890bff91fb81` Revert "usb: gadget: composite: fix OS descriptors w_value logic"
- `e2ef3d732148` lib: string_helpers: silence snprintf() output truncation warning
- `c5d39584d417` rtc: check if __rtc_read_time was successful in rtc_timer_do_work()
- `293429fa93c7` modpost: remove incorrect code in do_eisa_entry()
- `809fa31f6e67` regmap: detach regmap from dev on regmap_exit
- `41cb9e0aebe8` tracing: Use atomic64_inc_return() in trace_clock_counter()
- `ce3dfebd5e1c` modpost: Add .irqentry.text to OTHER_SECTIONS
- `77afebcfd837` usb: gadget: u_serial: Fix the issue that gs_start_io crashed due to accessing null pointer
- `d046f846b2aa` ALSA: usb-audio: Fix a DMA to stack memory bug
- `7c2a92a8907c` ALSA: usb: Fix UBSAN warning in parse_audio_unit()
- `df629073bb41` of/irq: Fix using uninitialized variable @addr_len in API of_irq_parse_one()
- `3af2afae689b` scsi: qla1280: Fix hw revision numbering for ISP1020/1040
- `b34afcbfd289` regmap: Use correct format specifier for logging range errors
- `11b57e938435` selinux: ignore unknown extended permissions
- `6dbd0b698aa1` irqchip/gic: Correct declaration of *percpu_base pointer in union gic_base
- `4f6bb8c8012b` modpost: fix input MODULE_DEVICE_TABLE() built for 64-bit on 32-bit host
- `163529197936` modpost: fix the missed iteration for the max bit in do_input()
- `9f3769378767` af_packet: avoid erroring out after sock_init_data() in packet_create()
- `20e3dfc5e631` net: inet: do not leave a dangling sk pointer in inet_create()
- `b317a37e42db` leds: class: Protect brightness_show() with led_cdev->led_access mutex
- `b5ed2cbe9247` posix-clock: Fix backported timespec64 check
- `cf634985fd09` jbd2: flush filesystem device before updating tail sequence
- `a7f44c6fba21` usb-storage: Add max sectors quirk for Nokia 208
- `3631ee65e928` usb: gadget: u_serial: Disable ep before setting port to null to fix the crash caused by port being null
- `20c7c96ad61a` usb: fix reference leak in usb_new_device()
- `907108515f42` usb: gadget: f_fs: Remove WARN_ON in functionfs_bind
- `ab05016e8fa8` iio: inkern: call iio_device_put() only on mapped devices
- `a4b7a2038a31` ipv6: avoid possible NULL deref in rt6_uncached_list_flush_dev()
- `c2284db1b8bb` Revert "usb: gadget: u_serial: Disable ep before setting port to null to fix the crash caused by port being null"
- `5d0eb8cae999` Input: xpad - add unofficial Xbox 360 wireless receiver clone
- `01221cd0dc76` Input: xpad - add support for wooting two he (arm)
- `57211c31a9ac` irqchip/gic-v3: Handle CPU_PM_ENTER_FAILED correctly
- `8280cb087245` regulator: of: Implement the unwind path of of_regulator_match()
- `cf8545e83203` genksyms: fix memory leak when the same symbol is read from *.symref file
- `e87972c642a2` printk: Fix signed integer overflow when defining LOG_BUF_LEN_MAX
- `08e6f6fde9d4` tun: fix group permission check
- `e1ebf3ea7568` tun: revert fix group permission check
- `f6e8edca50bf` of: Fix of_find_node_opts_by_path() handling of alias+path+options
- `0ea652d698a2` HID: multitouch: Add NULL check in mt_input_configured
- `92a2b0786e0b` arm64: cacheinfo: Avoid out-of-bounds write to cacheinfo array
- `9c1e2a27b646` USB: quirks: add USB_QUIRK_NO_LPM quirk for Teclast dist
- `620f2b19a1c7` usb: gadget: f_midi: fix MIDI Streaming descriptor lengths
- `27a600fa398c` USB: hub: Ignore non-compliant devices with too many configs or interfaces
- `194c12908844` USB: cdc-acm: Fill in Renesas R-Car D3 USB Download mode quirk
- `30e33d1e1f10` ipv4: use RCU protection in rt_is_expired()
- `447bae2a5e71` ipv6: use RCU protection in ip6_default_advmss()
- `17d865a55d92` ndisc: use RCU protection in ndisc_alloc_skb()
- `48fb19bf4e3a` neighbour: delete redundant judgment statements
- `04f42759b523` neighbour: use RCU protection in __neigh_notify()
- `b8b8d5248881` arp: use RCU protection in arp_xmit()
- `e30fe7c9691e` ndisc: extend RCU protection in ndisc_send_skb()
- `650c90dc9817` driver core: bus: Fix double free in driver API bus_register()
- `659b72f8fcb0` crypto: testmgr - Fix wrong test case of RSA
- `c5f8ed2bfda3` crypto: testmgr - fix version number of RSA tests
- `eeb7c6cc7e85` crypto: testmgr - populate RSA CRT parameters in RSA test vectors
- `fbe63b5ce89b` ftrace: Avoid potential division by zero in function_stat_show()
- `43b2ba5fdcd5` sched/core: Prevent rescheduling when interrupts are disabled
- `ba236908c5fd` wifi: cfg80211: regulatory: improve invalid hints checking
- `25025cc57729` net-timestamp: support TCP GSO case for a few missing flags
- `0d5cf0795dcc` usb: quirks: Add DELAY_INIT and NO_LPM for Prolific Mass Storage Card Reader
- `7317917c56cb` usb: gadget: Set self-powered based on MaxPower and bmAttributes
- `134d2fa5308b` usb: gadget: Fix setting self-powered state on suspend
- `3ffe068820fb` usb: gadget: Check bmAttributes only if configuration is valid
- `e4b9f1f00a8b` ipv4: use RCU protection in inet_select_addr()
- `8466b160abf4` pfifo_tail_enqueue: Drop new packet when sch->limit == 0
- `a80e9f6f4ff3` wifi: nl80211: reject cooked mode if it is set along with other flags
- `0580c4343e5a` block: fix 'kmem_cache of name 'bio-108' already exists'
- `fccdf4e26bbc` net/neighbor: add missing policy for NDTPA_QUEUE_LENBYTES
- `192840a31c02` ALSA: usb-audio: Add quirk for Plantronics headsets to fix control names
- `abca68f88b51` net: usb: usbnet: restore usb%d name exception for local mac addresses
- `f81fc5440529` fs/procfs: fix the comment above proc_pid_wchan()
- `09ca219f4d94` ring-buffer: Fix bytes_dropped calculation issue
- `3c39f3fd548f` ipv6: fix omitted netlink attributes when using RTEXT_FILTER_SKIP_STATS
- `2187045a82f5` tracing: Fix use-after-free in print_graph_function_flags during tracer switching
- `07c07e318f1b` net_sched: Prevent creation of classes with TC_H_ROOT
- `e9537d5c565c` net: add dev_net_rcu() helper *
- `f8c82e996676` HID: ignore non-functional sensor in HP 5MP Camera
- `0d496ff5ef3a` perf: arm_pmu: Don't disable counter in armpmu_add()
- `0dc9296da1a0` ALSA: usb-audio: Fix CME quirk for UF series keyboards
- `531b12deaf02` ext4: protect ext4_release_dquot against freezing
- `061b94ca0f99` ext4: fix off-by-one error in do_split
- `642e5af02fb5` jbd2: remove wrong sb->s_sequence check
- `048335547bcf` ftrace: Add cond_resched() to ftrace_graph_set_hash()
- `a9bd51f12238` of/irq: Fix device node refcount leakage in API irq_of_parse_and_map()
- `ce4d47e20777` net: openvswitch: fix race on port output
- `8c50f2ea31f3` ext4: optimize __ext4_check_dir_entry()
- `d93343de3fb7` ext4: fix OOB read when checking dotdot dir
- `4f62bd997682` usb: quirks: Add delay init quirk for SanDisk 3.2Gen1 Flash Drive
- `19e1352bbc75` USB: VLI disk crashes if LPM is used
- `eab1f9df6808` clk: check for disabled clock-provider in of_clk_get_hw_from_clkspec()
- `7f406a4c1fd7` mm: add missing release barrier on PGDAT_RECLAIM_LOCKED unlock
- `74915cd4a95a` of/irq: Fix device node refcount leakages in of_irq_init()
- `2c41eafcd800` tracing: Fix oob write in trace_seq_to_buffer()
- `1651cff98951` module: ensure that kobject_put() is safe for module type kobjects
- `99af1b1aab9f` do_umount(): add missing barrier before refcount checks in sync case
- `f1a36e6c8dd2` dql: Fix dql->limit value when reset.
- `105589c1e6c1` ext4: reorder capability check last
- `04485e3e49db` dm: restrict dm device size to 2^63-512 bytes
- `c31727908d63` posix-timers: Add cond_resched() to posix_timer_add() search loop
- `82c38140e3d0` ASoC: ops: Enforce platform maximum on initial value
- `cdfa8d7edc5e` pinctrl: devicetree: do not goto err when probing hogs in pinctrl_dt_to_map
- `a0ec89aca20a` ip: fib_rules: Fetch net from fib_rule in fib[46]_rule_configure().
- `fa086e850fff` scsi: st: Restore some drive settings after reset
- `c7789471c12a` __legitimize_mnt(): check for MNT_SYNC_UMOUNT should be under mount_lock
- `6b82cdf0f9b4` coredump: fix error handling for replace_fd()
- `874b3eee3cd9` tracing: Fix compilation warning on arm32
- `b90fdf5490c8` usb: quirks: Add NO_LPM quirk for SanDisk Extreme 55AE
- `2bb66a6b9b3a` usb: storage: Ignore UAS driver for SanDisk 3.2 Gen2 storage device
- `e176fe5b5412` fbdev: core: fbcvt: avoid division by 0 in fb_cvt_hperiod()
- `1feae3766893` do_change_type(): refuse to operate on unmounted/not ours mounts
- `80a2189f7a4e` fs/filesystems: Fix potential unsigned integer underflow in fs_name()
- `79cd322488f2` usb: Flush altsetting 0 endpoints before reinitializating them after reset.
- `d95ca408dbfe` ext4: inline: fix len overflow in ext4_prepare_inline_data
- `aad1e5b90ecb` ext4: fix calculation of credits for extent tree modification
- `4c3fcf9cca77` mm: fix ratelimit_pages update error in dirty_ratio_handler()
- `1acd7a7bff49` cpufreq: Force sync policy boost with global boost on sysfs update
- `9c7ad1c3e4da` ipv4/route: Use this_cpu_inc() for stats on PREEMPT_RT
- `7c9f9dc54d18` sock: Correct error checking condition for (assign|release)_proto_idx()
- `580b44fc8d8a` PM / suspend: Export pm_suspend_target_state
- `853c317a1694` HID: usbhid: Eliminate recurrent out-of-bounds bug in usbhid_parse()
- `1088981f3e49` posix-cpu-timers: fix race between handle_posix_cpu_timers() and posix_cpu_timer_del()
- `9d7cf725069c` scsi: core: Remove the /proc/scsi/${proc_name} directory earlier
- `a10f482f2b8c` usb: Add checks for snprintf() calls in usb_alloc_dev()
- `5a51dde2fdda` HID: wacom: fix kobject reference count leak
- `ddd77c7fce09` netlink: Fix wraparounds of sk->sk_rmem_alloc.
- `a5297d91f638` usb: gadget: u_serial: Fix race condition in TTY wakeup
- `90bea103f26c` netlink: make sure we allow at least one dump skb
- `13ba42ef7e9f` netlink: Fix rmem check in netlink_broadcast_deliver().
- `eef38bd172b8` Input: xpad - add support for Amazon Game Controller
- `f6520b25f9e2` Input: xpad - add VID for Turtle Beach controllers
- `698b9c24e3dd` net: ipv6: Discard next-hop MTU less than minimum link MTU
- `394ef56ad2d6` USB: serial: ftdi_sio: add support for NDI EMGUIDE GEMINI
- `24874aa9b5ba` usb: gadget: configfs: Fix OOB read on empty string write
- `a532558d9226` Input: xpad - set correct controller type for Acer NGR200
- `170243e1d294` HID: core: ensure the allocated report buffer can contain the reserved report ID
- `903e2e7466e4` HID: core: ensure __hid_request reserves the report ID as the first byte
- `58a6b69855e3` HID: core: do not bypass hid_hw_raw_request
- `43d674eec2be` net/sched: Return NULL when htb_lookup_leaf encounters an empty rbtree
- `68d0becd5d85` usb: musb: fix gadget state on disconnect
- `55140cc149e1` ASoC: ops: dynamically allocate struct snd_ctl_elem_value
- `ac416348b3f3` tcp: fix tcp_ofo_queue() to avoid including too much DUP SACK range
- `ec92023f3e0e` pptp: ensure minimal skb length in pptp_xmit()
- `4e7c2fb482ef` ipv6: reject malicious packets in ipv6_gso_segment()
- `78b50b96fb9c` pptp: fix pptp_xmit() error path
- `6d3b4f4a4598` perf/core: Don't leak AUX buffer refcount on allocation failure
- `b74bf49820ad` perf/core: Exit early on perf_mmap() fail
- `5d55d85fb82d` netlink: avoid infinite retry looping in netlink_unicast()
- `ab255ec2d8ef` ASoC: soc-dapm: set bias_level if snd_soc_dapm_set_bias_level() was successed
- `d0316d0021c1` wifi: cfg80211: reject HTC bit for management frames
- `61b83f92afbc` net: ipv4: fix incorrect MTU in broadcast routes
- `1de091a6fe31` ext4: do not BUG when INLINE_DATA_FL lacks system.data xattr
- `6b72479d6686` jbd2: prevent softlockup in jbd2_log_do_checkpoint()
- `e5f8bea7a947` fs/buffer: fix use-after-free when call bh_read() helper
- `e0961cf85935` usb: quirks: Add DELAY_INIT quick for another SanDisk 3.2Gen1 Flash Drive
- `3e154bb10374` net: usbnet: Fix the wrong netif_carrier_on() call
- `64e8efb8c134` tracing: Add down_write(trace_event_sem) when adding trace event
- `ae1a8769c0e3` USB: cdc-acm: do not log successful probe on later errors
- `72fadcf48789` alloc_fdtable(): change calling conventions.
- `54b8c8ed8628` ftrace: Fix potential warning in trace_printk_seq during ftrace_dump
- `c51659f5f910` scsi: core: sysfs: Correct sysfs attributes access rights
- `63794f831a7c` net: ipv4: fix regression in local-broadcast routes
- `a36ed859c25e` usb: core: usb_submit_urb: downgrade type check
- `dbb3112350bf` usb: hub: fix detection of high tier USB3 devices behind suspended hubs
- `ac25767ccd3c` net/packet: fix a race in packet_set_ring() and packet_notifier()
- `12f7d504dfa0` usb: gadget : fix use-after-free in composite_dev_cleanup()
- `103aea64adf6` fs: Prevent file descriptor table allocations exceeding INT_MAX
- `ad2f3c28f0a3` netfilter: ctnetlink: fix refcount leak on table dump
- `0f4815799389` net: usbnet: Avoid potential RCU stall on LINK_CHANGE event
- `58c3b0e7ca5f` usb: hub: avoid warm port reset during USB3 disconnect
- `63ee13edccd7` usb: hub: Don't try to recover devices lost during warm reset.
- `f3c9f4951d38` cdc-acm: fix race between initial clearing halt and open
- `3fbf291e1b91` ppp: fix memory leak in pad_compress_skb
- `0471194b744a` ALSA: usb-audio: Add mute TLV for playback volumes on some devices
- `288556648a61` ALSA: usb-audio: Fix block comments in mixer_quirks
- `560c1c62a6fb` ALSA: usb-audio: Avoid multiple assignments in mixer_quirks
- `e4336114e91d` ALSA: usb-audio: Simplify NULL comparison in mixer_quirks
- `d18e1b42e9e7` ALSA: usb-audio: Remove unneeded wmb() in mixer_quirks
- `0167590ca04c` usb: core: Add 0x prefix to quirks debug output
- `9a94a5980766` net: Fix null-ptr-deref by sock_lock_init_class_and_name() and rmmod.
- `94dea4b28fe8` blk-mq: check kobject state_in_sysfs before deleting in blk_mq_unregister_hctx
- `ef96d2880a21` block: use int to store blk_stack_limits() return value
- `d943053873e5` tcp: fix __tcp_close() to only send RST when required
- `038eea6bfcc2` usb: gadget: configfs: Correctly set use_os_string at bind
- `487595be3110` mm: hugetlb: avoid soft lockup when mprotect to large memory area
- `8f81f734bf03` Input: uinput - zero-initialize uinput_ff_upload_compat to avoid info leak
- `b73e72866835` pinctrl: check the return value of pinmux_ops::get_function_name()
- `d708592a7053` tcp: Don't call reqsk_fastopen_remove() in tcp_conn_request().
- `55b0e02b9e59` lib/genalloc: fix device leak in of_gen_pool_get()
- `f1cdc996fab9` mmc: core: SPI mode remove cmd7
- `62f565e67ad4` pid: Add a judgment for ns null in pid_nr_ns
- `91fabbb02503` exec: Fix incorrect type for ret
- `7cc0f7e81e7b` net: rtnetlink: remove redundant assignment to variable err
- `c01b0d79d07d` ext4: avoid potential buffer over-read in parse_apply_sb_mount_options()
- `ca92da26edd9` vfs: Don't leak disconnected dentries on umount
- `5d8f98b0462c` rtc: interface: Ensure alarm irq is enabled when UIE is enabled
- `9d5dd367ddc1` ext4: increase i_disksize to offset + len in ext4_update_disksize_before_punch()
- `5e4cb116f19a` rtc: interface: Fix long-standing race when setting alarm
- `697d8a82e098` tracing: Fix race condition in kprobe initialization causing NULL pointer dereference
- `5c05d0a6abe9` usbnet: Prevents free active kevent
- `d9714a48ec19` net: ipv6: fix field-spanning memcpy warning in AH output
- `ad81a423818e` char: misc: Does not request module for miscdevice with dynamic minor
- `f706479bf77d` ALSA: usb-audio: apply quirk for MOONDROP Quark2
- `004089c63cbb` usb: mon: Increase BUFF_MAX to 64 MiB to support multi-MB URBs
- `6f8ccf311bbb` ipv6: np->rxpmtu race annotation
- `a11ff1d09313` net_sched: remove need_resched() from qdisc_run()
- `dc9bcfc4bcae` regulator: fixed: use dev_err_probe for register
- `c0bd6fd98181` ipv4: route: Prevent rt_bind_exception() from rebinding stale fnhe
- `a67540a60aa4` usb: gadget: f_eem: Fix memory leak in eem_unwrap
- `98f113c39149` usb: storage: Fix memory leak in USB bulk transport
- `cc8417c01669` fs: writeback: fix use-after-free in __mark_inode_dirty()
- `e6ed95118b23` usb: gadget: f_ncm: Fix MAC assignment NCM ethernet
- `769e87f218d7` fs/proc: fix uaf in proc_readdir_de()
- `dbb799aa98c3` net/sched: sch_qfq: Fix null-deref in agg_dequeue
- `3b3c1dad4a1e` ext4: refresh inline data size before write operations
- `2792db6fc5af` ext4: add i_data_sem protection in ext4_destroy_inline_data_nolock()
- `e23e588e1884` crypto: asymmetric_keys - prevent overflow in asymmetric_key_generate_id
- `83d7ae2f619b` regulator: core: Protect regulator_supply_alias_list with regulator_list_mutex
- `4057820c66f5` fs/nls: Fix utf16 to utf8 conversion
- `2033ff03c9d6` fs/nls: Fix inconsistency between utf8_to_utf32() and utf32_to_utf8()
- `733e7a9388e8` usb: usb-storage: No additional quirks need to be added to the EL-R12 optical drive.
- `537a3459db9c` ext4: fix incorrect group number assertion in mb_check_buddy
- `b3b6ffaf2ccf` usb: usb-storage: Maintain minimal modifications to the bcdDevice range.
- `3bedcbe2404c` tracing: Do not register unsupported perf events
- `42f8169ba6dd` ipv4: Fix reference count leak when using error routes with nexthop objects
- `ea4b30efab36` ipv6: Fix potential uninit-value access in __ip6_make_skb()
- `6503c0c9f7fd` ipv4: Fix uninit-value access in __ip_make_skb()
- `908b0db514f0` HID: core: Harden s32ton() against conversion to 0 bits
- `dab88c4c088a` ext4: fix string copying in parse_apply_sb_mount_options()
- `bc4a6b7b9dcf` inet: ping: Fix icmp out counting
- `c6ed6a641e8d` net: sock: fix hardened usercopy panic in sock_recv_errqueue
- `4c4ef8aa289f` jbd2: avoid bug_on in jbd2_journal_get_create_access() when file system corrupted
- `b28425bd7b18` ext4: remove unused return value of __mb_check_buddy
- `8664ee6cbd00` ext4: improve integrity checking in __mb_check_buddy by enhancing order-0 validation
- `cf2644aaa858` crypto: seqiv - Do not use req->iv after crypto_aead_encrypt
- `3e8fdb67ebf2` jbd2: fix the inconsistency between checksum and data in memory for journal sb
- `be35917aa82e` HID: usbhid: paper over wrong bNumDescriptor field
- `e21477c49ff7` USB: OHCI/UHCI: Add soft dependencies on ehci_platform
- `227c7752c1d7` crypto: authencesn - reject too-short AAD (assoclen<8) to match ESP/ESN spec
- `34f1fa241af4` ALSA: usb-audio: Fix use-after-free in snd_usb_mixer_free()
- `9902e598e233` driver core: fix potential null-ptr-deref in device_add()
- `4bc6bc6e40a3` writeback: fix 100% CPU usage when dirtytime_expire_interval is 0
- `6b86e1d838dd` ring-buffer: Avoid softlockup in ring_buffer_resize() during memory free
- `b6a2d325fb62` HID: uclogic: Correct devm device reference for hidinput input_dev name
- `5731fb7ed50e` fs: add <linux/init_task.h> for 'init_fs'
- `b0005d98b2d6` PM: sleep: wakeirq: harden dev_pm_clear_wake_irq() against races
- `1f0084ce2423` sched/rt: Skip currently executing CPU in rto_next_cpu()
- `9a7aa6096256` pstore/ram: fix buffer overflow in persistent_ram_save_old()
- `3a48d6167258` fat: avoid parent link count underflow in rmdir
- `34fa0caccdaa` procfs: fix missing RCU protection when reading real_parent in do_task_stat()
- `6b4f9fae8afc` netfilter: nf_conntrack_h323: don't pass uninitialised l3num value
- `6a930dec1cc3` ext4: fix memory leak in ext4_ext_shift_extents()
- `36b85339e2be` rtc: interface: Alarm race handling should not discard preceding error
- `65dea3589dca` pstore: ram_core: fix incorrect success return when vmap() fails
- `04e475ac0549` HID: multitouch: add eGalaxTouch EXC3188 support
- `0ea945644bc3` netfilter: xt_tcpmss: check remaining length before reading optlen
- `53f7309951c3` ipv4: fib: Annotate access to struct fib_alias.fa_state.
- `7c2043307626` HID: hid-pl: handle probe errors
- `148053a991d8` HID: magicmouse: Do not crash on missing msc->input
- `83370a4e280c` HID: prodikeys: Check presence of pm->input_ep82
- `4d73db6c36f1` ext4: don't cache extent during splitting extent
- `6493a55c1178` xfrm6: fix uninitialized saddr in xfrm6_get_saddr()
- `bc88426b1d6d` hrtimer: Fix trace oddity
- `89d2eb5c5d41` wifi: radiotap: reject radiotap with unknown bits
- `9338dea0182a` unshare: fix unshare_fs() handling
- `78a95ed95a0d` netfilter: x_tables: guard option walkers against 1-byte tail reads
- `e8a1f39d95f4` net: Handle napi_schedule() calls from non-interrupt
- `8519b9b867c2` ext4: drop extent cache when splitting extent fails
- `c7ff09d66ac8` ext4: fix dirtyclusters double decrement on fs shutdown
- `d4cf9b608a4e` tracing: Fix syscall events activation by ensuring refcount hits zero
- `0f79ae4192ce` netfilter: nf_conntrack_sip: fix Content-Length u32 truncation in sip_help_tcp()
- `861e0795baae` netfilter: xt_time: use unsigned int for monthday bit shift
- `18b664793843` netfilter: nf_conntrack_h323: check for zero length in DecodeQ931()
- `2b81dcfa209e` icmp: fix NULL pointer dereference in icmp_tag_validation()
- `8ecd5d9470fb` af_key: validate families in pfkey_send_migrate()
- `b018b7a9b5ac` net: fix fanout UAF in packet_release() via NETDEV_UP race
- `0a07af54c400` netfilter: nf_conntrack_sip: fix use of uninitialized rtp_addr in process_sdp
- `0ccf88cbe5ee` sysctl: fix uninitialized variable in proc_do_large_bitmap
- `c8f6234d8ee5` ext4: avoid allocate block from corrupted group in ext4_mb_find_by_goal()
- `62d717f58902` ext4: reject mount if bigalloc with s_first_data_block != 0
- `4ffbcd096519` HID: wacom: fix out-of-bounds read in wacom_intuos_bt_irq
- `1c52f0b7304d` HID: multitouch: Check to ensure report responses match the request
- `8419c800b36f` net: ipv6: ndisc: fix ndisc_ra_useropt to initialize nduseropt_padX fields to zero to prevent an info-leak
- `e5a7fd6455b5` ip6_tunnel: clear skb2->cb[] in ip4ip6_err()
- `966d21669b6a` net: ipv6: flowlabel: defer exclusive option free until RCU teardown
- `c10f74e36879` netfilter: nfnetlink_log: account for netlink header size
- `7751e6776d86` netfilter: ctnetlink: zero expect NAT fields when CTA_EXPECT_NAT absent
- `de3d27a5ed1b` netfilter: x_tables: restrict xt_check_match/xt_check_target extensions for NFPROTO_ARP
- `3ac33b608197` ipv6: avoid overflows in ip6_datagram_send_ctl()
- `eb9ccd4f866e` USB: core: add NO_LPM quirk for Razer Kiyo Pro webcam
- `0bf91be4816d` Input: xpad - add support for Razer Wolverine V3 Pro
- `7e5b6916d967` xfrm_user: fix info leak in build_report()
- `3aa4ed8040e1` mm/hugetlb: make detecting shared pte more reliable
- `0b3343c283c5` net: rfkill: prevent unlimited numbers of rfkill events from being created
- `2f5c526de010` usb: gadget: u_ether: Fix race between gether_disconnect and eth_stop
- `6c801008c94d` ext4: fix the might_sleep() warnings in kvfree()
- `a165924659fb` ext4: convert inline data to extents when truncate exceeds inline size
- `d72871b3a245` net/tcp-md5: Fix MAC comparison to be constant-time
- `5e4f671fc032` netfilter: ctnetlink: remove refcounting in expectation dumpers
- `a5936cde4da3` netfilter: ctnetlink: fix use-after-free in ctnetlink_dump_exp_ct()
- `d30126998283` PM: runtime: Fix a race condition related to device removal
- `bf6a33159f15` netfilter: nfnetlink_log: fix uninitialized padding leak in NFULA_PAYLOAD
- `c7898ae6bfb1` virtio_net: Fix UAF on dst_ops when IFF_XMIT_DST_RELEASE is cleared and napi_tx is false
- `a5d6d84903ee` ptrace: slightly saner 'get_dumpable()' logic
- `801c0e7c8b9f` HID: roccat: fix use-after-free in roccat_report_event
- `2d50e2b363c1` xfrm_user: fix info leak in build_mapping()
- `e33854d482eb` netfilter: xt_multiport: validate range encoding in checkentry
- `a3bd8ec4943b` netfilter: ip6t_eui64: reject invalid MAC header for all packets
- `1c11d8b42144` HID: core: clamp report_size in s32ton() to avoid undefined shift
- `e35b33b1194f` usb: storage: Expand range of matched versions for VL817 quirks entry
- `058ba871dbce` xfrm: clear trailing padding in build_polexpire()
- `23e0dfcd2ff1` ALSA: usb-audio: apply quirk for MOONDROP JU Jiu
- `c3939bf9aa31` ALSA: usb-audio: stop parsing UAC2 rates at MAX_NR_RATES
- `b2d069b4eb06` ALSA: usb-audio: Avoid false E-MU sample-rate notifications
- `c1ccb6bc0d8d` ALSA: usb-audio: Fix Audio Advantage Micro II SPDIF switch
- `16b7e60e7afe` ALSA: control: Validate buf_len before strnlen() in snd_ctl_elem_init_enum_names()
- `cf6c4330b922` lib/ts_kmp: fix integer overflow in pattern length calculation *
- `9278c60df848` taskstats: set version in TGID exit notifications
- `db5c7f90edee` dm: don't report warning when doing deferred remove
- `965d75c68965` dm: fix a buffer overflow in ioctl processing
- `96ffc2f7f1cb` pstore/ram: fix resource leak when ioremap() fails
- `9115bec5606c` ppp: require CAP_NET_ADMIN in target netns for unattached ioctls
- `5f3364dd3822` ipv6: fix possible UAF in icmpv6_rcv()
- `be166b80dc49` netfilter: xtables: restrict several matches to inet family
- `7549df4f4225` slip: bound decode() reads against the compressed packet length
- `37747b51b83c` netfilter: xt_policy: fix strict mode inbound policy matching
- `db27241a1e8e` audit: fix incorrect inheritable capability in CAPSET records
- `77668ea36278` audit: enforce AUDIT_LOCKED for AUDIT_TRIM and AUDIT_MAKE_EQUIV
- `d5d1adff00ef` ALSA: usb-audio: Bound MIDI endpoint descriptor scans
- `72bbcc601c34` sysfs: don't remove existing directory on update failure
- `1f1f647ed8fd` ipv4: raw: reject IP_HDRINCL packets with ihl < 5
- `d63bf8dee7b3` l2tp: Drop large packets with UDP encap
- `347a6c423d06` crypto: authencesn - reject short ahash digests during instance creation
- `69afc7ff769c` slip: reject VJ receive packets on instances with no rstate array
- `a67ec9b00a05` ALSA: core: Validate compress device numbers without dynamic minors
- `2fa508411b92` HID: usbhid: fix deadlock in hid_post_reset()
- `3e15e174cdf0` netfilter: conntrack: remove sprintf usage
- `037e75df82d3` netfilter: arp_tables: fix IEEE1394 ARP payload parsing
- `4002687e69d4` netfilter: nf_conntrack_sip: don't use simple_strtoul
- `3ee66b5b54f8` netfilter: synproxy: refresh tcphdr after skb_ensure_writable
- `c958562c5fcf` ipv4: free net->ipv4.sysctl_local_reserved_ports after unregister_net_sysctl_table()
- `cb319346a70a` ipv6: exthdrs: refresh nh pointer after ipv6_hop_jumbo()
- `4c610dfc5864` ipv6: validate extension header length before copying to cmsg
- `4ea09159b6ae` ip6: vti: Use ip6_tnl.net in vti6_changelink().
- `dfe0e5e3698a` USB: quirks: add NO_LPM for Lenovo ThinkPad USB-C Dock Gen2 hub controllers
- `c2d7af3d0314` serial: zs: Switch to using channel reset
- `b5b2dcc22d96` usb: core: Fix SuperSpeed root hub wMaxPacketSize
- `00a4736fdab9` netfilter: xt_NFQUEUE: prefer raw_smp_processor_id
- `cafd818b725b` netfilter: conntrack_irc: fix possible out-of-bounds read
- `d75262fe06dd` ipv4: restrict IPOPT_SSRR and IPOPT_LSRR options
- `1a0657386695` signal: clear JOBCTL_PENDING_MASK for caller in zap_other_threads()
- `40848289f25c` ext4: validate p_idx bounds in ext4_ext_correct_indexes
- `be5b0202ec46` ipv6: sit: reload inner IPv6 header after GSO offloads
- `8c51a5622492` ip6_vti: fix incorrect tunnel matching in vti6_tnl_lookup()
- `825e37609f5d` ALSA: core: Fix potential data race at fasync handling
- `a17d7644a152` fuse: limit FUSE_NOTIFY_RETRIEVE to uptodate folios
- `a73dd03d2fac` ip6_vti: set netns_immutable on the fallback device.
- `33b8575b3fbd` net/sched: cls_u32: use skb_header_pointer_careful()
- `f3883f2b02d7` ext4: add bounds check for inline data length in ext4_read_inline_page
- `a59aab2a3d69` fbdev: modedb: Fix misaligned fields in the 1920x1080-60 mode
- `5787aee03b5f` ALSA: usb-audio: fix null pointer dereference on pointer cs_desc
- `f1c35a8e626c` ip6: vti: Use ip6_tnl.net in vti6_siocdevprivate().
- `33041ff74678` HID: wacom: Fix OOB write in wacom_hid_set_device_mode()
- `3490b988fb40` xfrm: route MIGRATE notifications to caller's netns
- `55d20b8d51d8` netfilter: conntrack: tcp: do not force CLOSE on invalid-seq RST without direction check
- `fff5e9af18f9` fs/fcntl: fix SOFTIRQ-unsafe lock order in fasync signaling
- `97fdf4d94dcf` thermal: core: Fix thermal zone governor cleanup issues
- `e1506f60389e` sched: Use u64 for bandwidth ratio calculations
- `19cca2f38f17` usb: gadget: function: rndis: add length check to response query
- `8b2c7beb020b` ALSA: usb-audio: Propagate errors in scarlett_ctl_enum_put()
- `3cdcdfff9561` virtio-mmio: fix device release warning on module unload
- `57b4f9d9caef` net: af_key: initialize alg_key_len for IPComp states
- `83bbf205258b` ipv4: igmp: remove multicast group from hash table on device destruction
- `27898fff2231` sched/rt: Have RT_PUSH_IPI be default off for non PREEMPT_RT
- `48b11866cab9` posix-cpu-timers: Fix pid refcount leak in do_cpu_nanosleep() error path
- `0a0ca36284fa` USB: core: add USB_QUIRK_NO_LPM for VIA Labs USB 2.0 hub
- `9d29fb905cab` usb: gadget: composite: fix dead empty check in the USB_DT_OTG handler
- `6d8a3279ff8f` net: add skb_header_pointer_careful() helper
- `791e6dcc1c1a` USB: quirks: add NO_LPM for the Samsung T5 EVO Portable SSD
- `2572e3dbffed` dm-ioctl: report an error if a device has no table
- `bb5a6bbf61a9` crypto: drbg - Fix returning success on failure in CTR_DRBG
- `e577071637bc` crypto: drbg - Fix the fips_enabled priority boost
- `56175dbbc0f6` fuse: fix device node leak in cuse_process_init_reply()
- `d095cb882189` fuse: re-lock request before returning from fuse_ref_folio()
- `7213ed9725f8` sysfs: clamp show() return value in sysfs_kf_read()
- `c514b939e10e` workqueue: drop spurious '*' from print_worker_info() fn declaration
- `24227b58bc3f` netfilter: synproxy: protect nf_ct_seqadj_init() with conntrack lock
- `87e2a23349b9` crypto: rng - Free default RNG on module exit
- `2b4d4306c16e` KEYS: Use acquire when reading state in keyring search
- `87e95dc25103` ipv6: fix error handling in disable_ipv6 sysctl
- `02085bc087f4` ipv6: fix error handling in ignore_routes_with_linkdown sysctl
- `88e1b7d076b2` ipv6: fix error handling in forwarding sysctl
- `b3b940b1c7f2` HID: picolcd: prevent NULL pointer dereference in picolcd_send_and_wait()
- `11821f3e0527` net: usb: net1080: validate packet_len before pad-byte access in rx_fixup
- `882570b16689` netfilter: xt_u32: reject invalid shift counts
- `eeb481f52517` ring-buffer: Fix event length with forced 8-byte alignment
- `f5f6f31e8c91` fbdev: sm712: Fix operator precedence in big_swap macro
- `843160c4a887` netfilter: nf_conntrack_irc: fix parse_dcc() off-by-one OOB read
- `f67d9966bdb1` dm-stats: fix dm_jiffies_to_msec64
- `0c0d7402caa5` dm-verity: increase sprintf buffer size
- `ebdff246e8e8` cgroup/cpuset: rebind mm mempolicy to effective_mems, not mems_allowed
- `a3421396d18d` proc: use generic setattr() for /proc/$PID/net
- `6854b090be74` ipv6: mcast: use rcu-safe version of ipv6_get_lladdr()
- `2c9c3bdc31e6` mld: fix suspicious RCU usage in __ipv6_dev_mc_dec()
- `e81953282b0e` ipv4: fib: free fib_alias with kfree_rcu() on insert error path
- `add02a52423d` ipv4: icmp: fill flow parameters in icmp_route_lookup decoy lookup
- `37cbbd45f7ac` pppoe: reload header pointer after dev_hard_header()
- `49ab21aafe51` netfilter: nf_conntrack_sip: widen NAT rewrite delta to s32 in sip_help_tcp()
- `7e1dbb884e1c` assoc_array: trim the final shortcut word using the current chunk end
- `5ac2bd6ac581` pinctrl: devicetree: don't free uninitialized dev_name on error path
- `6f5551cf6c2b` audit: fix potential use-after-free in audit_del_rule()
- `70e27e4afd4f` ALSA: usb-audio: fix OOB write in snd_usbmidi_akai_output()
- `529fc252003c` ALSA: usb-audio: Fix DMA buffer out-of-bounds write when fill_max is set
- `f696748b1eaf` ALSA: usb-audio: Clamp frame size in implicit-feedback mode
- `5f55f3687022` tracing: Check return value of __register_event() in trace_module_add_events()
- `679c9bfc756f` Input: evdev - sanitize event type index when fetching event masks
- `f8c40429de12` ALSA: usb-audio: fix OOB write on Type II inbound URBs
- `9b1cb25cad70` usb: gadget: f_ncm: Use unsigned int for ndp_index
- `06c1ac43dba0` ipv6: fix Route Information option length validation
- `dde446d38001` ip6_tunnel: clear skb2->cb[] in ip6ip6_err()
- `d3d11c9c53cf` selinux: reject a class permission count below its inherited common
- `2cf12234a4e8` audit: use 'unsigned int' instead of 'unsigned'
- `c8525f3028e3` ring-buffer: Remove jump to out label in ring_buffer_swap_cpu()
- `f65d2fece9e8` ring-buffer: Use current_context for safe per-CPU buffer swap
- `4e8a59eada37` ipv6: fib6: fix NULL deref in fib6_walk_continue() on multi-batch dump
- `df145dbe9ac1` netfilter: nf_nat_sip: reload possible stale data pointer
- `cafcb0f899e0` writeback: fix race between cgroup_writeback_umount() and inode_switch_wbs()
- `351849809025` iio: event: Fix event FIFO reset race
- `947913f148c7` cpufreq: Fix hotplug-suspend race during reboot
- `eb05537c6221` ipv6: addrconf: bail out of dad_failure when state is no longer POSTDAD
- `46ab3e2ae005` firmware_loader: Fix recursive lock in device_cache_fw_images()
- `c7db8f5d7930` xfrm: validate selector family and prefixlen during match
- `1d5e1e384c55` netfilter: nf_conntrack_reasm: guard mac_header adjustment after IPv6 defrag
- `194e25e00a30` netfilter: xt_nat: reject unsupported target families
- `161cbad7a6a8` net/packet: avoid fanout hook re-registration after unregister
- `3b48b3276ef4` ftrace: Add global mutex to serialize trace_parser access
- `5ffc6bb1644a` net: ip_gre: require CAP_NET_ADMIN in the device netns for changelink
- `f971edd21532` i2c: core: fix adapter registration race
- `12015197ef38` xfrm: defensively unhash xfrm_state lists in __xfrm_state_delete
- `8040d7c8fc8f` ipv6: mcast: Fix potential UAF in MLD delayed work
- `0e84055b98a8` skbuff: introduce skb_pull_data
- `ab9db3ea4343` audit: fix potential integer overflow in audit_log_n_string() *
- `99d877efc47c` net: ip_vti: require CAP_NET_ADMIN in the device netns for changelink
- `537991358ff7` net: ip6_tunnel: require CAP_NET_ADMIN in the device netns for changelink
- `03e6c8544aa7` net: sit: require CAP_NET_ADMIN in the device netns for changelink
- `1d109f80c321` net: ip6_vti: require CAP_NET_ADMIN in the device netns for changelink
- `0ef412909416` usb: gadget: f_fs: initialize reset_work at allocation time *
- `fbdc16b25738` bpf: fix overflow in prog accounting *
- `e5a8ef2f119b` mm/slub: avoid accessing metadata when pointer is invalid in object_err() *
- `59575faf7fe9` ext4: fix wrong unit use in ext4_mb_clear_bb *
- `8b7f51ff7353` Revert "rtnetlink: Reject negative ifindexes in RTM_NEWLINK" *
- `560553980e1f` usb: musb: Get the musb_qh poniter after musb_giveback *
- `531f67f23f4f` USB: usbcore: Introduce usb_bulk_msg_killable() *
- `5c9b964e128e` mmc: core: Fix kernel panic when remove non-standard SDIO card *
- `e410676af66e` net: fix a memleak when uncloning an skb dst and its metadata
- `87d43d5fe2d4` scm: fix MSG_CTRUNC setting condition for SO_PASSSEC
- `c69538a72e5f` net: remove CAP_SYS_RAWIO zero-padding in dev_validate_header
- `add0e1f1f93d` ipv6: prevent in6_dev_get() from resurrecting inet6_dev
- `5fafea55366b` ipv4: fib: Don't ignore error route in local/main tables.
- `4768e869d7b0` tcp/dccp: complete lockless accesses to sk->sk_max_ack_backlog
- `131c5fe4fafb` tcp/dccp: allow a connection when sk_max_ack_backlog is zero
- `dc69bdfc565d` ip: Fix a data-race around sysctl_fwmark_reflect.
- `5ecdc984c93a` tcp/dccp: Fix a data-race around sysctl_tcp_fwmark_accept.
- `4f669cb975a7` net: Fix a data-race around sysctl_net_busy_poll.
- `0dff7a9719b6` tcp: Reduce chance of collisions in inet6_hashfn().
- `3b9b02fa7179` sock: make lockdep_sock_is_held static inline
- `1229c813bcd9` net: Fix build failure due to lockdep_sock_is_held().
- `125b1adef813` add barriers to buffer_uptodate and set_buffer_uptodate
- `978ef2996df4` fs: only do a memory barrier for the first set_buffer_uptodate()
- `65f9334cad83` poll_wait: add mb() to fix theoretical race between waitqueue_active() and .poll()
- `a5e51db5eae3` fsnotify: Do not generate events for O_PATH file descriptors
- `01244b9be2f6` bpf: Don't use %pK through printk
- `080d120eabaf` netdev: preserve NETIF_F_ALL_FOR_ALL across TSO updates
- `28595af90aea` net: Fix features skip in for_each_netdev_feature()
- `7a23cbd3cbc1` crypto: drbg - Fix drbg_max_addtl() on 64-bit kernels
- `a497e81b384e` mm/ksm: fix flag-dropping behavior in ksm_madvise
- `c298467c605f` rfs: annotate lockless accesses to sk->sk_rxhash
- `d4df28ff5191` net/sched: make psched_mtu() RTNL-less safe

## 1. Mali GPU driver: highest priority

`/dev/mali0` is reachable from every app, and Mali kernel-driver bugs have been exploited in the wild on Android.

- In this tree: Midgard **r28p0-01rel0** (`drivers/gpu/arm/midgard`).
- On the watches: kernel and userspace (`/vendor/lib*/egl/libGLES_mali.so`) are **r21p0-01rel0**, UK interface 11.0.

Known Midgard issues to patch, all in r28p0:

| CVE | What | Status |
|---|---|---|
| CVE-2021-28663 / CVE-2021-28664 | memory import / page refcount | exploited in the wild |
| CVE-2022-36449 | GPU memory access after free | – |
| CVE-2022-38181 | JIT memory handling UAF | exploited in the wild |
| CVE-2023-4211 | `munmap` / `mmap_lock` race (UAF) | exploited in the wild |
| CVE-2022-46395, CVE-2023-26083 | Arm security bulletin, check applicability to Midgard | – |

Approach:
- [ ] **Cherry-pick the individual fixes into r28p0**, from Arm's later Midgard releases (last Midgard drop: r32p1) or the patches published with each Arm Mali security bulletin.
- [ ] Don't jump the whole driver to r32p1 unless the userspace blob is tested with it. The kernel driver must keep the user-kernel (UK) interface the vendor's r21p0 userspace expects. First check that r28p0 already accepts the r21p0 userspace (UK 11.x, minor-version negotiation). That's a boot-time test: GPU must work.

## 2. Linux 4.4 stable: 4.4.147 → 4.4.302

About 155 stable releases, hundreds of fixes: network stack, filesystems, USB, TTY, ALSA, netfilter, `af_unix`, binder.

Options, best first:
- [ ] **Merge AOSP's Android common kernel `android-4.4-q`** (`android.googlesource.com/kernel/common`, branch `android-4.4-q`). This tree is Android 10 based, and that branch already has 4.4.302 plus Android's own fixes merged and conflict-resolved against Android code (binder, ashmem, ION, cpufreq-interactive). Expect conflicts only in UNISOC-modified files.
- [ ] Or merge upstream `linux-4.4.y` tag by tag (`v4.4.148` … `v4.4.302`). More conflict work, same result for the core kernel, without the Android-specific fixes.
- [ ] After either: build, boot, run the per-device test checklist (`TODO.md`, step 7) after each batch. Bisect any regression to the batch that caused it.

## 3. After 4.4.302: CIP security branch

- [x] (done, see "CIP linux-4.4.y-st after 4.4.302") Merge or cherry-pick from CIP `linux-4.4.y-st` (security-only, maintained into 2027) for fixes after February 2022, for example netfilter, `cls_route` (CVE-2022-2588), `af_packet`, USB gadget, TTY.
- [ ] Only take what applies to our config: drivers not built (`=n`) don't matter. Use the CIP CVE tracking lists to filter.

## 4. Vendor (UNISOC) drivers

- [ ] Go through the UNISOC monthly security bulletins (2019 → now) for the kernel drivers we build: SIPC / `sbuf` / `spipe`, WCN (`sprdwcn_bsp`, `sprdwl_ng`), ION, display (ADF / dispc), camera, audio, modem control, `sprd_dsp`.
- [ ] Device nodes reachable from apps are the ones that matter. Check `ueventd*.rc` permissions and the SELinux policy for each `/dev/*` node.
- [ ] This depends on UNISOC publishing patches. Where only an advisory exists, assess and fix locally.

## 5. Out-of-tree and project code

- [ ] Custom modules (`aod_idle`, `batt_idle`, `wearit_aod`): review their sysfs/ioctl input handling, since they expose `/sys/kernel/*` knobs. Make writes root-only.
- [ ] Ported drivers (CST0xx touch, HRS3xxx heart rate): validate lengths from I2C and from user writes.
- [ ] **KernelSU**: this tree carries KernelSU hooks (`fs/exec.c`, `fs/open.c`). That is root by design. Decide per build: off for a fleet build, or on with the manager restricted. It must not be on silently.

## 6. Hardening config (cheap, no backport)

Done as two fragments in `arch/arm64/configs/`. Apply them on top of the full watch config with `scripts/kconfig/merge_config.sh -m -O $O $O/.config <fragment>` and then `make olddefconfig`. Each option and its cost is commented in the file; the effective values were checked after `olddefconfig`.

- `watch_hardening.fragment`: for every build.
- `watch_release.fragment`: apply after the first one for builds that go to users.

| Option | Choice | Why |
|---|---|---|
| `HARDENED_USERCOPY`, `CC_STACKPROTECTOR_STRONG`, `DEBUG_RODATA`, `DEBUG_SET_MODULE_RONX` | on | Already set in the device's stock 4.4.83 config and in `sprd_sharkle_defconfig`. Pinned so they can't be lost. |
| `ARM64_SW_TTBR0_PAN` | on | Off in the device's stock 4.4.83 config, on in `sprd_sharkle_defconfig`. A53 has no hardware PAN. Small uaccess cost. |
| `DEBUG_LIST` | on | Cheap list corruption check. |
| `SECURITY_DMESG_RESTRICT`, `SECURITY_PERF_EVENTS_RESTRICT` | on | Kernel log and perf are root-only by default. |
| `BPF_JIT_ALWAYS_ON` | on | Removes the BPF interpreter. Only classic BPF/seccomp is used here. |
| `SECCOMP_FILTER` | on | Android 8.1 needs it. |
| `USER_NS` | off | Android 8.1 (zygote, WebView and Chrome sandbox) uses seccomp and isolated UIDs, not user namespaces. The device's stock 4.4.83 config already has it off. |
| `LDISC_AUTOLOAD`, `LEGACY_PTYS`, `DEVMEM`, `DEVKMEM`, `PROC_KCORE`, `SECURITY_SELINUX_DISABLE` | off | Attack surface. PPP async, the only ldisc used, is built in. |
| `RANDOMIZE_BASE` (KASLR), `UNMAP_KERNEL_AT_EL0` (KPTI) | **left as in `sprd_sharkle_defconfig` (both on)** | Not touched by the fragment. `android-base.config` requires `RANDOMIZE_BASE=y`. A seed would come from the bootloader, which inserts `/chosen/kaslr-seed` at boot (never in a `.dts`); the kernel zeroes the value but keeps the property (`kaslr.c`). In this tree KASLR forces KPTI on (`unmap_kernel_at_el0()` in `cpufeature.c`). |
| `MODULE_SIG` | **off** | The vendor modules are loaded from an unsigned ramdisk and by the Wi-Fi HAL, with an unlocked bootloader. Needs a signing key process for little gain. |
| `SLAB_FREELIST_RANDOM/HARDENED`, `REFCOUNT_FULL`, `PAGE_POISONING` | not available / not cheap | Not in this 4.4 tree (`PAGE_POISONING` is only selected by `DEBUG_PAGEALLOC`). A backport is a possible follow-up. |
| `KEXEC` | n/a | Doesn't exist on arm64 in 4.4. |

Release fragment: `DEBUG_FS`, `FTRACE`, `KALLSYMS_ALL`, `SLUB_DEBUG_ON`, `SLUB_STATS` and `SECURITY_SELINUX_DEVELOP` off. Development builds keep them, since `DEBUG_FS` is needed for the current measurements. `atrace` stops working in release builds.

SELinux must stay **enforcing** in builds that go to users. The DM63 unit currently runs Permissive. With `SECURITY_SELINUX_DEVELOP` off in the release fragment, the kernel cannot be switched to permissive, so the policy denials must be cleaned up first. The release fragment is not ready to use until then: it also needs a release build booted on each device with init, zygote, vold, netd and the vendor HALs checked, including any vendor consumer of debugfs.

Open item (KASLR/KPTI): on hardware, as root with `kptr_restrict` set to 1 (Android sets 2, which shows 0 even to root), check whether `_text` in kallsyms changes across reboots. A missing `/proc/device-tree/chosen/kaslr-seed` means no seed; its presence alone is inconclusive (the kernel ignores a zero or malformed seed and wipes the value). If no seed is passed, KASLR randomizes nothing, and dropping `RANDOMIZE_BASE` and `UNMAP_KERNEL_AT_EL0` would save the KPTI cost on the A53 (not Meltdown-affected). That is a later battery decision and a deviation from `android-base.config`.

Build rule: every external or ramdisk `.ko` (`mali`, `sprdwl_ng`, `sprdbt_tty`, `sprd_fm`) must be rebuilt against the matching `O=` output (dev or release). Do not mix them: MODVERSIONS symbol CRCs and the FTRACE `_mcount` calls differ between the two (and any module built against a config with a different `SW_TTBR0_PAN` setting, such as the stock one, has different uaccess code).

The release fragment also drops these options through dependencies: `DYNAMIC_DEBUG`, `IODEBUG`, `IODEBUG_BDI`, `IODEBUG_VFS`, `SPRD_CPU_USAGE`, `SPRD_EIRQSOFF`, `F2FS_STAT_FS`, plus `TRACING`, `EVENT_TRACING`, `TRACEPOINTS` and the tracers.

To verify on hardware: boot with `SW_TTBR0_PAN` (vendor drivers that touch user memory); PPP/Wi-Fi/BT/GNSS paths with `LDISC_AUTOLOAD` off; seccomp policy install (zygote, mediacodec, mediaextractor) and the DHCP and packet-socket filters with `BPF_JIT_ALWAYS_ON`; a release build with SELinux enforcing.

## 7. Userspace (not this repo, for completeness)

The Android userspace on the watches is 8.1 at patch level 2019-11-05. A patched kernel doesn't fix userspace bugs: media framework, Bluetooth stack, Wi-Fi supplicant, WebView. Track that separately.

## Order of work

1. First booting build (no backports), so regressions can be told apart from bring-up problems.
2. Mali fixes (smallest change, highest risk reduction).
3. TCP SACK, `af_unix` GC, `seq_file`, epoll: small, targeted cherry-picks.
4. Full stable merge (`android-4.4-q`), in batches with boot tests.
5. CIP post-302 fixes, UNISOC bulletin fixes.
6. Hardening config.

## UNISOC bulletins

Sources: UNISOC security bulletins 2023-01, 2023-03, 2023-05 and 2024-02 (SC9832E listed as affected, all versions), read through the cybersecurity-help.cz mirrors. UNISOC publishes no patches or function names, so each entry was matched to code from the one-line description. Where the match is a guess it says "probable" and the fix is defensive, not a claimed fix of that CVE. Only kernel components are listed; Android service, modem and WLAN firmware entries in the same bulletins are out of scope. Bulletins after 2024-02 were not reviewed.

| CVE | Component | Applies? | Status | Note |
|---|---|---|---|---|
| CVE-2022-39116, CVE-2022-39118 | sprd_sysdump | yes (CONFIG_SPRD_SYSDUMP=y) | fixed (defect), attribution probable (description match), "sysdump: bound sprd_sysdump write..." | 5-byte stack buffer, terminator written at [count]. The bug is real; that it is the bulletin bug rests on the description only. |
| CVE-2022-47488 | spipe (sipc sbuf) | yes | probable, "sipc: sbuf: bound ring indexes..." and "sipc: spool: validate file offset..." | Peer-written rdptr could push txsize past the ring; int truncation of count; unchecked bufid; spool lseek offset. Defensive, match unproven. |
| CVE-2022-48385 | cp_dump | yes (cproc "mem" dump) | probable, "cproc: reject negative seek offsets..." and "cproc: bound mini_dump segment offset..." | Negative lseek offset, and an int-truncated mini_dump offset after a large lseek, mapped physical memory below the modem region. |
| CVE-2022-48389 | modem control device | yes (cproc mini_dump) | probable, same cproc commit | sprintf into a fixed stack buffer from shared-memory names. |
| CVE-2022-48380, CVE-2022-48381 | modem control device | partly | not located | "Resource exhaustion / OOB write"; no matching code found in the built cproc. sprd_modem_control.c is not built on this config (hardened anyway, "modem_control: bound mini_dump..."). |
| CVE-2023-49667, CVE-2023-49668, CVE-2023-49669, CVE-2023-49670 | cp_dump | yes | probable, covered by the cproc commit | Same code as CVE-2022-48385; no separate bug found. |
| CVE-2023-49671 | ion | yes | probable, "ion: check dma-buf is an ion buffer..." and "ion: check ion dma-buf in sprd_ion helpers..." | Type confusion on non-ion dma-bufs in INVALIDATE, MSYNC and the exported helpers. Match unproven. |
| CVE-2023-49672 | ion (use-after-free, logic error) | yes | probable; fixed in tree on branch security/ion-lifetime (commits not yet merged/boot-tested) | `get_ion_buffer()` in sprd_ion.c dropped the dma-buf reference before callers used the returned ion_buffer (concurrent close frees it). Fixed by new `sprd_ion_get_buffer_ref()` / `sprd_ion_get_sg_table_ref()` / `sprd_ion_put()` and by converting every in-tree caller to hold the reference for as long as it uses the pointer; see "Concrete residual defects". One caller (dcam_r6p0 SPRD_IMG_IO_GET_SG) cannot be fixed this way. Separate hardening commits stop ION_SPRD_CUSTOM_MAP_KERNEL leaking a kernel address and add is-ion checks to the helpers. |
| CVE-2022-48239, CVE-2022-48240 | camera driver | yes (CONFIG_SPRD_CAMERA=y) | not located | Description names no ioctl; not audited line by line. |
| CVE-2022-48237, CVE-2022-48238 | Image Filter | unknown | not located | No matching module found in the tree. |
| CVE-2022-47469, CVE-2022-47470, CVE-2022-47486 | ext4fsfilter | no | not applicable | Driver not present in this tree. |
| CVE-2022-47457, CVE-2022-47458 | FM driver | no | not applicable | sprdwcn_bsp FM driver exists in the tree but no FM option is enabled in the sharkle config. |
| CVE-2022-47453 | WCN service | yes (SC2342 integrated) | not located | Missing parameter check; no function named. |
| CVE-2022-47454, CVE-2022-47455, CVE-2022-47456, CVE-2022-47459 | WLAN driver (sprdwl_ng) | yes (module) | not located | Descriptions give no function; the 2023-01 WLAN entries (CVE-2022-44xxx) are WLAN firmware. |
| CVE-2022-47460 | GPU device | yes | not UNISOC code | Mali use-after-free, see the Mali section. |
| (no CVE assigned, Aug 2026) | Modem VoLTE chain | n/a | firmware | Modem side, no kernel patch possible. It makes peer-written shared-memory values (sbuf/sblock pointers) untrusted, which is why the sbuf bounds fixes matter. |

Concrete residual defects (real bugs in this tree, independent of CVE attribution, still open):
- ion (largely fixed on security/ion-lifetime, not yet boot-tested): `sprd_ion_get_buffer()` and `sprd_ion_get_sg_table()` returned an `ion_buffer`/`sg_table` pointer after dropping the fd's dma-buf reference (use-after-free with a concurrent close; probable CVE-2023-49672). New `sprd_ion_get_buffer_ref()`, `sprd_ion_get_sg_table_ref()` return the reference in `*held`; `sprd_ion_put(held)` drops it. All in-tree callers were converted: vsp/jpg/vpp (`*_get_iova`, put right after `sprd_iommu_map`), camera cpp r1p0/r2p0/lite_r1p0/lite_r2p0/lite_r3p0/lite_r4p0 (`cpp_get_sg_table` keeps the reference in `pfinfo->dmabuf_p[]`, `cpp_free_addr` drops it), isp g2v1/g2v3 (reference held only while the table is consumed; firmware buffer kept in `dmabuf_p` until the last release), dcam_if_r5p0_isp_r6p12 (ISP statistics reference in `isp_statis_dmabuf`, dropped on re-config/deinit; `sprd_cam_buf_sg_table_get` now uses one lookup and the per-camera list owns a reference until `sprd_cam_buf_sg_table_put`), cam_sl5_roc1 (`cambuf_get_ionbuf` takes one reference, dropped by `cambuf_put_ionbuf`). cam_iommu and gsp already held their own reference. Ownership notes: cpp keeps the reference per slot from `get_sg_table` until `free_addr` (a slot whose unmap fails keeps its reference, a leak being safer than a use-after-free) and clears buffer/size/iova together with the reference; isp g2v1/g2v3 pin buffers programmed into the hardware on a per-pipeline list released when the pipeline is closed. The old `sprd_ion_get_buffer()`/`sprd_ion_get_sg_table()` remain only for the case below and are marked deprecated. Still open: dcam_r6p0 `SPRD_IMG_IO_GET_SG` returns the raw kernel `sg_table` pointer to user space and `SPRD_IMG_IO_MAP_IOVA`/`UNMAP_IOVA` use the pointer user space passes back (`dcam_get_sg`, `dcam_get_iova`, `dcam_free_iova` in dcam_img.c). That is a kernel address leak plus a user-controlled pointer, not a reference-count problem; it needs a uapi change (pass the fd, look it up in the kernel) and was not changed. `sprd_ion_get_phys_addr()` and `sprd_ion_is_reserved()` were fixed earlier.
- sbuf/sblock: `sblock_receive()` trusts the peer-supplied block address and length when computing `rxrecord` indexes, and `spool_read()` copies `blk.length` bytes from `blk.addr`. A compromised modem can drive out-of-bounds reads and writes there.
- sbuf: the debugfs/status dump still reads ring sizes and pointers from the peer-writable header (debug only).
- ION_SPRD_CUSTOM_PHYS returns physical addresses to any ion client.

Uncertain attribution (separate from the above): the "probable" rows are matched from one-line bulletin text only; a fix there does not prove the bulletin bug is closed.

### Notes for the security team (ion lifetime series)

- After an orphan unmap (`sprd_ion_unmap_dma()` when an ion buffer is destroyed while still mapped) a stale user-held IOVA can be reallocated to another client, so a later FREE_IOVA / unmap with the old address can remove another client's mapping. This is a design risk of the IOVA API (callers pass raw iova/size back), outside this series and not changed.
- dcam_r6p0 (`dcam_img.c`, `SPRD_IMG_IO_GET_SG`, `MAP_IOVA`, `UNMAP_IOVA`) still returns a raw kernel `sg_table` pointer to user space and trusts the pointer passed back: kernel address leak and user-controlled kernel pointer. Not fixed; needs a uapi change (pass the fd).
- cpp (all variants): a request whose first buffer (`mfd[0]`) is 0 or whose lookup fails used to reach the hardware with IOVA 0 in the source/destination registers. This was reachable before the ion lifetime series too: `get_sg_table` skips `mfd <= 0`, the per-call state starts zeroed (and, on later calls, kept the previous call's size/iova), `get_addr` skipped the slot and the ignoring void callers programmed `iova[0]` regardless. With the iommu extension's out-of-range bypass (sprd_iommuex_cll.c, an IOVA outside the pool is passed through as a physical address; the CPP IOVA pool starts at 0x60000000) IOVA 0 becomes physical address 0, i.e. a user-sized DMA write to low physical memory. The series now makes `get_addr` fail with -EINVAL for a missing first plane and makes the r1p0/r2p0/lite_r1p0 start functions refuse to start when a buffer failed to map. The bypass itself is a vendor design risk (any stale or out-of-range IOVA reaches physical memory) and was not changed.
- isp g2v3/g2v1: `sprd_isp_path_set` (called from the dcam driver) reads `s_isp_pipeline[id]` without locking, so it can race with `isp_release` freeing the pipeline (pre-existing). Pins are now kept on the group, which outlives the pipelines, so they no longer land in a freed object, but the unlocked pipeline pointer use remains; serializing it with the release needs a lock order against the dcam caller and was not attempted.
