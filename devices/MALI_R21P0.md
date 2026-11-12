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
   `0x3000000`, off-value `0x2000000`, on-value `0`).

   Note the mask is **not** the same value as the generic sharkle.dtsi
   reference above, which uses `0x2000000` - a single bit. Per
   `include/linux/mfd/syscon/sprd/sharkle/pmu_apb.h`, offset `0x20` is
   `PD_GPU_TOP_CFG`, where bit 25 is a hard force-shutdown and bit 24 is
   automatic shutdown - two independent bits, not one 2-bit field. The
   generic DTS only ever touches the force-shutdown bit; this port's mask
   (`0x3000000`) touches both, so power-on here also disables automatic
   shutdown and leaves the domain fully software-managed. That is a
   deliberate difference, not a transcription of the generic DTS value - see
   the comment in `mali_kbase_config_sharkle.c` above `GPU_PWR_DOMAIN_MASK`.
   Corroborated three ways: (a) disassembling the stock k18 `mali.ko`'s
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
| CVE-2022-38181 (JIT allocation freed while still active) | Yes | `9d6803c3b3f0`, follow-up `13b61199b5b3` - new internal-only `KBASE_REG_ACTIVE_JIT_ALLOC` region flag, set/cleared under `reg_lock` across a JIT allocation's active lifetime; `MEM_FLAGS_CHANGE` now refuses to mark an active JIT region evictable, so only `kbase_jit_free()` can do so. A reviewer pass found the flag was still being set *after* `reg_lock` was dropped and re-acquired (pooled path) or after `kbase_mem_alloc()` had already published the region (fresh-allocation path), leaving the same publish-before-protect race the flag was meant to close; `13b61199b5b3` moves the flag set inside the critical section that makes the allocation non-evictable in both paths. |
| CVE-2022-36449 (imported user-buffer / Project Zero 2327 group) | Yes | `45269373e6fe`, superseded by `b53e25f60940` - the first pass invalidated CPU PTE mappings of an imported user buffer's GPU VA with `unmap_mapping_range()` before the backing pages were unpinned, but a reviewer pass found this only covered one unpin path (`kbase_unmap_external_resource()`'s normal teardown); `kbase_gpu_munmap()`'s `PINNED_ON_IMPORT` teardown and the failed-import rollback (which passes a `NULL` reg) unpinned the same pages without going through it. `b53e25f60940` instead rejects a direct CPU mmap of `KBASE_MEM_TYPE_IMPORTED_USER_BUF` regions in `kbase_mmap()`. A third review found that the cookie mmap reserving a SAME_VA import still reached `kbase_cpu_mmap()`; that mapping cannot be refused (it reserves the GPU address), so `kbase_cpu_mmap()` now maps no pages for such a buffer and `kbase_cpu_vm_fault()` returns `SIGBUS` for it. No CPU PTE of an imported user buffer can exist, which covers every unpin path. The importer already has its host mapping. `kbase_vmap_prot()` still refuses to map the same region type for its own, independent reason (its kref doesn't pin the GUP pages). |
| User-buffer import failure: double free / unbalanced `mm_count` (same CVE-2022-36449 area, found in review) | Yes | `1697359fa01f` - `kbase_mem_from_user_buffer()`'s `fault_mismatch` path freed `user_buf->pages` and then `kbase_mem_kref_free()` freed it again once the caller's `kbase_mem_phy_alloc_put()` dropped the last reference; fixed by giving the array exactly one owner (the kref-free path). Separately, `user_buf->mm` was assigned before `get_user_pages()` but its `mm_count` reference was only taken after success, while `kbase_mem_kref_free()` unconditionally `mmdrop()`s whenever `user_buf->mm` is non-NULL - a failed import dropped a reference it never took; fixed by taking the reference at the same point `user_buf->mm` is assigned. |
| CVE-2023-4211 (splittable tracking VMA / stale `mm_struct`) | Yes | `5e9df698246f`, follow-up `1abf1905df8d` - `kbase_tracking_page_setup()` rejects any `BASE_MEM_MAP_TRACKING_HANDLE` mmap that isn't exactly one page, closing the mprotect/munmap split vector. A reviewer pass found a second vector this didn't cover: this kernel's `vma_to_resize()` permits an equal-size `mremap()` move before its `VM_DONTEXPAND` check, and `move_vma()` duplicates the tracking VMA via `.open()`/`.close()` without going through `kbase_tracking_page_setup()` at all, so the old unconditional drain-on-every-close let a stale moved VMA outlive a `process_mm` that had already been handed to a different context. `1abf1905df8d` adds a `process_mm_vm_count` refcount driven by a new `.open()` callback, draining only on the last close, and holds an explicit `mm_count` reference across the drain. The completeness claim in the previous entry is corrected: single-page restriction alone was not sufficient. |
| MMU page-table dump physical-address disclosure | Yes | `1e3a294054df` - `BASE_MEM_MMU_DUMP_HANDLE` mmap now requires `capable(CAP_SYS_ADMIN)` before dumping raw GPU page-table physical addresses/PTEs to userspace. |
| GHSL-2023-005 pattern (JIT region resized through `MEM_COMMIT`) | Yes (hardening) | `59cfac84bc5b` - `kbase_mem_commit()` refuses `KBASE_REG_JIT_OWNED` regions under `reg_lock`. The race described for `kbase_jit_grow()` (stale sizes across a dropped `reg_lock`) does not exist in this tree: `kbase_jit_allocate()` keeps `reg_lock` from `evictable_unmake()` through the grow, and `MEM_COMMIT` takes the same lock; the missing check was still a way to resize a kernel-owned region. |
| CVE-2023-6241 (GPU page-fault worker grows a reused JIT region while it is being re-armed) | No - doesn't apply | There is no `kbase_jit_grow()` and no lock drop: `kbase_jit_allocate()` holds `reg_lock` while it reads `nents` and calls `kbase_alloc_phy_pages_helper()`/`kbase_mem_grow_gpu_mapping()`, and `page_fault_worker()` takes `reg_lock` before reading the backing size, grows, and releases it only after the MMU insert. It also skips `DONT_NEED` regions. Size and delta therefore cannot go stale. No change. |
| `kbase_jit_free()` trims before taking `reg_lock` | No - doesn't apply | This tree's `kbase_jit_free()` has no trim step (no `kbase_mem_shrink_*`/`kbase_free_phy_pages_helper()` call); it takes `reg_lock`, clears `ACTIVE_JIT_ALLOC` and makes the region evictable and pooled in one `jit_evict_lock` section. No change. If a trim is ever added it must run under `reg_lock`. |
| CVE-2023-26083 (`TLSTREAM_ACQUIRE` leaks kernel pointers) | Yes | `fc02d270850b` - `kbase_api_tlstream_acquire()` returns `-EPERM` unless `capable(CAP_SYS_ADMIN)`. |
| CVE-2023-4272 (imported user buffer: partial-page DMA mapping, stale reads on non-coherent systems) | Yes | `5caa2e346725` - `kbase_jd_user_buf_map()` and the import-time pinning in `kbase_mem_from_user_buffer()` DMA-map every page whole (offset 0, `PAGE_SIZE`), as the GPU gets whole-page mappings; `kbase_jd_user_buf_unmap()` unmaps `PAGE_SIZE`, the rollback paths already did. Partial-page imports are still accepted (no ABI change). |
| Failed pooled JIT growth leaves unmapped backing / region not reclaimable (own finding, not an ARM backport; ARM r44p0 still has it) | Yes | `ccbddfa379af` - when `kbase_mem_grow_gpu_mapping()` fails the added pages are freed again (gpu and cpu alloc), likewise in `kbase_mem_evictable_unmake()`; the failure path of `kbase_jit_allocate()` now calls `kbase_mem_evictable_make_jit()` under `reg_lock`, as `kbase_jit_free()` does, so pool membership and evictability are published together. |
| JIT_ALLOC result-address map failure leaves the region unbound (own finding) | Yes | `c4f79fa087cd` - `kbase_jit_allocate_process()` binds `kctx->jit_alloc[id]` before returning `JOB_INVALID` when `kbase_vmap()` of `gpu_alloc_addr` fails, so the JIT_FREE userspace submits anyway releases the region instead of it staying active until context teardown. |
| CVE-2022-46395 (`kbase_vmap` of imported user buffers) | Not affected | The existing `kbase_vmap_prot()` refusal of `KBASE_MEM_TYPE_IMPORTED_USER_BUF` regions blocks it. |
| CVE-2022-46891, CVE-2023-6143 | Not assessed | No conclusion. |
| Page-table reclamation race / `cleanup_empty_pte()` (CVE-2022-36449 group, issue 2333) | Latent, not reachable | `397adfd60b62` - documented only: the hazard is real but only reachable via the `MALI_2MB_ALLOC`/huge-page-insertion path, which is `default n` upstream and absent from both `k18_defconfig` and `dm63_defconfig`. Added a comment at `cleanup_empty_pte()` so enabling that option later isn't done without first fixing the free-before-flush ordering; `8eebe0ab3268` adds a build-time `#error` (and a Kconfig warning) so `CONFIG_MALI_2MB_ALLOC` cannot be enabled until that is fixed. |
| CVE-2022-33917 (CSF VMA-splitting) | No - doesn't apply | This import is Job Manager (JM), not Command Stream Frontend (CSF); there are no CSF queue mappings to split. No change. |
| CVE-2022-36449, CSF short-read overwrite | No - doesn't apply | The JM `BASE_JD_SET_EVENTS`/replay reader already checks `count` (`mali_kbase_core_linux.c`); this is a CSF-only code path not present here. No change. |

