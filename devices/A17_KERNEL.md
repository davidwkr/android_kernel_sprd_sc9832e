# Android 17 kernel features on the 4.14 watch kernel

**Read first:** unmodified Android refuses this kernel from Android 15 on. AOSP's network BPF loader
(`NetBpfLoad.cpp`) requires 4.14 for Android 14, 4.19 for Android 15 and 5.4 (and a BPF JIT) for later
releases, and init reboots when it fails (see the BPF section). So these backports serve Android 14
(Go), the newest release that runs on 4.14 as is; Android 15 to 17 would also need a patched loader in
our own ROM build, which is not done here.

Reference: Xiaomi apollo 4.19 (`kernel_xiaomi_apollo`). Config lines live in
`arch/arm64/configs/watch_414_a17.fragment`.

## binderfs (`CONFIG_ANDROID_BINDERFS`)

Backported from the apollo/android-4.19 series, original authorship kept
(`UPSTREAM:` subjects, upstream SHAs in each commit message):

- 3ad20fe393b3 binder: implement binderfs, plus fdd94acd50d6, b6c770d7c9dc,
  849d540ddfcd (max option), c13295ad219d (binderfs.h), 36bdf3cae09d,
  211b64e4b5b6 (+ 01684db950ea, 36975fc3e5f2, 01b3f1fc5683, 4198479524ae,
  29ef1c8e16ae, da8ddba566ff, 7e7ca7744a53, 7fefaadd6a96, e98e6fa18636,
  7c4d08fc4d5a, 5b9633af298b): binderfs follow-up fixes
- ca2864c6e896, 028fb5822b76: default devices through binderfs, name validation
- f00834518ed3, 0e13e452dafc, c31e73121f4c, 4feb80faf428: `stats=global` mount
  option, stats/state/transactions/transaction_log/proc/<pid> files
- 2669b8b0c798 + f0fe2c0f050d "prevent UAF for binderfs devices" I and II,
  "prevent UAF read in print_binder_transaction_log_entry()", `__u32` device
  numbers (uapi `struct binderfs_device`, `BINDER_CTL_ADD` unchanged)

4.14 adaptations (separate commit): no `linux/xarray.h`, `ida_alloc_max()` /
`ida_free()` replaced by `ida_simple_get()` / `ida_simple_remove()`;
`BINDER_DEBUG_ENTRY()` kept instead of `DEFINE_SHOW_ATTRIBUTE()` in binder.c.

Config: `CONFIG_ANDROID_BINDERFS=y` (with the existing
`CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"`). With binderfs
enabled the legacy misc devices are not registered; the devices appear in each
binderfs mount instead.

