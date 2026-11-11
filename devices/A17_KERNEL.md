# Android 17 kernel requirements (4.14 watch kernel)

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
