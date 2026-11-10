# Mali Midgard r21p0 (T820, "sharkle") for K18 and DM63

Stock `libGLES_mali.so` on these boards is Midgard r21p0-01rel0, GPU T820,
and refuses to initialize against a kbase driver whose UK (userspace/kernel)
ABI version doesn't match. The in-tree driver at `drivers/gpu/arm/midgard`
is r28p0 (newer kbase UK ABI, different ioctl set) and was never wired into
any defconfig, so no board built a `mali.ko` at all before this change.

## Source and provenance

- `drivers/gpu/arm/midgard_r21p0/` is ARM's GPL Midgard r21p0 kbase release,
  imported from `/home/blackbox/.cache/mali-r21p0/src/drivers/gpu/arm/midgard`
  (a third-party re-host of the ARM GPL tarball; treated as untrusted input
  and reviewed before use). `docs/`, `tests/` and the vexpress/devicetree
  platform directories we don't use were left out of the import commit;
  `ipa/` was initially left out too and then re-added (see below).
- `platform/sharkle/` started as a byte-for-byte copy of the sprd vendor
  glue already in this tree at
  `drivers/gpu/arm/midgard/mali/platform/sharkle/` (the unwired r28p0 copy),
  which targets the same SoC/DT. It needed adapting: r21p0's platform API
  predates a few r28p0-isms, and - more importantly - the actual k18/dm63
  device trees differ from what that file assumes (see below).
- The stock `mali.ko` pulled from the K18 boot ramdisk
  (`/home/blackbox/.cache/k18-inputs/ramdisk`, extracted read-only with
  `cpio --no-absolute-filenames` into a scratch dir, never executed) was
  used throughout as ground truth: `modinfo`, `strings`, and
  `aarch64-linux-gnu-objdump -t`/`-dr` (static disassembly only) against the
  prebuilt cross toolchain.

## What the stock binary told us

```
version:  r21p0-01rel0 (UK version 11.0)
vermagic: 4.4.83 SMP preempt mod_unload modversions aarch64
alias:    of:N*T*Csprd,mali-midgard*
parm:     gpu_always_on, gpu_qos_threshold_aw, gpu_qos_threshold_ar,
          mali_debug_level, gpu_upthreshold, gpu_downdifferential,
          gpu_pollingtime, gpu_freq_cur, gpu_voltage_cur, gpu_freq_list,
          gpu_boost_level, gpu_power_alawys_on
```

This confirms:
- **UK version 11.0** is what the paired `libGLES_mali.so` expects.
  `BASE_UK_VERSION_MAJOR`/`MINOR` in this tree's
  `mali_kbase_ioctl.h` are 11/0 - r21p0 is the right release.