### Not done
- `features/` directory (`oneway_spam_detection`, `freeze_notification`):
  apollo advertises both, but 4.14 lacks `BINDER_ENABLE_ONEWAY_SPAM_DETECTION`,
  `BINDER_FREEZE`, `BINDER_GET_FROZEN_INFO` and freeze notifications, so
  advertising them would be wrong. They are separate binder.c/binder_alloc.c
  backports (apollo: "BACKPORT: binder: Add freeze notification support for
  Android 14+" and the oneway spam detection series). Neither apollo nor 4.14
  has `BINDER_GET_EXTENDED_ERROR` ("extended_error").
- SELinux policy for `binder` fs type / `/dev/binderfs` labelling is a
  userspace policy matter (genfscon).

### Test on a device
    adb shell mount | grep binder        # init mounts binder at /dev/binderfs
    adb shell ls -l /dev/binderfs        # binder hwbinder vndbinder binder-control features?
    adb shell ls /dev/binderfs/binder_logs   # state stats transactions transaction_log proc/
    adb shell 'mkdir /tmp/b; mount -t binder -o max=4 binder /tmp/b; ls /tmp/b'
Check `dmesg | grep -i binder` for errors, and that servicemanager/hwservicemanager
start (they open /dev/binder, /dev/hwbinder, /dev/vndbinder via the symlinks).

Reference: Xiaomi apollo 4.19 (boots Android 17). Each section below is one
feature group backported to `kernel-4.14` from the commits apollo carries.

## pidfd and close_range (branch a17/pidfd)

Syscalls, numbers identical to upstream, wired for arm64 native and the
arm32 compat table (zygote64_32):

| syscall            | nr  |
|--------------------|-----|
| pidfd_send_signal  | 424 |
| pidfd_open         | 434 |
| close_range        | 436 |

`__NR_syscalls` and `__NR_compat_syscalls` are 437; 4.14 lacks 425-433 and
435 (io_uring, clone3, ...), those slots stay `sys_ni_syscall`. clone3,
pidfd_getfd and process_madvise are not added (apollo lacks them too).
`CLONE_PIDFD` (0x1000) works for `clone()`/`fork` paths; pidfds are pollable
(readable once the thread group has exited) and `pidfd_send_signal()` also
accepts a `/proc/<pid>` directory fd.

### Commits (apollo hash order, upstream subject kept)

- signal: add pidfd_send_signal() syscall; don't silently convert SI_USER
  signals to non-current pidfd; use fdget() since we don't allow O_PATH
- Make anon_inodes unconditional (also dropped `select ANON_INODES` everywhere)
- clone: add CLONE_PIDFD; signal: support CLONE_PIDFD with pidfd_send_signal;
  fork: do not release lock that wasn't taken; signal: improve comments
- pidfd: add polling support; pidfd: fix a poll race when setting exit_state
- pid: add pidfd_open(); arch: wire-up pidfd_open()
- fork: don't check parent_tidptr with CLONE_PIDFD
- copy_process(): don't use ksys_close() on cleanups, folded with
  fork: return proper negative error code and
  copy_process(): Move fd_install() out of sighand->siglock critical section
- open: add close_range(); close_range: add CLOSE_RANGE_UNSHARE;
  fs, close_range: add flag CLOSE_RANGE_CLOEXEC; close_range: unshare all fds
  for CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC; file: fix close_range() for
  unshare+cloexec; file: simplify logic in __close_range(); fs: fd tables have
  to be multiples of BITS_PER_LONG; close_range(): fix the logics in
  descriptor table trimming; fix bitmap corruption on close_range() with
  CLOSE_RANGE_UNSHARE; arch: wire-up close_range()

Adaptations for 4.14 are noted in each commit message ("[kernel-4.14: ...]").
Notable: no `clear_siginfo()`, no `PIDTYPE_TGID` (pidfd_open checks
`thread_group_leader()`), no `__poll_t`, no `bitmap_size()`, `sys_close()`
instead of `ksys_close()`; the SPRD ION `get_ion_user_info()` hook in
`__close_fd` moved into `pick_file()` so close_range() releases ION mapper
records too.

### Not taken

- fork: add clone3 and clone: fix CLONE_PIDFD support: the latter fixes
  clone3 argument handling (needs `kernel_clone_args`); 4.14 already rejects
  `CLONE_PIDFD | CLONE_PARENT_SETTID` in `copy_process()`.
- fork: fix pidfd_poll()'s return type: 4.14 has no `__poll_t`.
- P_PIDFD for waitid(), PIDFD_NONBLOCK, pidfd_get_pid() move,
  thread_group_exited(): apollo carries them, not requested; add them if
  Android 17 userspace turns out to need `waitid(P_PIDFD)`.

### Config

`arch/arm64/configs/watch_414_a17.fragment` (merge after watch_414 and
watch_414_adf). Only `CONFIG_PROC_FS=y` is needed; the syscalls and
anon_inodes are unconditional.

### Testing on a device

1. `strace`-less check from a static arm64 and an arm32 binary: call
   `syscall(424/434/436, ...)`; ENOSYS means the table entry is missing.
2. `pidfd_open(getpid(), 0)` returns an fd; `cat /proc/self/fdinfo/<fd>`
   shows `Pid:`. `pidfd_open(tid_of_non_leader, 0)` fails with EINVAL.
3. `fork` child via `clone(SIGCHLD | CLONE_PIDFD, ..., &pidfd)`: poll the
   pidfd, it must stay quiet until the child exits. `CLONE_PIDFD |
   CLONE_PARENT_SETTID` and `CLONE_PIDFD | CLONE_THREAD` give EINVAL.
4. `pidfd_send_signal(pidfd, 0, NULL, 0)` returns 0 on a live process, ESRCH
   after it is reaped; flags != 0 gives EINVAL.
5. `close_range(3, ~0U, 0)` closes everything above stderr;
   `CLOSE_RANGE_CLOEXEC` (1<<2) only sets FD_CLOEXEC;
   `CLOSE_RANGE_UNSHARE` (1<<1) with a CLONE_FILES sibling thread must leave
   the sibling's table intact.
6. On Android 17 userspace: boot, check `logcat | grep -i pidfd` for
   ENOSYS from zygote/libprocessgroup/ActivityManager.

Reference: the Xiaomi apollo 4.19 kernel (`kernel_xiaomi_apollo`), which boots
Android 17. Each backport below is the commit apollo carries (original authorship
kept, `(cherry picked from apollo commit ...)` trailer added); where 4.14 differs
the commit message carries a `[4.14: ...]` note. Config goes in
`arch/arm64/configs/watch_414_a17.fragment`, merged on top of
`sprd_sharkle_defconfig` + `watch_414.fragment` (+ `watch_414_adf.fragment`).

## userfaultfd (ART userfaultfd-based GC)

### What ART asks the kernel for

- `userfaultfd(O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY)`, falling back to
  the flag-less call when the kernel rejects it with `EINVAL`.
- `UFFDIO_API` with `UFFD_FEATURE_SIGBUS` and, when probing for the shadow-map
  (minor fault) mode, `UFFD_FEATURE_MINOR_SHMEM`; `UFFDIO_REGISTER` with
  `UFFDIO_REGISTER_MODE_MISSING` and (shmem) `UFFDIO_REGISTER_MODE_MINOR`;
  `UFFDIO_COPY`, `UFFDIO_ZEROPAGE`, `UFFDIO_CONTINUE`, `UFFDIO_WAKE`.
- `mremap(MREMAP_MAYMOVE | MREMAP_DONTUNMAP)` on anonymous and memfd (shmem)
  mappings to move the compacted spaces.
- SELinux: the `[userfaultfd]` anon inode is labelled through the
  `anon_inode` class (`type_transition ... anon_inode ... "[userfaultfd]"`),
  which the Android 17 policy uses for `userfaultfd` access.
- This list is from ART's userfaultfd GC as I know it (not re-verified against
  the Android 17 sources in this session); ART probes the features through
  `UFFDIO_API`, so a missing feature should just make it fall back to the
  previous collector. Any kernel-version gate inside ART is outside this tree.