ABI notes for the JIT/timeline rows: `MEM_COMMIT` on a JIT region now fails with `-EINVAL` (JIT backing is
managed only by `JIT_ALLOC`/`JIT_FREE`, grow on fault and the shrinker); `TLSTREAM_ACQUIRE` needs `CAP_SYS_ADMIN`, so
profiling tools (gator, the vendor timeline) must run as root.

Owned-file findings from the review are all accounted for above. The
devfreq/power-domain findings (platform/sharkle glue) are out of scope for
this set of commits.

### Later review rounds (3 to 10)

Ten review rounds (gpt-6.1-sol) in all. Rounds 3 to 10 found incomplete fixes and further bugs inherited from
r21p0 in the JIT, eviction and mapping code; all of these are fixed:
- Imported user buffers: the cookie mmap that reserves a SAME_VA import maps no pages, and CPU faults on such a
  mapping return SIGBUS (completes the CVE-2022-36449 fix).
- JIT ownership: `KBASE_REG_JIT_OWNED`, set at creation and never cleared; user `MEM_FREE`, `MEM_FLAGS_CHANGE`,
  `MEM_ALIAS` (as a source) and `kbase_vmap_prot()` refuse JIT regions.
- Eviction: no CPU mmap of `DONT_NEED` regions; reclaim accounting and `DONT_NEED` are set before an allocation is
  published to the shrinker; `unmake()` and region teardown detach from `evict_list` under `jit_evict_lock` before
  clearing `alloc->reg`; `kbase_jit_free()` moves the region into the pool in the same `jit_evict_lock` section
  that makes it evictable; pooled regions are detached from both lists with `list_del_init()`; the shrinker is
  unregistered before JIT teardown.
