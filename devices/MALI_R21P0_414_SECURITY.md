# Mali r21p0 (4.14, built in): review of the r28p0-fork findings

A read-only review of the SPRD Mali Midgard r28p0 fork listed four issues and assumed the same code exists in
`drivers/gpu/arm/midgard_r21p0`. Each was checked against this tree.

| # | Finding | Status |
|---|---------|--------|
| 1 | JIT regions writable through `MEM_COMMIT` (GHSL-2023-005 pattern) | race not present; hardening `f7627ae3d536` (as on 4.4) |
| 2 | CVE-2023-6241: GPU fault worker grows a JIT region while `kbase_jit_grow()` has dropped its locks | not applicable: no `kbase_jit_grow()`, no lock drop |
| 3 | `kbase_jit_free()` trims before taking `reg_lock` | not applicable: `kbase_jit_free()` does not trim |
| 4 | CVE-2023-26083: `TLSTREAM_ACQUIRE` without a capability check | fixed in 1e84a2977793 |

Follow-up review (same tree, byte-identical to the 4.4 r21p0 files):

| # | Finding | Status |
|---|---------|--------|
| 5 | CVE-2023-4272: imported user buffers DMA-mapped partially (edge pages), GPU gets whole pages | fixed in 9fd0051a2cb9: every page mapped/unmapped whole (offset 0, `PAGE_SIZE`) in `kbase_jd_user_buf_map()` and `kbase_mem_from_user_buffer()`; partial-page imports still accepted |
| 6 | Failed pooled JIT growth: added pages not rolled back, region not made reclaimable again (own finding, not an ARM backport; ARM r44p0 still has it) | fixed in b37bff05896c: added pages freed when the GPU mapping grow fails (also in `kbase_mem_evictable_unmake()`); failure path calls `kbase_mem_evictable_make_jit()` under `reg_lock` as `kbase_jit_free()` does |
| 7 | JIT_ALLOC: `kbase_vmap()` of `gpu_alloc_addr` fails before `jit_alloc[id]` is set, region stuck active | fixed in 5e922f4d536e: region bound to its id on that path so the JIT_FREE userspace submits releases it |
| 8 | `cleanup_empty_pte()` frees the page-table page before the GPU invalidate (Project Zero 2333, CVE-2022-36449) | dormant, `CONFIG_MALI_2MB_ALLOC` is off in our configs; code unchanged, `#error` guard + Kconfig warning in b7760a114a70, must be fixed before enabling |
| - | CVE-2022-46395 | not affected: blocked by the existing `kbase_vmap_prot()` refusal of imported user buffers |
| - | CVE-2022-46891, CVE-2023-6143 | not assessed, no conclusion |

## 1-3: why they do not apply

This tree has the original r21p0 JIT, which predates JIT trimming, `kbase_jit_grow()` and the pool pre-allocation of
later releases.

- `kbase_jit_allocate()` takes `kbase_gpu_vm_lock()` (`reg_lock`) before `kbase_mem_evictable_unmake()` and holds it
  through the grow (`kbase_alloc_phy_pages_helper()` + `kbase_mem_grow_gpu_mapping()`) until the
  `KBASE_REG_ACTIVE_JIT_ALLOC` flag is set. Nothing drops `reg_lock` in between (the page allocator only takes pool
  locks), so there is no stale `old_size`/`delta`.
- `MEM_COMMIT` (`kbase_mem_commit()`) and the GPU page-fault worker (`kbase_mmu_page_fault_worker()`) both take
  `reg_lock` around their whole size read, allocation and mapping update, so they serialise with the JIT paths.
  A pooled region is `KBASE_REG_DONT_NEED`, which `kbase_mem_commit()` and the fault worker both refuse.
- `kbase_jit_free()` takes `reg_lock` first and only clears `ACTIVE_JIT_ALLOC` and moves the region to the pool
  (`kbase_mem_evictable_make_jit()`). There is no initial trim (no `kbase_mem_shrink_*`, no page release), so there
  is no window between CPU-mapping invalidation and page release.
- The race is not present, but `kbase_mem_commit()` now refuses `KBASE_REG_JIT_OWNED` regions (`f7627ae3d536`,
  the same hardening as the 4.4 r21p0, and what later ARM releases do): the kernel sizes JIT regions, userspace
  must not resize them, consistent with the MEM_FREE/MEM_FLAGS_CHANGE/MEM_ALIAS refusals.

## ABI notes

- `KBASE_IOCTL_TLSTREAM_ACQUIRE` returns `-EPERM` without `CAP_SYS_ADMIN`. Profiling tools that use the timeline
  stream (gator, the vendor timeline) need root. The normal graphics path does not use it.
- `MEM_COMMIT` on a JIT region fails with `-EINVAL`. Unverified against the stock blob until a GPU run on a watch.

## First boot: grep dmesg for "mali: refused"

Each refusal above logs one rate-limited line (the error code is unchanged):
`mali: refused <operation> from <comm> (tgid <n>)`. After the first boot run
`dmesg | grep 'mali: refused'`; any hit names the vendor library call that
depends on a refused operation. The operations:

- `TLSTREAM_ACQUIRE without CAP_SYS_ADMIN`
- `MMU page-table dump mmap without CAP_SYS_ADMIN`
- `tracking page mmap larger than one page`
- `MEM_COMMIT on a JIT region`
- `MEM_COMMIT under a kernel mapping`
- `MEM_FLAGS_CHANGE on a JIT region`
- `MEM_FLAGS_CHANGE under a kernel mapping`
- `MEM_ALIAS of a JIT region`
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