### Backported (apollo commit -> upstream)

Prerequisites: `4bc945da561c` spin_is_locked -> lockdep, `ca5324599684`
refcount_t for `userfaultfd_ctx`.

Security knobs: `df0ac3860adc` `vm.unprivileged_userfaultfd` sysctl,
`59af43d7faaa` `UFFD_USER_MODE_ONLY`, `00f3652d3dfb` user-mode-only mode of the
sysctl. **Default is 0**: unprivileged callers (no `CAP_SYS_PTRACE`) must pass
`UFFD_USER_MODE_ONLY`; kernel-mode faults (copy_to_user into a registered
range) then fail with `SIGBUS`/`EFAULT`. Set `vm.unprivileged_userfaultfd=1`
only if something needs kernel-mode faults without the capability. ART uses the
user-mode-only flag, so the default needs no change.

Anon-inode / SELinux: `5d65d87742d7` `inode_init_security_anon` LSM hook,
`84bd2fa085a5` `anon_inode_getfd_secure()`, `bfd00dd472bc` SELinux `anon_inode`
class + hook, `0fd5ebfb087e` userfaultfd uses secure anon inodes,
`1d6a8497dbcc` userfaultfd opened `O_RDONLY`. 4.14 creates the file before
installing the fd, so `anon_inode_getfile_secure()` is added next to the
`getfd` variant. `anon_inode` is appended to `secclass_map` after `bpf`; the
class is resolved by name so the policy needs no rebuild beyond defining it.

`MREMAP_DONTUNMAP`: `4dec884665e0`, `7ec3a44fbb0f`, `404d1ff14d0c`,
`28f11799cf34`, `8f3d04cfe243` (extend to non-anonymous mappings),
`2171399ae5f5`, plus the tools header sync `69fcbdde5955`. `364b0e513fc3` and
its revert `d099eb5382f1` cancel out and are not carried.