- Kernel mappings: `alloc->kernel_mappings` (from `kbase_vmap_prot()`) blocks `DONT_NEED` and `MEM_COMMIT` while a
  soft job holds a mapping.
- CPU mappings keep the region's `start_pfn` and `nr_pages`, so faults and `MEM_FIND_CPU_OFFSET` never read a freed
  region.
- Soft-job debug copy: zero, wrapping and oversized buffers are rejected before the page array and GUP.
- The debugfs memory dump takes a page reference under `jit_evict_lock`, dropped before `vmap()`.

Known, not fixed (not reachable on these watches, or root-only):
- Replay (`mali_kbase_replay.c`): FBD hierarchy masks can index past the 8 weight entries, and a cyclic job list can
  loop forever under `jctx.lock`. Replay is disabled on the T820 by its hardware-issue gate.
- Context debugfs files (`atoms`, JIT statistics, ...) keep an unreferenced `kctx` after the context is destroyed;
  needs `CONFIG_DEBUG_FS` and root.

### First boot: grep dmesg for "mali: refused"

Each refusal above logs one line (the error code is unchanged):
`mali: refused <operation> from <comm> (tgid <n>)`. The limit is the kernel
default for `dev_warn_ratelimited()`, 10 messages per 5 s per call site, not
once only, so a busy loop is still sampled. After the first boot run
`dmesg | grep 'mali: refused'`. A hit identifies the refused operation and the
task that asked for it; by itself it does not establish that the vendor
library depends on that operation (a probing or fuzzing process produces the
same line). The operations:

- `TLSTREAM_ACQUIRE without CAP_SYS_ADMIN`
- `MMU page-table dump mmap without CAP_SYS_ADMIN`
- `tracking page mmap larger than one page`
- `MEM_COMMIT on a JIT region`
- `MEM_COMMIT under a kernel mapping`
- `MEM_FLAGS_CHANGE on a JIT region`
- `MEM_FLAGS_CHANGE under a kernel mapping`
- `MEM_ALIAS of a JIT region`
- `MEM_ALIAS with stride * nents overflowing the address range`
- `MEM_ALIAS with an offset beyond the aliased allocation`
- `MEM_ALIAS with offset + length beyond the aliased allocation`
- `CPU mmap of an imported user buffer`
- `CPU mmap of an evictable region`
- `CPU fault on an imported user buffer`
- `kernel vmap of an imported user buffer`
- `kernel vmap of an evictable or JIT region`
- `debug copy of an empty or wrapping buffer`
- `debug copy of an oversized buffer`

