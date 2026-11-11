# Android 10-13 GSI on the 4.4 kernel

Opt-in kernel config for booting an Android 10-13 "ab" GSI on the 4.4.302 SPRD tree (with the
first-stage ramdisk done elsewhere). It is not part of any device config: the committed
`arch/arm64/configs/*_defconfig` and the default `scripts/watch_build.sh` output are the
stock-8.1 builds and do not change. **Untested**: nothing here was booted on a watch; it was
only checked that the options survive `olddefconfig` and that the kernel builds.

## Use

```
scripts/watch_build.sh --newandroid DEVICE [OUTDIR]      # or WATCH_EXTRA_FRAGS=watch_newandroid
```

The fragment is appended after the device fragments (`scripts/watch_lib.sh`), so it wins on any
overlap, and `watch_verify` checks every line took effect. The build goes to
`OUTDIR/DEVICE+watch_newandroid/` (objects in `OUTDIR/obj-DEVICE+watch_newandroid/`) so it never
overwrites a stock build. `scripts/watch_defconfig.sh` ignores `WATCH_EXTRA_FRAGS`: the
defconfigs always stay the stock composition.

## What it enables

| Option | Why |
|---|---|
| `CGROUP_BPF` | In this tree's `kernel/configs/android-base.config`. `netd`/`bpfloader` attach cgroup eBPF programs (`BPF_CGROUP_INET_INGRESS/EGRESS`) for per-UID traffic accounting and firewalling. The 4.4 backport is here (`kernel/bpf/cgroup.c`, `BPF_PROG_ATTACH`); `BPF_SYSCALL` and `BPF_JIT` were already on. |
| `CGROUP_SCHEDTUNE` | Android 10+ `libprocessgroup` (`cgroups.json`, task profiles) mounts the `schedtune` controller as `/dev/stune` for the top-app/foreground/background boost groups. Without it the mount and those profiles fail with log errors. `SCHED_TUNE` was already on. |

Everything else Android 10-13 needs on 4.4 (binder with `binder,hwbinder,vndbinder`, ashmem, ION,
sdcardfs, FUSE, FBE on ext4/f2fs, dm-verity with FEC, SELinux, seccomp-bpf, xt_qtaguid/quota2, memcg,
cpusets, `UID_SYS_STATS`, `CPU_FREQ_TIMES`) is already in the watch configs.

## Considered and left out

| Option | Why not |
|---|---|
| `KSU*` | KernelSU, present in the third-party reference kernel: a root backdoor, never adopted. |
| `SPRD_SIPC_STTY`, `SPRD_SIPC_GNSS_STTY`, `SPRD_AUDIO_SIPC` | The reference kernel's Android 10 vendor image needs them; our stock 8.1 vendor does not (all three are `not set` in every `devices/*/stock.config`) and the vendor partition is the stock one. Enable them only together with a vendor that expects those tty/audio sipc channels: `watch_vndk29`, see `devices/VNDK29_44.md`. |
| `PSI` | Not in 4.4 (upstream 4.20, Android backports start at 4.9). Android 10+ `lmkd` falls back to the kernel-LMK/memcg vmpressure path when `/proc/pressure/memory` is missing. |
| `NET_SCH_INGRESS`, `NET_SCH_CLSACT` | Only used by the `tc` BPF programs of the network BPF loader, which needs 4.9+ (see below); `CLSACT` does not exist in 4.4. |
| `NETFILTER_XT_MATCH_OWNER` | Conflicts with `xt_qtaguid` ("quota, tag, uid"), which netd uses on 4.4. |
| `DM_DEFAULT_KEY`, `INCREMENTAL_FS`, `ANDROID_BINDERFS`, `SYNC_FILE` | No Kconfig symbol in this tree (backports, not config). Metadata encryption and incfs are optional features of Android 11+, binderfs is optional (the `/dev/binder*` devices from `ANDROID_BINDER_DEVICES` are used), and the legacy `SYNC`/`SW_SYNC` fences are on. |

## Limits

- Android 10-13 on 4.4 is an upgrade-device case, not a supported GKI/launch kernel. Android's
  own kernel requirements start at 4.9 (Q), 4.14 (R), 4.19 (S) and 5.10 (T) for launch devices;
  this config only meets the retained Pie 4.4 base requirements (4.4.107+).
- Networking: Android 10-11 `netd` stays on xt_qtaguid below 4.9. Android 12-13 do not: AOSP
  removed the `mBpfEnabled` checks and the legacy socket-tagging fallback, so traffic-controller
  initialization tries the BPF setup and fails on 4.4. An Android 12-13 GSI needs legacy-kernel
  networking patches (phh/TrebleDroid carry one that tolerates the failure); `CGROUP_BPF=y` does
  not supply the missing ABI. Per-UID data accounting and firewalling on 12-13 are unverified
  until a patched GSI is identified and tested. [I: from review, not checked against source]
- Encryption is legacy fscrypt v1 (no v2 policies, no metadata encryption): keep `/data`
  unencrypted or v1-only. Overlayfs is off (no overlay remount workflows; boot unaffected).
  Ramdisks must be gzip (LZ4 decompression is off).
- **Android 14 does not work with this kernel.** Its network BPF loader (`netbpfload` /
  Mainline `NetBpfLoad`) and the connectivity module's programs require 4.14 (and map/program
  types 4.4 lacks, e.g. `LRU_HASH`, `CGROUP_SOCK_ADDR`), so the network stack cannot start. Staying
  on Android 13 or lower is the ceiling for 4.4.
- Untested on hardware; the first-stage ramdisk, `fstab`/AVB and vendor interface (VNDK 28 vs
  29+) are outside this fragment.