Minor faults (`UFFD_FEATURE_MINOR_SHMEM`, `UFFDIO_REGISTER_MODE_MINOR`,
`UFFDIO_CONTINUE`): `c3243db59561`, `b7f87ffdc54c`, `fe68331da8b1`,
`b9580527e1a1`, `ed11b43e5519`, `6fb4f0e03c87`, `a34b836062ff`,
`27212cde5e9c`, `aba2e030c769`. The apollo revert pair `c102818fcc79` /
`90042bb49a49` (and the page-leak fixes it reverts) nets to zero and is skipped.
`VM_UFFD_MINOR` is VM flag bit 37 (`HAVE_ARCH_USERFAULTFD_MINOR`, selected by
arm64 when `USERFAULTFD=y`); `UFFDIO_CONTINUE` is `_IOWR(0xAA, 0x07, ...)`.

### Not carried (and why)

- hugetlbfs minor faults (`16bc0c098816`, `bb1ebf05e3a1`, `46e766e0815a`,
  `6f17989f222a`, `fb69a9bf080a`, `bf642b62dac9` and the hugetlb hunks): hugetlbfs
  is off in this config and ART does not use it. `UFFD_FEATURE_MINOR_HUGETLBFS`
  is defined (ABI) but `UFFDIO_API` never reports it.
- The `FAULT_FLAG_INTERRUPTIBLE` / multi-retry series (`fead958727d3`,
  `dbf1f2cb22ad`, `3fca93d8f696`, `5b0d75d47b32`, `c6d116ae07f8`): 4.14's
  `handle_userfault()` already returns to userland for user faults and only
  affects fatal-signal handling inside GUP; it touches every arch fault path.
- `UFFDIO_WRITEPROTECT` items (`21f43ebab390`, `b8b047bfee2b`, `c2a42553bf92`):
  4.14 has no uffd-wp ioctl; ART does not use it.
- `151be207dd5f` untag fix (4.14 has no untagged-pointer handling here),
  `0071d25c0000` KSM (off), `ae6bcc6fedcc` (`mmap_min_addr` loosening),
  `64ef1d029072` docs (different doc layout), tools selftests.
- 4.14 has no `mmap_changing` handling, so `UFFDIO_CONTINUE` does not return
  `-EAGAIN` for concurrent fork/remap events like 4.19 does (same as
  COPY/ZEROPAGE here).

### How to test on a device

1. `zcat /proc/config.gz | grep USERFAULTFD` -> `=y`;
   `cat /proc/sys/vm/unprivileged_userfaultfd` -> `0`.
2. `VmFlags` in `/proc/<pid>/smaps` shows `ui` for minor-registered shmem VMAs.
3. Run the kernel selftest (`tools/testing/selftests/vm/userfaultfd shmem 20 16`
   and the `mremap_dontunmap` test from a newer tree) or a small program:
   `userfaultfd(O_CLOEXEC|UFFD_USER_MODE_ONLY)`; `UFFDIO_API` with
   `UFFD_FEATURE_MINOR_SHMEM` must return it in `features`; register a memfd
   mapping in `MODE_MINOR`, touch it, resolve with `UFFDIO_CONTINUE`;
   `mremap(addr, len, len, MREMAP_MAYMOVE|MREMAP_DONTUNMAP)` on an anonymous
   and a memfd mapping must succeed and leave the old VMA mapped.
4. SELinux: with the Android 17 policy, `dmesg | grep avc.*anon_inode` should
   show no denials for `[userfaultfd]` from `app_zygote`/apps; on a policy that
   lacks the rule, the denial appears as `tclass=anon_inode`.
5. ART: on an Android 17 image with the userfaultfd GC enabled, check
   `logcat -s art` for the collector selected at boot (it logs the userfaultfd
   feature probe result); if the probe fails ART falls back to the previous
   collector, so a missing feature shows up as that fallback rather than a crash.


Reference: the Xiaomi apollo 4.19 kernel (`kernel_xiaomi_apollo`, HEAD
3b4b0017b71f) that boots Android 17. Config lives in
`arch/arm64/configs/watch_414_a17.fragment`; merge it after
`sprd_sharkle_defconfig` + `watch_414.fragment` (+ `watch_414_adf.fragment`),
then `olddefconfig`.

## EROFS + incfs (branch a17/erofs-incfs)

### Backported

