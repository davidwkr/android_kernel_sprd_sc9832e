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

Applied from the 4.4 stable versions (github.com/gregkh/linux, tags v4.4.182–v4.4.294). Each commit keeps the original author and its `commit <sha> upstream.` line. Hand-adapted ones say `[ backport: ... ]` in the message. **None is compile-tested yet** (no cross-compiler installed).

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

- [ ] Merge or cherry-pick from CIP `linux-4.4.y-st` (security-only, maintained into 2027) for fixes after February 2022, for example netfilter, `cls_route` (CVE-2022-2588), `af_packet`, USB gadget, TTY.
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