For the kernel vmap lines the process is whichever one ran the soft job, which
may be a kernel worker rather than the GPU library. MEM_FREE on a JIT region
already logged `kbase_mem_free called on JIT memory` before this change.

## Platform fixes

Review findings against `backend/gpu/mali_kbase_devfreq.c` and
`platform/sharkle/` from the same pass that produced the security-backports
table above. Scope here is devfreq/platform glue; the kbase-core memory/MMU
findings from the same review are tracked separately (above).

| Finding | Fix |
|---|---|
| Sharkle devfreq integration broke other selectable platforms: the common backend unconditionally called `kbase_platform_get_init_freq()`/`kbase_platform_set_freq()`, which only `platform/sharkle/` implements, so `CONFIG_MALI_PLATFORM_NAME="devicetree"` (the Kconfig default) with `CONFIG_MALI_DEVFREQ=y` failed to link. | `backend/gpu/mali_kbase_devfreq.c` now falls back to the generic `kbdev->clock` (`clk_get_rate()`/`clk_set_rate()`) path, same as upstream. The sharkle-only calls are gated behind a new `MALI_PLATFORM_SHARKLE` macro, defined only by `platform/sharkle/Kbuild` (`ccflags-y += -DMALI_PLATFORM_SHARKLE`) so it's only visible when `CONFIG_MALI_PLATFORM_NAME="sharkle"` is actually the platform being built - k18 and dm63 are unaffected. Verified both ways: `k18_defconfig`/`dm63_defconfig` (sharkle path) and a throwaway `CONFIG_MALI_PLATFORM_NAME="devicetree"` + `CONFIG_MALI_DEVFREQ=y` config (generic path, not committed) both build `mali.ko` cleanly with `W=1`. |
| `mali_platform_term()` freed `gpu_dfs_ctx.gpu_clk_src` with `vfree()` directly, without `clk_put()` on its entries or on `gpu_clock`/`gpu_clock_i`: every unbind/rebind of the platform device leaked those clock references. | Teardown now calls `mali_freq_term()` (already used on the `mali_platform_init()` error path) instead of hand-rolling the free, so the clocks are `clk_put()` before the array is freed. No change to the power-off/clock-off sequencing or to any regulator/vddcore handling. |

Both fixes keep the stock `libGLES_mali.so` (r21p0, UK 11.0) ABI: neither
touches an ioctl or a userspace-visible struct layout.

## GPU voltage scaling

The stock `mali.ko` raises vddcore (the `gpu` supply) before switching to a faster OPP and lowers it after a slower
one: it imports `regulator_get_voltage` and carries the `upgrade_voltage` messages. The r21p0 glue has that code under
`CONFIG_ADJUST_VOLTAGE`, which nothing defined, so 680 MHz ran at 900 mV. The platform Kbuild now defines it
(stock table: 256-600 MHz at 900 mV, 680 MHz at 950 mV). On top of the stock code:

- At init the rail's real voltage is read and raised to the default OPP's if lower; a failed raise fails the probe.
- A failed clock rate/parent change keeps `freq_cur`, skips the voltage decrease and returns the error to devfreq.
- On clock-on, a failed restore of `freq_cur` falls back to the default OPP (TWPLL 384 MHz, the lowest voltage);
  a failed switch to the default parent fails clock-on.
- A failed voltage decrease leaves `cur_voltage` at the real, higher value, so the next lower transition retries.

Known, accepted (same ordering as stock, GPU idle, needs a failing or unusual regulator state):
- Probe enables the GPU clocks at the default 384 MHz before the regulator is acquired and checked.
- Clock-on enables the clocks before switching to the default parent: after devfreq lowered the OPP while the
  clocks were off, the old parent (up to 680 MHz) can run for about 300 us at the lower voltage.
- The legacy `CONFIG_MALI_MIDGARD_DVFS` path writes `freq_next` without the semaphore; the watches build
  `CONFIG_MALI_DEVFREQ`.

## GPU cap, thermal cooling and the boost hint

Three follow-ups to the devfreq wiring (untested on hardware).

**Cap edge case.** `kbase_devfreq_target()` clamps to `sprd,dfs-range-max`
with `DEVFREQ_FLAG_LEAST_UPPER_BOUND`, but 4.4's `devfreq_recommended_opp()`
falls back from the floor to the ceiling when no enabled OPP is at or below
the request (for example when thermal cooling disabled the low OPPs). The
chosen OPP is now checked against the cap; if above, a strict
`dev_pm_opp_find_freq_floor()` is tried and the request fails when there is
none.