| Commit subject | Source |
|---|---|
| locking/barriers: Introduce smp_cond_load_relaxed() and atomic_cond_read_relaxed() | upstream fcfdfe30e324 (v4.17), trimmed (no atomic_long_ variants) |
| include/linux/pagemap.h: introduce attach/detach_page_private | upstream (v5.8) via apollo 1d696af89e90, Guoqing Jiang |
| fs: add EROFS read-only filesystem (with LZ4 decompression) | apollo `fs/erofs` + `trace/events/erofs.h`, upstream v5.10 feature set |
| block: add bio_first_page_all() | upstream v4.13+ |
| fs-verity: add the fs/verity core | apollo `fs/verity`, uapi/fsverity.h (upstream v5.4 series + fixes) |
| stddef: Introduce sizeof_field() | upstream v5.4 |
| locking/rwsem: Add down_read_killable() | upstream 76f8507f7a64 (v4.16), asm-generic part only |
| ANDROID: Incremental fs: add the Incremental File System | apollo `fs/incfs` + uapi/incrementalfs.h |

EROFS and incfs are imported as final trees: 4.14 has neither, so the
47 (EROFS) / 183 (incfs) individual apollo commits cannot be applied one by
one. Authors are in apollo's `git log -- fs/erofs` / `fs/incfs`.

4.14 adaptations (everything else is verbatim):
- EROFS: own `workstn_lock` (4.14 radix trees have no `xa_lock`);
  `<linux/overflow.h>` for `struct_size()`.
- fs-verity: `mempool_create_kmalloc_pool()` (no `mempool_init*()`),
  `KMEM_CACHE()` (no usercopy whitelist), `inode_permission()`,
  `FIELD_SIZEOF()`.
- incfs: `__poll_t`/`EPOLL*` -> `unsigned int`/`POLL*`, `ksys_close` ->
  `sys_close`, `vm_fault_t` -> `int`, `ZSTD_DECOMPRESS`/`LZ4_DECOMPRESS`
  selects; one unused local dropped.
- UAPI added exactly as upstream: `FS_IOC_{ENABLE,MEASURE,READ_METADATA}_VERITY`
  (133/134/135), `struct fsverity_*`, `S_VERITY` 65536, `FS_VERITY_FL`,
  `STATX_ATTR_VERITY`, `EROFS_SUPER_MAGIC_V1`, `incrementalfs.h` ioctls.

### Config

```
CONFIG_EROFS_FS=y  _XATTR _POSIX_ACL _SECURITY _ZIP (LZ4)
CONFIG_FS_VERITY=y  CONFIG_FS_VERITY_BUILTIN_SIGNATURES=y
CONFIG_INCREMENTAL_FS=y
```

### What remains / limits

- No filesystem provides `fsverity_operations` (ext4/f2fs hooks not
  backported); fs-verity is only used through incfs. Android's
  `/data` fs-verity (APK verity on f2fs/ext4) therefore still fails with
  `ENOTTY`/`EOPNOTSUPP`.
- `EROFS_FS_PCPU_KTHREAD` (off) was not build-tested.
- `down_read_killable()` is only implemented for the asm-generic rwsem
  (arm64); other architectures would not build from this tree.
- The in-tree `lib/lz4` (v1.8.3 port) is kept, apollo uses lz4 v1.10.0;
  upstream EROFS ran on the old library until v5.19.
- The `.fs-verity` keyring needs the signing certificate loaded by
  userspace (init) for `BUILTIN_SIGNATURES` to accept v4 APK signatures.
- Not run on hardware yet; only compile-tested.

### Testing on a device

1. `zcat /proc/config.gz | grep -E 'EROFS|INCREMENTAL|FS_VERITY'`;
   `grep -E 'erofs|incremental' /proc/filesystems`.
2. EROFS: build an image on the host
   (`mkfs.erofs -zlz4hc img.erofs dir`, and one without `-z`), then
   `mount -t erofs -o ro,loop img.erofs /mnt`; compare `find`/`sha256sum`
   against the source dir, check `ls -Z`/`getfattr -d` for xattrs and
   `dmesg | grep erofs` for errors. Boot with `system`/`vendor` as EROFS.
3. incfs: `ls /sys/fs/incremental-fs/features` (expect `corefs`, `v2`,
   `zstd`, ...); `adb install --incremental app.apk` with an APK v4
   signature (needs the Android incremental service); or
   `mount -t incremental-fs` via the incfs selftests from the Android tree
   (`tools/testing/selftests/filesystems/incfs`).
