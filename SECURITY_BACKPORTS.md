# Security backports

This tree is Linux **4.4.147** (August 2018) with Android 10 / UNISOC changes on top. The 4.4 LTS line ended at **4.4.302** (February 2022), and the Civil Infrastructure Platform (CIP) keeps a security-only 4.4 branch after that. Everything fixed upstream after 4.4.147 is missing unless the vendor or this fork picked it individually.

The watches' stock state, for comparison: kernel 4.4.83, Android security patch level **2019-11-05**, Mali GPU driver **r21p0** (kernel and userspace).

## Spot checks in this tree (2026-10-29)

Grep-level checks for well-known fixes. They indicate, they don't prove: a fix can be present in another form.

| Issue | Impact | In this tree? |
|---|---|---|
| CVE-2019-2215, binder UAF ("Bad Binder", exploited in the wild) | local root from any app | **looks fixed** (`POLLFREE` handling in `binder.c`) |
| CVE-2019-11477/11478/11479, TCP SACK panic / slowness | **remote** DoS over any network | **missing** (no `TCP_MIN_SND_MSS`) |
| CVE-2021-0920, `af_unix` garbage collector UAF (exploited in the wild on Android) | local root from any app | **missing** (no `unix_gc_lock` sync in `unix_peek_fds`) |
| CVE-2021-33909, `seq_file` size overflow ("Sequoia") | local root | **missing** (no `MAX_RW_COUNT` check) |
| CVE-2021-22555, netfilter `x_tables` heap write | local root (needs user namespaces) | looks fixed |
| CVE-2020-0466 / CVE-2021-1048, epoll UAF (1048 exploited in the wild) | local root | unclear, check by hand |
| Meltdown (KPTI) | – | present (`UNMAP_KERNEL_AT_EL0`) |
| Spectre v2 / SSBD | – | absent, **not needed**: Cortex-A53 is not affected by variants 2, 3 and 4 |

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

Check and enable where the 4.4 tree supports it:
- [ ] `CONFIG_HARDENED_USERCOPY` (backported in `android-4.4`)
- [ ] `CONFIG_CC_STACKPROTECTOR_STRONG`
- [ ] `CONFIG_DEBUG_RODATA` / strict kernel RWX
- [ ] `CONFIG_ARM64_SW_TTBR0_PAN` (PAN emulation; A53 has no hardware PAN)
- [ ] `CONFIG_RANDOMIZE_BASE` (KASLR, if the bootloader passes a seed)
- [ ] `CONFIG_SECURITY_PERF_EVENTS_RESTRICT`, `kptr_restrict`, `dmesg_restrict`
- [ ] Disable `CONFIG_USER_NS` unless needed (blocks a whole class of netfilter exploits)
- [ ] Disable `DEBUG_FS` in release builds (it's needed for development measurements now)

SELinux must stay **enforcing** in builds that go to users. The DM63 unit currently runs Permissive.

## 7. Userspace (not this repo, for completeness)

The Android userspace on the watches is 8.1 at patch level 2019-11-05. A patched kernel doesn't fix userspace bugs: media framework, Bluetooth stack, Wi-Fi supplicant, WebView. Track that separately.

## Order of work

1. First booting build (no backports), so regressions can be told apart from bring-up problems.
2. Mali fixes (smallest change, highest risk reduction).
3. TCP SACK, `af_unix` GC, `seq_file`, epoll: small, targeted cherry-picks.
4. Full stable merge (`android-4.4-q`), in batches with boot tests.
5. CIP post-302 fixes, UNISOC bulletin fixes.
6. Hardening config.
