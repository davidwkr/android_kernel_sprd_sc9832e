# Android 17 kernel features on the 4.14 watch kernel

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
