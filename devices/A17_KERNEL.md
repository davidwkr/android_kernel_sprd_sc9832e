# Android 17 kernel features on the 4.14 watch kernel

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
