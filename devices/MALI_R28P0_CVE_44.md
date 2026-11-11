# Mali Midgard r28p0 (SPRD fork): security fixes ported from the r21p0 port

Branch `fixes/mali-r28p0-cve-44`, on top of `security-backports`.

The kernel module `mali.ko` for the sharkle watches is built from SPRD's
Midgard `r28p0-01rel0` fork at `drivers/gpu/arm/midgard/mali` (ARM r28p0, about
600 changed lines of SPRD changes, `platform/sharkle`), which matches the r28p0
GPU user space of the Android 10 (VNDK 29) vendor we target. The r21p0 port
(`drivers/mali-r21p0-cve2`, `fix/mali-r21p0-fences`) carries a larger set of
kbase fixes. This document records, for every one of them, whether the r28p0
fork needed it. The r28p0 code is close to r21p0 in these paths, so most
fixes were ported as logic (not as text) with one commit each; the commit
message names the r21p0 commit it follows.

All fixes are local ports, not Arm's patches. Nothing was run on a device
(offline task): only the build and static review below. Test with the vendor
user space (GPU rendering, JIT-using workloads, OpenCL if used) before
shipping.

## Earlier fixes already on `security-backports`

| Commit | Fix |
|---|---|
| `4d03f9a45ced` | CVE-2022-38181: `kbase_mem_flags_change()` refuses regions carrying `KBASE_REG_JIT` |
| `539427c567b5` | CVE-2023-4211: `process_mm` is taken in `kbase_open()` with an `mm_count` reference and dropped at context destroy; the tracking page no longer sets it |
| `f721742a8d33` | `get_user_pages()` gup_flags form on 4.4.168+ (build fix) |

## r21p0 fixes against the r28p0 fork

`ported` = new commit on this branch. `present` = r28p0 already does it (code
cited). `n.a.` = the bug cannot occur in r28p0 (reason given).