4. Check `dmesg` for lockdep/KASAN reports while reading compressed
   EROFS files under memory pressure.


Reference: Xiaomi apollo 4.19 kernel (boots Android 17). Config fragment:
`arch/arm64/configs/watch_414_a17.fragment`.

## BPF (network BPF loader, netd)

### Result: Android 17 netbpfload refuses to run on 4.14

Source: AOSP `packages/modules/Connectivity`, `bpf/loader/NetBpfLoad.cpp`
(branch main), `doLoad()`. Read through a web summarizer, not a local AOSP
tree: re-check the exact lines against the Android 17 branch used for the
build before acting on them.

| Check | Message / action |
|---|---|
| `!isAtLeastKernelVersion(4, 9, 0)` | "Android S & T require kernel 4.9", return 1 |
| `isAtLeastU && !isAtLeastKernelVersion(4, 14, 0)` | "Android U requires kernel 4.14", return 1 |
| `isAtLeastV && !isAtLeastKernelVersion(4, 19, 0)` | "Android V requires kernel 4.19", return 1 |
| `isAtLeast25Q2 && !isAtLeastKernelVersion(5, 4, 0)` | "Android 25Q2 requires kernel 5.4", return 1 |
| `!isLtsKernel()` / `REQUIRE(4.19.236, 5.4.186, ...)` | log only, not fatal |

On 4.14 the V and 25Q2 checks fail: an unmodified Android 17 netbpfload exits
non-zero, and init treats a netbpfload failure as fatal (`reboot,bpfloader-failed`,
see the dev.to log below). This cannot be fixed in the kernel without lying about
`uname -r`/`LINUX_VERSION_CODE`; that was deliberately not done. Options:

1. Platform patch to NetBpfLoad (and the matching netd / ConnectivityService
   checks) lowering the minimum to 4.14 and handling absent programs/maps.
   The community "fuck-bpf" patch set does this for <=4.19 on Android 16 QPR2
   and reports 4.14 boots; it is third-party and must go through review.
2. Use a kernel of at least 5.4 (not feasible for this SoC BSP).

Other `doLoad()` gates the kernel does satisfy once JIT is on:
`/proc/sys/net/core/bpf_jit_enable` and `bpf_jit_kallsyms` must be writable
(ENOENT means `CONFIG_BPF_JIT`/`CONFIG_HAVE_EBPF_JIT` missing); BTF is only
loaded from 5.10; `unprivileged_bpf_disabled` failure is fatal only from 5.13.

### What the programs themselves need (netd.c in Connectivity main)

Programs carry per-kernel version ranges and the loader skips those outside
the range, so 4.14 is covered by fallbacks for the core accounting path:

- `cgroupskb/{ingress,egress}/stats$4_9` (KVER_NONE..4.19): cgroup_skb, hash
  and array maps; works on 4.14 (`BPF_PROG_TYPE_CGROUP_SKB`, present).
- `cgroupsock/inet_create$4_14` (4.14..5.10): `BPF_PROG_TYPE_CGROUP_SOCK`, present.
- `skfilter/*/xtbpf` (`CONFIG_NETFILTER_XT_MATCH_BPF=y`) and
  `schedact/ingress/account` (`CONFIG_NET_CLS_BPF=y`; `NET_ACT_BPF` is off, the
  program type is SCHED_CLS so check on device).
- Skipped on 4.14 by version range: `bind4/bind6`, `connect4/6`,
  `sendmsg/recvmsg4/6` (cgroup_sock_addr, 4.17 feature, loader min 4.19);
  `getsockopt/setsockopt` (cgroup_sockopt, 5.3); `cgroupsockrelease`, `sk_storage`,
  `packet_trace_ringbuf` (5.10); `local_net_*` LPM maps are 25Q2 and only matter
  with the programs above.
- Helpers used by the 4.9/4.14 variants (`get_socket_cookie`, `get_socket_uid`,
  `skb_load_bytes`, `get_current_uid_gid`) exist in 4.14. Missing on 4.14:
  `skb_load_bytes_relative` (4.18, only used by `$4_19`+ variants) and
  `ktime_get_boot_ns` (5.8, packet tracing).
- Attach: third-party reports say netd attaches with flags 0, so
  `BPF_F_ALLOW_MULTI` (4.15, `cgroup_bpf` rework) is not required; 4.14 only
  accepts `BPF_F_ALLOW_OVERRIDE`.