**Thermal cooling (DM63, DW100U only).** The DT `gpu-thmzone` (trips 70 C and
85 C passive, 110 C critical; the cooling map acts at 85 C) binds
`gpu-cooling-devices/gpu-cooling0` (`sprd,mali-power-model`). The stock
kernel and the stock `mali.ko` contain no GPU cooling code at all (no
`devfreq_cooling`, no sprd gpu cooling, no IPA), and the stock DT zones equal
ours (only phandle numbering differs), so stock never throttled the GPU
thermally. Ours goes beyond stock: `gpu_thermal_sharkle.fragment` sets
`DEVFREQ_THERMAL` and `SPRD_GPU_COOLING_DEVICE` for dm63/dw100u, and r21p0
calls `create_gpu_cooling_device()` (as the old midgard driver does) instead
of registering against the GPU node. IPA still initialises, because the job
manager calls into it. Trip temperatures are unchanged. Check on a device:
`/sys/class/thermal/cooling_device*/type` should list the GPU device and
`gpu-thmzone` should show it bound. Revert the commit to return to stock
behaviour.

Why the power model stays: neither DT zone has a `governor` property (4.4's
`of-thermal` does not read one) and the merged config sets
`THERMAL_DEFAULT_GOV_POWER_ALLOCATOR=y` for every zone, so `gpu-thmzone` runs
power_allocator. That governor ignores cooling devices without power ops
(`-ENODEV` when none is an actor), so a plain state-based devfreq cooling
device would never throttle, and switching the zone to step_wise would mean
changing the default for the other zones. The power callbacks were fixed
instead: `devfreq_cooling` passes frequency in Hz and voltage in mV, so the
dynamic callback now converts Hz to MHz (it used Hz as MHz) and the static
callback takes mV (it divided by 1000 again, expecting uV). A zero dynamic
coefficient or voltage base (DM63 has `sprd,dynamic-cluster = <0 0 0>`) now
yields no power instead of dividing by zero. The constants themselves are
unchanged vendor values and unverified on hardware. Failure paths also
unwind: a partial registration unregisters the devices already registered,
and `kbase_devfreq_init()` calls `kbase_ipa_term()` when cooling
registration fails.

**Boost hint ioctl.** The stock GPU library issues `0x4004801d`
(`_IOW(0x80, 29, u32)`). Stock stores the value in the context and
`kbase_jd_submit()` passes it to `kbase_platform_set_boost()` on every submit,
which keeps the highest pending level in `gpu_boost_level`;
`kbase_platform_modify_target_freq()` consumes it. Ours (`CONFIG_MALI_BOOST`,
all watches): the ioctl accepts 0/5/7/9/10 and returns `-EINVAL` otherwise
(stock accepted any value); the level is stored per context
(`kctx->sprd_boost`, accessed with `WRITE_ONCE`/a single `READ_ONCE` in
submit) and applied at submit as in stock. 10 means the highest
list entry, 9/7/5 the `sprd,dfs-scene-extreme/medium/low` entries.
`kbase_devfreq_target()` takes the pending level once (it is cleared) and
uses its frequency as a floor for that request, then applies the
`sprd,dfs-range-max` cap, so the cap wins. Stock instead pinned min and max
to the boost frequency for that evaluation; a floor lets the governor go
higher on its own. The boost also ignores a user `max_freq` set through
devfreq sysfs; thermal limits still hold because cooling disables OPPs.
`kbase_platform_modify_target_freq()` is kept but unused.

## Open items

- **Hardware bring-up is untested** (offline-only task: no device access).
  Everything above is static analysis (source review + stock-binary
  disassembly) plus a successful cross-build; nothing has run on real
  silicon.
- Power-domain force-shutdown register: see the residual-uncertainty note
  above (point 1) - the exact bit-level behavior of stock's hand-rolled
  ioremap path wasn't fully reconstructed from disassembly.
- `CONFIG_MALI_BOOST` is wired up for the ioctl and the devfreq floor (see
  "GPU cap, thermal cooling and the boost hint"); `gpu_boost_level` is not
  yet confirmed to be what the stock library expects on a device.
- `MALI_GATOR_SUPPORT`/Streamline tracing (ARM's own Kconfig default `y`)
  did not resolve to `y` through this tree's defconfig-merge path either,
  same as `MALI_DEVFREQ`; left off since it isn't needed for DDK
  compatibility and stock doesn't clearly need it either.
- Only k18 and dm63 are wired up. mt107/mt108/dm82/dw100u have their own
  stock ramdisks/DTs that haven't been checked for a `gpu@` node or a
  stock `mali.ko` the same way.