- **Module name `mali`** (not upstream's default `mali_kbase`).
- **DT compatible `sprd,mali-midgard`** - matches the `gpu@60000000` node in
  both `arch/arm64/boot/dts/sprd/sp9832e-1h10-k18.dts` and
  `sp9832e-1h10-dm63.dts` (reconstructed from the stock DT table).
- **Module parameter set** matches a build with `CONFIG_MALI_DEVFREQ=y`
  (stock has `kbase_devfreq_init`/`kbase_devfreq_target`/etc. in its symbol
  table) and *not* `CONFIG_MALI_MIDGARD_DVFS` (no `gpu_freq_min_limit`/
  `gpu_freq_max_limit` params, which that option would add) - and *not*
  `CONFIG_DEVFREQ_THERMAL` (no IPA/cooling symbols at all). `CONFIG_MALI_BOOST`
  (an sprd-only Kconfig symbol, not present in the vanilla ARM Kconfig we
  imported) clearly *was* set in stock - `kbase_platform_set_boost` and
  `kbase_platform_modify_target_freq` are present and exported - but nothing
  in this tree calls either function, and no DT/HAL caller for it is
  visible from here. Left unconfigured (dead code in `platform/sharkle/`,
  compiles out). **Open item**: if some vendor HAL/thermal service expects
  to poke GPU boost via this module, it isn't wired up.

## platform/sharkle/ changes from the r28p0 copy

1. **GPU power-domain force-shutdown register**: the r28p0/ARM-reference
   code (and the generic `arch/arm64/boot/dts/sprd/sharkle.dtsi` `gpu@`
   node) gets this via `syscons = <&pmu_apb_controller 0x20 0x2000000>;
   syscon-names = "top_force_shutdown";` and
   `syscon_regmap_lookup_by_name()`/`syscon_get_args_by_name()`. Neither the
   k18 nor the dm63 *reconstructed stock* DTS carries a `syscons`/
   `syscon-names` property on `gpu@60000000` - only `sprd,syscon-aon-apb`
   and `sprd,syscon-pmu-apb` phandles - so the by-name lookup would return
   an error and crash the first power-on. Changed to
   `syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-pmu-apb")`
   with the reg/mask spelled out as constants (offset `0x20`, mask
   `0x3000000`, off-value `0x2000000`, on-value `0`). Corroborated three
   ways: (a) the generic sharkle.dtsi reference above uses the same
   offset/mask/off-value; (b) disassembling the stock k18 `mali.ko`'s
   `mali_platform_init` shows `of_parse_phandle` + `of_translate_address`
   + **`add x0, x0, #0x20`** + `__ioremap`, i.e. the same `+0x20` byte
   offset from the syscon base, right before a
   `syscon_regmap_lookup_by_phandle` call; (c) `strings` on the stock
   module shows both `sprd,syscon-aon-apb` and `sprd,syscon-pmu-apb` and
   `syscon_regmap_lookup_by_phandle` (not `_by_name`) as the only syscon
   lookup it uses. Residual uncertainty: the stock driver's power-on/off
   path is actually hand-rolled `ioremap()` + spinlocked read/modify/write,
   not `regmap_update_bits()`, and a second register write sequence visible
   in the same function touches different-looking bits (`and
   w0,w0,#0xff000000` / `orr w0,w0,#0x10101`) that could not be confidently
   attributed from static disassembly alone in the time available - it may
   be the QOS AR/AW threshold setup (`config_gpu_qos()`, called later in
   the same function with a clearly-matching 2-bit-field pattern at a
   *different*, hardcoded `0x60100014` address) or something else. This is
   flagged as an open item for hardware bring-up, not blocking: the value
   used here is the best-corroborated one and `regmap_update_bits()` against
   a syscon is the standard, safe way to do a masked register
   read-modify-write.
2. **Debug print macro**: r21p0 has no `KBASE_DEBUG_PRINT`; added to
   `mali_kbase_debug.h`, gated by a new `mali_debug_level` module parameter
   (declared in `mali_kbase_core_linux.c`, matching stock's name/default).
3. **Module parameters**: `mali_debug_level`, `gpu_upthreshold`,
   `gpu_downdifferential`, `gpu_pollingtime`, `gpu_freq_cur`,
   `gpu_voltage_cur`, `gpu_freq_list`, `gpu_boost_level`,
   `gpu_power_alawys_on` added to `mali_kbase_core_linux.c` (sprd's glue
   expects these as `extern`s). Names, types and permissions match stock's
   `modinfo` output exactly (verified side by side).
4. **`GPU_FREQ_KHZ_MAX`**: was `600000` (copied from a different board's
   OPP table); the k18/dm63 `operating-points` table tops out at `680000`
   kHz (`0xa6040` in the reconstructed DTS). Fixed; `GPU_FREQ_KHZ_MIN`
   (`256000`) was already correct.
5. Removed `POWER_MODEL_CALLBACKS`/`SECURE_CALLBACKS` from
   `mali_kbase_config_platform.h`: r21p0 kbase doesn't reference either
   macro (both are from later IPA/secure-mode-switch kbase releases); they
   were dead, misleading no-ops.
6. `platform/sharkle/Kbuild` built the file with `obj-y` instead of the
   module-object variable: harmless for an in-tree build where
   `obj-$(CONFIG_MALI_MIDGARD)` eventually reaches it, but for an
   out-of-tree (`M=`) build `obj-y` has no `built-in.o`/`vmlinux` target to
   attach to and silently drops the object, leaving
   `platform_sharkle_funcs`/`pm_sharkle_callbacks` undefined in the final
   `mali.ko`. Changed to append to the `mali-y` module-object list (same
   pattern `platform/devicetree/Kbuild` already used, just not yet renamed
   - see below).

## Driver-wide changes (not sharkle-specific)

- **Module renamed `mali_kbase` -> `mali`**: `obj-$(CONFIG_MALI_MIDGARD) +=
  mali.o` / `mali-y := ...` in `Kbuild`, propagated to every platform Kbuild
  that appended to the old `mali_kbase-y` variable name (devicetree,
  sharkle, the three vexpress dirs, `ipa/Kbuild`). Required: the ramdisk's
  init scripts `insmod mali.ko` by that literal name, and
  `tools/sprd_ramdisk.py replace-modules` matches on basename.
- **`ipa/` re-imported**: `mali_kbase.h` unconditionally `#include
  "ipa/mali_kbase_ipa.h"`, and `mali_kbase_core_linux.c` declares (though
  does not unconditionally *call*) `kbase_ipa_debugfs_init()` from
  `ipa/mali_kbase_ipa_debugfs.h` - both are hard compile errors without the
  directory present at all, so the original "leave it out, we don't need
  devfreq cooling" call in the import commit doesn't build. Re-imported
  verbatim from the same r21p0 source tree. Its `.c` files are gated in
  `ipa/Kbuild` behind `CONFIG_DEVFREQ_THERMAL` (not set here, matching
  stock - see above), so only the headers are actually used, and
  `mali_kbase_ipa.h`'s own `#else` branch reduces them to inert stubs.
- **`get_user_pages()` 4-vs-7-arg mismatch** in `mali_kbase_mem.c` and
  `mali_kbase_mem_linux.c`: the `#if LINUX_VERSION_CODE <
  KERNEL_VERSION(4, 6, 0)` branch assumes the pre-4.6 `(tsk, mm, start,
  nr_pages, write, force, pages, vmas)` signature, but this tree's 4.4.302
  already carries the `4.4.168+` stable backport of the `gup_flags` form
  (verified against `include/linux/mm.h`): same 7-argument shape as the
  4.6-4.9 case. Fixed both call sites to use `FOLL_WRITE` instead of the
  separate `write`/`force` args.
- **DT match table**: added `{ .compatible = "sprd,mali-midgard" }` to
  `kbase_dt_ids[]` in `mali_kbase_core_linux.c` (kept the two upstream
  generic entries too, harmless).

## Config wiring

New shared fragment `arch/arm64/configs/gpu_mali_sharkle.fragment`
(`CONFIG_MALI_MIDGARD=m`, `CONFIG_MALI_PLATFORM_NAME="sharkle"`,
`CONFIG_MALI_DEVFREQ=y` - the last one explicit rather than relied on as a
Kconfig default, since `default y` did not actually resolve to `y` through
this tree's defconfig-merge path when left unspecified), added to
`DEVFRAGS` for `k18` and `dm63` only in `scripts/watch_lib.sh`. Both
boards' `operating-points`/`sprd,dfs-*` DT properties and
`CONFIG_PM_DEVFREQ`/`CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND` were already on.
Regenerated `k18_defconfig`/`dm63_defconfig` with
`scripts/watch_defconfig.sh`; `scripts/watch_defconfig.sh --check all`
passes for every board (the other four boards are untouched - GPU presence
on them hasn't been checked the same way, see Open items).

## Build verification

Out-of-tree (`O=`), `ARCH=arm64`, the pinned
`gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu` cross compiler:

```
make O=$O k18_defconfig   # or dm63_defconfig
make O=$O modules_prepare
make O=$O M=drivers/gpu/arm/midgard_r21p0 modules
```

Both `k18_defconfig` and `dm63_defconfig` build `mali.ko` cleanly (zero
warnings from a touch-everything rebuild). Resulting `modinfo`:

```
version:  r21p0-01rel0 (UK version 11.0)      # matches stock exactly
vermagic: 4.4.302+ ... aarch64                # matches this kernel (not stock's 4.4.83 - expected for an out-of-tree rebuild)
alias:    of:N*T*Csprd,mali-midgard* (+ the 2 generic upstream aliases, harmless)
parm:     <identical name/type/description list to stock's modinfo, see above>
```

## Putting `mali.ko` in the ramdisk

Per `devices/README.md`'s ramdisk-module-swap section:

```
python3 -I tools/sprd_bootimg.py unpack boot.img dir/
python3 -I tools/sprd_ramdisk.py replace-modules dir/ramdisk new.gz --kernel Image --module mali.ko
python3 -I tools/sprd_bootimg.py pack dir/ new.img --kernel Image --ramdisk new.gz
```

Strip debug info first (`aarch64-linux-gnu-strip --strip-debug mali.ko`,
*not* `--strip-all`) or build with `INSTALL_MOD_STRIP=1`, same as the other
ramdisk modules.

## Security backports

Reviewed against the public kbase-security-fix list for releases around
r21p0 (a reviewer pass over the imported `mali_kbase_mem*.c`,
`mali_kbase_softjobs.c`, `mali_kbase_mmu.c`, `mali_kbase_core_linux.c`).
Scope here is the kbase core memory/MMU findings; the devfreq and
power-domain findings from the same review are tracked separately
(platform/backend, owned elsewhere). No ioctl numbers or userspace-visible
struct layouts changed by any of these - all are internal checks/bookkeeping
tightened or new capability gates; each rejects a bad argument combination
with an error rather than changing the ABI a correct caller sees.

| CVE | Applied? | Fix |
|---|---|---|
| CVE-2021-28663 (pending aliases bypass eviction protection) | Yes | `3c9e05b454f5` - `gpu_mappings` on an aliased native alloc is now taken for the alias's whole lifetime (from `kbase_mem_alias()` through `kbase_mem_kref_free()`), not only while actually GPU-mapped, closing the pending-cookie window where `MEM_FLAGS_CHANGE`/resize could evict/shrink a still-aliased allocation. |
| CVE-2021-28664 / CVE-2021-39793 / CVE-2022-22706 (missing CPU-write check in user-buffer GUP) | Yes | `0ec9fa205da1` - both `get_user_pages()` call sites (import-time and deferred job-time pinning) now request `FOLL_WRITE` from `(KBASE_REG_CPU_WR \| KBASE_REG_GPU_WR)`, not `GPU_WR` alone; the matching dirty-page bookkeeping on unmap was updated to match. |
| CVE-2022-20186 (also ARM CVE-2022-28348; alias size-check integer overflow) | Yes | `1d7f6c9a7f91` - `kbase_mem_alias()`'s `nents * stride` overflow check reordered to a division, and the per-entry `offset + length > nents` check changed to a subtraction form. |
| CVE-2022-38181 (JIT allocation freed while still active) | Yes | `9d6803c3b3f0` - new internal-only `KBASE_REG_ACTIVE_JIT_ALLOC` region flag, set/cleared under `reg_lock` across a JIT allocation's active lifetime; `MEM_FLAGS_CHANGE` now refuses to mark an active JIT region evictable, so only `kbase_jit_free()` can do so. |
| CVE-2022-36449 (imported user-buffer / Project Zero 2327 group) | Yes | `45269373e6fe` - CPU PTE mappings of an imported user buffer's GPU VA are invalidated with `unmap_mapping_range()` before the backing pages are unpinned, rather than rejecting the mmap outright. Checked first: `libGLES_mali.so` isn't in `/home/blackbox/.cache/k18-inputs`, but a copy exists at `.../tp/sfd_tool/extracted/vendor/lib*/egl/libGLES_mali.so`; `strings` on it was inconclusive for `BASE_MEM_IMPORT_TYPE_USER_BUFFER` (import-type constants are compiled-in enum values, not strings), but its build-config string shows the OpenCL `clImportMemoryARM`/`cl_arm_import_memory_host` path compiled in, so a real host-buffer USER_BUFFER import can't be ruled out - invalidate-before-unpin was used per the task's guidance for that case. `kbase_vmap_prot()` now refuses to map `KBASE_MEM_TYPE_IMPORTED_USER_BUF` regions at all, since its kref doesn't pin the GUP pages and none of its kernel-side callers (replay jobs, soft jobs, vinstr) have a legitimate reason to target one. |
| CVE-2023-4211 (splittable tracking VMA / stale `mm_struct`) | Yes | `5e9df698246f` - `kbase_tracking_page_setup()` now rejects any `BASE_MEM_MAP_TRACKING_HANDLE` mmap that isn't exactly one page, so the mapping can't be split and give rise to multiple independent `close()` callbacks racing the single `kctx->process_mm` reference. |
| MMU page-table dump physical-address disclosure | Yes | `1e3a294054df` - `BASE_MEM_MMU_DUMP_HANDLE` mmap now requires `capable(CAP_SYS_ADMIN)` before dumping raw GPU page-table physical addresses/PTEs to userspace. |
| Page-table reclamation race / `cleanup_empty_pte()` (CVE-2022-36449 group, issue 2333) | Latent, not reachable | `397adfd60b62` - documented only: the hazard is real but only reachable via the `MALI_2MB_ALLOC`/huge-page-insertion path, which is `default n` upstream and absent from both `k18_defconfig` and `dm63_defconfig`. Added a comment at `cleanup_empty_pte()` so enabling that option later isn't done without first fixing the free-before-flush ordering. |
| CVE-2022-33917 (CSF VMA-splitting) | No - doesn't apply | This import is Job Manager (JM), not Command Stream Frontend (CSF); there are no CSF queue mappings to split. No change. |
| CVE-2022-36449, CSF short-read overwrite | No - doesn't apply | The JM `BASE_JD_SET_EVENTS`/replay reader already checks `count` (`mali_kbase_core_linux.c`); this is a CSF-only code path not present here. No change. |

Owned-file findings from the review are all accounted for above. The
devfreq/power-domain findings (platform/sharkle glue) are out of scope for
this set of commits.

## Open items

- **Hardware bring-up is untested** (offline-only task: no device access).
  Everything above is static analysis (source review + stock-binary
  disassembly) plus a successful cross-build; nothing has run on real
  silicon.
- Power-domain force-shutdown register: see the residual-uncertainty note
  above (point 1) - the exact bit-level behavior of stock's hand-rolled
  ioremap path wasn't fully reconstructed from disassembly.
- `CONFIG_MALI_BOOST` (gpu_boost_level / modify_target_freq) is present in
  stock but not wired up here - no known caller in this tree.
- `MALI_GATOR_SUPPORT`/Streamline tracing (ARM's own Kconfig default `y`)
  did not resolve to `y` through this tree's defconfig-merge path either,
  same as `MALI_DEVFREQ`; left off since it isn't needed for DDK
  compatibility and stock doesn't clearly need it either.
- Only k18 and dm63 are wired up. mt107/mt108/dm82/dw100u have their own
  stock ramdisks/DTs that haven't been checked for a `gpu@` node or a
  stock `mali.ko` the same way.