Not backported, deliberately: BPF_MAP_TYPE_RINGBUF (5.8; adds a new map type,
mmap interface, new helpers and verifier reference tracking, on top of the
4.14 verifier that lacks most of what it builds on, only consumed by 5.10+
programs that the loader skips), SK_STORAGE (5.2/5.10), CGROUP_SOCK_ADDR (4.17),
CGROUP_SOCKOPT (5.3), BTF (4.18, loader uses it from 5.10), bounded loops.
apollo carries all of these as large UPSTREAM series (165 `FN()` helpers vs 55 here).

### arm64 JIT

`CONFIG_BPF_JIT=y` enabled (was off; `HAVE_EBPF_JIT` already selected).
apollo's arm64 JIT commits were checked one by one against
`arch/arm64/net/bpf_jit_comp.c`; none is a needed fix on 4.14:

- already present in the 4.14 JIT: `16338a9b3ac3` (tail call OOB), `a2284d912bfc`
  (stack depth with tail calls), `4423a82cbde3` (STADD with LSE), `1bfceb375034`
  (no prefetch in xadd), `c362b2f34e26` (JLT/JLE/JSLT/JSLE).
- not applicable: `e195b13dfe46` (bounded loops, 5.3), `0380e0457cac` (JMP32,
  5.1), `a7cdbca279b2` (exception tables, 5.11), `0211fdf84b40` /
  `790c4b643723` (subprog/line info, 4.16+/5.0), `ed39fd97593e` (BTI),
  `91cdb5b36234` (nospec insn, no such insn in the 4.14 verifier), `0fcca0defb5e`
  (atomics, 5.12), `efb90b93de44`/`c10baf76063b` (Android CFI).
- optimisations only, not taken: `877301fa8215`, `9316367d4752`, `6b317971fcbb`,
  `195f70f3643f`, `6d2eea6fb079`, `56ea6a8b4949`, `09ece3d0f2ee`.
- `6af9f48e1074` / `030d374eb947` (JIT image not in the module area): not needed
  on 4.14, JIT calls helpers via absolute 64-bit addresses.

`CONFIG_BPF_JIT_ALWAYS_ON` (apollo sets it) is left off: with it a JIT failure
means the program fails to load, and this JIT has not been exercised on the
watch yet. Consider it after testing.

The cgroup-BPF refcount issue reported on 4.9 (`cgroup_sk_clone` missing the
`cgroup_bpf` reference) does not apply: 4.14 has no per-cgroup percpu_ref for
bpf, so `cgroup_sk_clone()` is already consistent.

### Remaining

- Platform side: NetBpfLoad 4.19/5.4 minimum (above) is the blocker.
- Re-check the version gates against the actual Android 17 source tree.
- If cgroup_sock_addr hooks turn out to be required by Android 17 netd on this
  device, backport the 4.17 series (`4fbac77d2d09` and dependents); not done.

### Testing on a device

    zcat /proc/config.gz | grep BPF_JIT      # CONFIG_BPF_JIT=y
    cat /proc/sys/net/core/bpf_jit_enable    # exists; netbpfload sets 1
    cat /proc/sys/net/core/bpf_jit_kallsyms  # exists
    logcat -b all | grep -i NetBpfLoad       # which gate fires, skipped maps/progs
    ls /sys/fs/bpf/netd_shared               # pinned progs after a successful load
    dmesg | grep -i bpf

A kernel-side smoke test: run the kernel selftest `tools/testing/selftests/bpf/test_verifier`
(needs the 4.14 selftests) with `bpf_jit_enable=1`, or `CONFIG_TEST_BPF` as a module.

### Sources (web content treated as untrusted data)

- AOSP NetBpfLoad.cpp: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/heads/main/bpf/loader/NetBpfLoad.cpp
- AOSP netd.c: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/heads/main/bpf/progs/netd.c
- Android 17 on 4.9: https://dev.to/gde/android-17-on-a-49-kernel-lineageos-240-on-the-pixel-3a-1mok
- fuck-bpf: https://github.com/Doze-off/fuck-bpf
- AOSP eBPF traffic monitoring: https://source.android.com/docs/core/data/ebpf-traffic-monitor
- Local: apollo `git log -- arch/arm64/net kernel/bpf net/core/filter.c`.
