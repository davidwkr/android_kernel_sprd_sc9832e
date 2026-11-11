# Mali r21p0 (4.14, built in): review of the r28p0-fork findings

A read-only review of the SPRD Mali Midgard r28p0 fork listed four issues and assumed the same code exists in
`drivers/gpu/arm/midgard_r21p0`. Each was checked against this tree.

| # | Finding | Status |
|---|---------|--------|
| 1 | JIT regions writable through `MEM_COMMIT` (GHSL-2023-005 pattern) | race not present; hardening `f7627ae3d536` (as on 4.4) |
| 2 | CVE-2023-6241: GPU fault worker grows a JIT region while `kbase_jit_grow()` has dropped its locks | not applicable: no `kbase_jit_grow()`, no lock drop |
| 3 | `kbase_jit_free()` trims before taking `reg_lock` | not applicable: `kbase_jit_free()` does not trim |
| 4 | CVE-2023-26083: `TLSTREAM_ACQUIRE` without a capability check | fixed in 1e84a2977793 |

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