| r21p0 commit | Bug | r28p0 status |
|---|---|---|
| `0ec9fa205da1` | User-buffer GUP pinned read-only although CPU-writable (CVE-2021-28664 / CVE-2021-39793 / CVE-2022-22706 pattern) | ported in `7061fa7119d1`. Same `GPU_WR`-only test at both pinning sites and both dirty-page decisions. |
| `1d7f6c9a7f91` | `kbase_mem_alias()` size and offset+length overflow (CVE-2022-20186 / CVE-2022-28348) | ported in `0518ba0dced7` |
| `3c9e05b454f5` | Alias ownership counted only while GPU-mapped; pending cookie lets a native alloc be evicted (CVE-2021-28663) | ported in `e3a063a74af6` |
| `9d6803c3b3f0` | MEM_FLAGS_CHANGE makes an active JIT region evictable (CVE-2022-38181) | already fixed by `4d03f9a45ced`: `kbase_mem_flags_change()` refuses every `KBASE_REG_JIT` region, active or pooled, which is stronger than the r21p0 `KBASE_REG_ACTIVE_JIT_ALLOC` test. No extra flag needed. |
| `13b61199b5b3` | JIT flag set after the region was published | ported in `6e3fa2bbfe76`: r28p0 set `KBASE_REG_JIT` after `kbase_mem_alloc()` returned; it is now set before publishing. The pooled path was not affected (pooled regions keep the flag until destroyed). |
| `576b79e62797` | User MEM_FREE and flags change on JIT regions | present, with the gap closed by `6e3fa2bbfe76`: flags change by `4d03f9a45ced`; `kbase_mem_free_region()` returns -EINVAL for `KBASE_REG_JIT`, which covers MEM_FREE. The r21p0 never-cleared `KBASE_REG_JIT_OWNED` is not needed: r28p0 clears `KBASE_REG_JIT` only in the destroy/evict/term paths, under `reg_lock`, right before freeing. |
| `0aac92ce7d54` | JIT region usable as alias source | ported in `78e0dc34c8c5` |
| `73de9b90c252` | CPU mmap of evictable regions; debug mem view vs JIT reclaim | ported in `c70ef2257165` (the mem_view file is root-only debugfs) |
| `47f0f56fd0a5` | Eviction-list publish/detach vs shrinker | ported in `faab98316eab`: `kbase_mem_evictable_make()` accounting order and `kbase_free_alloced_region()` detach. Present: `kbase_mem_evictable_unmake()` already takes `jit_evict_lock`; `kbase_jit_free()` already makes the allocation evictable and moves the region into the pool in one `jit_evict_lock` section. |
| `c5790ab16e3f` | JIT/eviction teardown ordering | ported in `ec03cea0d83d`: `kbase_mem_free_region()` delists first; shrinker unregistered before `kbase_jit_term()`. n.a.: the `list_del_init()` changes, since r28p0 takes a pooled region off `jit_node` and `evict_list` in one section. |
| `f50473f61aa4` | Kernel mappings do not pin the backing; CPU fault uses a freed region | ported in `3187e159f3db` |
| `582ce2e94081` | CPU-offset lookup uses the region; debug-copy size bounds | ported in `7c05c7acafc8` |
| `45269373e6fe` | `kbase_vmap_prot()` on imported user buffers (CVE-2022-36449) | ported in `771798ad79b0` (the `unmap_mapping_range()` half was superseded by the next two on r21p0 and is not ported) |
| `b53e25f60940` | Direct CPU mmap of imported user buffers (CVE-2022-36449) | ported in `0a2e702e12d4` |
| `4877a688e637` | Cookie mmap path maps user-buffer pages (CVE-2022-36449) | ported in `6bdfe9df5428` |
| `1697359fa01f` | User-buffer import failure: double free, unbalanced `mm_count` | present: `kbase_mem_from_user_buffer()` takes the `mm_count` reference (`mmgrab()`) right after `user_buf->mm = current->mm`, and its `fault_mismatch` path does not free `user_buf->pages` (freed once in `kbase_mem_kref_free()`). |
| `5e9df698246f` | Multi-page tracking mmap can be split (CVE-2023-4211) | n.a.: after `539427c567b5` the tracking page sets no `process_mm` and has no close handler, and `kbase_tracking_page_setup()` already requires `vma_pages(vma) == 1`. |
| `1abf1905df8d` | Tracking VMA ownership across `mremap()` | n.a.: no `process_mm` drain or `vm_ops` on the tracking VMA exist (`539427c567b5`); the mm reference is held by the context. |
| `1e3a294054df` | MMU page-table dump readable by any client | ported in `55aad84ac22b` |
| `397adfd60b62` | `cleanup_empty_pte()` frees before TLB flush (comment only) | ported as the same comment in `88e9a891ca21`; the hazard exists in r28p0, reachable only with `CONFIG_MALI_2MB_ALLOC`, which the module build does not pass. |
| `17fe1a634c66` | Sharkle: regulator released twice | ported in `1e006fe3f61c` |
| `b210273f1144` | Sharkle: asserts instead of checks on power-up | ported in `0a78b93b064f` (r28p0 uses `syscon_regmap_lookup_by_name()`, so the check was adapted; also requires both syscon args) |
| `64ff72adfe82` | Sharkle: clocks not put on teardown | ported in `75800efd9593` |
| `6bfe544e853c` | devfreq breaks other platforms | ported in `6d476e19befe` (macro defined in the sharkle and pike2 Kbuilds, the two platforms that provide the functions) |
| `1b0f1de2dad0` | Power-domain mask comment | n.a.: r21p0 uses a phandle syscon; r28p0 reads `top_force_shutdown` by name and has no such comment. |
| `4b7ac8034153` | Wire sharkle `set_freq` into devfreq | n.a.: the fork already calls it (SPRD's own wiring). |
| `a3f9b5aa9ae2` (fences) | `dep_count` raised after `dma_fence_add_callback()` | present: `kbase_fence_add_callback()` does `atomic_inc(dep_count)` before the call and `atomic_dec()` on both failure paths. |
| `a92c8b0ce4fe` (fences) | Duplicate reservation drops the exclusive bit | ported in `9fc0e2324bd2` (dormant, see below). |

## Review findings (JIT and timeline stream)

| Finding | Status |
|---|---|
| GHSL-2023-005 pattern: `MEM_COMMIT` resizes JIT regions | confirmed, fixed in `4e0865415f51`: `kbase_mem_commit()` refuses `KBASE_REG_JIT` under `reg_lock`. |
| CVE-2023-6241: GPU page-fault worker grows a reused JIT region while `kbase_jit_grow()` has dropped its locks | confirmed (DONT_NEED is cleared first, grow-on-fault flags stay, and `old_size`/`delta` were cached across the pool grow), fixed in `e4e8dbaad23e`: the size is re-read after every lock-dropping pool grow and the calculation restarts; if the region already reached the commit nothing is allocated. |
| `kbase_jit_free()` trimmed before taking `reg_lock` | confirmed, fixed in `a9f8d5552539`: `reg_lock` is held from the size snapshot to the DONT_NEED transition. |
| CVE-2023-26083: `KBASE_IOCTL_TLSTREAM_ACQUIRE` without a capability check | confirmed, fixed in `87dd22dab752`: `CAP_SYS_ADMIN` required, otherwise `-EPERM`. |

ABI notes:

- `KBASE_IOCTL_MEM_COMMIT` on a JIT region now fails with `-EINVAL`. JIT sizes
  are only changed by the kernel through the JIT soft jobs.
- `KBASE_IOCTL_TLSTREAM_ACQUIRE` needs `CAP_SYS_ADMIN`: profiling tools (gator,
  the vendor's timeline) must run as root.

The other commits in the r21p0 range (imports, Kconfig help text, module
rename, docs) have no counterpart to port.

## Fence code is not built

`mali_kbase_dma_fence.c` is only compiled with `CONFIG_MALI_DMA_FENCE`, which
`scripts/watch_build.sh` does not pass; `mali_kbase_fence.c` is compiled with
`CONFIG_MALI_DMA_FENCE` or `CONFIG_SYNC_FILE` (Kbuild), and the dm63 kernel
config sets neither. The built `mali.ko` has no `kbase_dma_fence_*` symbols
and no object from `mali_kbase_fence.c`. The duplicate-reservation
fix is therefore dormant, as the fence fixes are on r21p0.

## Build

`scripts/watch_build.sh dm63 <outdir under ~/.cache>` builds the kernel and
the external module from this fork: `modinfo mali.ko` reports
`version: r28p0-01rel0 (UK version 11.13)` and
`vermagic: 4.4.302+ SMP preempt mod_unload modversions aarch64`. The object
code of `kbase_devfreq_target()` still calls `kbase_platform_set_freq()`.
`scripts/watch_defconfig.sh --check all` reports every defconfig up to date.
Only the sequential dm63 build was run; the generic (non-sharkle) devfreq
branch was not compiled.

## Not covered

- The public list of Midgard CVEs fixed after r28p0 was not checked, so
  there may be fixes that neither this port nor the r21p0 port carries.
  Nothing in the code or commit history here names any.
- `sprd,dfs-*` devicetree index parsing in `mali_freq_init()` is still not
  validated (as on r21p0); the devicetree is ours, not attacker input.
- `kbase_vmap_prot()` now refuses JIT regions and imported user buffers, and
  the CPU mmap of imported user buffers is refused. These follow r21p0 and
  assume the vendor user space does not need them; unverified on a device.
- Code outside the paths above (replay, vinstr, hwcnt, IPA, ...) was not
  reviewed.
