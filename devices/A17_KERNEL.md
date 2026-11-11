# Android 17 kernel features on the 4.14 watch kernel

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
