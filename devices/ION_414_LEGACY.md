# ION legacy ABI compatibility (4.14)

## Problem

The stock Android 8.1 vendor userspace for this family of devices
(`libion.so`, `libmemion.so`, `gralloc.sp9832e.so`, the camera HAL stack, and
whatever links against them) was built against the pre-4.12 "legacy" ION
ioctl ABI: `ION_IOC_ALLOC` returns an opaque int *handle*, and
`ION_IOC_FREE`/`ION_IOC_MAP`/`ION_IOC_SHARE`/`ION_IOC_IMPORT`/`ION_IOC_SYNC`
operate on that handle (or convert it to/from a dma-buf fd). There is also a
vendor-specific `ION_IOC_CUSTOM` sub-command set (`ION_SPRD_CUSTOM_*`).

The `kernel-4.14` branch carries the post-rewrite ion driver
(`drivers/staging/android/ion/`), which dropped `ion_handle`/`ion_client`
entirely: `ION_IOC_ALLOC` now returns a dma-buf fd directly, and only
`ION_IOC_ALLOC`, `ION_IOC_HEAP_QUERY`, `ION_IOC_PHY` and `ION_IOC_VERSION`
exist. The handle-based ioctls, and the custom sub-command dispatch hook
that used to carry `ION_SPRD_CUSTOM_*`, are gone. The stock vendor binaries
cannot work against this driver unmodified.

## What was done

Branch `kernel-4.14-ion-legacy`, worktree
`/home/blackbox/Documents/repos/alt/kwt-414-ion` (from `kernel-4.14`).

Added a compatibility shim, `drivers/staging/android/ion/ion_legacy.{c,h}`,
wired into the existing `ion_fops` in `ion.c`:

* `.open`/`.release` now point at `ion_legacy_open()`/`ion_legacy_release()`,
  which allocate/free a small per-open-file table (`struct
  ion_legacy_client`: a mutex + an `idr`) in `filp->private_data`. This slot
  was unused before (`ion_fops` had no `.open`, so `misc_open()` never wrote
  to it), so this is safe to repurpose.
* `.unlocked_ioctl` now points at `ion_ioctl_dispatch()`, `.compat_ioctl` at
  `ion_compat_ioctl_dispatch()`. Both recognise the legacy ioctl numbers
  first and fall through to the existing `ion_ioctl()` (the native 4.14
  dispatch in `ion-ioctl.c`, unchanged) for anything else.
* A legacy "handle" is just the key used in that per-fd `idr`, with the
  value being a `struct dma_buf *` holding one reference (`ion_new_alloc()`'s
  return already owns a reference; `dma_buf_get()` from `ION_IOC_IMPORT`
  takes one explicitly). `ION_IOC_FREE` drops it with `dma_buf_put()`.
  `ION_IOC_MAP`/`ION_IOC_SHARE` install a *new* fd for the dma-buf behind an
  existing handle via `get_dma_buf()` + `dma_buf_fd()`, without consuming
  the handle.
* `ION_IOC_SYNC` and the `ION_SPRD_CUSTOM_MSYNC`/`INVALIDATE` sub-commands
  are handle-free: they resolve the fd straight to a `struct dma_buf *` via
  `dma_buf_get()` and call the public `dma_buf_begin_cpu_access()` /
  `dma_buf_end_cpu_access()` API (which ends up calling ion's own
  `ion_dma_buf_begin/end_cpu_access` ops) to flush/invalidate the backing
  `sg_table`.
* `ION_SPRD_CUSTOM_PHYS` reuses the existing `ion_phys(fd, &phys, &size)`
  (same function the native `ION_IOC_PHY` ioctl already calls), copying
  into/out of the legacy `struct ion_phys_data` layout
  (`fd_buffer`/`phys`/`size`), which is unchanged from the 4.4 tree and
  still present verbatim in `include/uapi/linux/sprd_ion.h` on this branch
  (only the `enum ION_SPRD_CUSTOM_CMD` dispatch values were removed
  upstream; they are restored, same numeric values, in `ion_legacy.c`).
* `ION_SPRD_CUSTOM_MAP_KERNEL`/`UNMAP_KERNEL` reuse the existing exported
  `sprd_ion_map_kernel()`/`sprd_ion_unmap_kernel()` helpers in
  `sprd/sprd_ion.c`. `MAP_KERNEL` maps, reads `buffer->size`, and
  immediately unmaps again before returning — the kernel VA is zeroed in
  the struct handed back to userspace (a kernel pointer is unusable there
  and would leak kernel layout; this matches the hardened behaviour already
  present in the 4.4 `security-backports` branch). `UNMAP_KERNEL`
  consequently has nothing persistent left to tear down and only validates
  the fd.
* `ION_SPRD_CUSTOM_FENCE_CREATE`/`FENCE_SIGNAL`/`FENCE_DUP` are **not**
  implemented: the 4.4 `security-backports` sprd driver itself no longer
  handles them (falls to `default: return -ENOTTY`), and the vendor binary
  scan (see below) found no caller for them, so the unmodified `-ENOTTY`
  default case in `ion_legacy.c` is already the correct, matching
  behaviour.
* 32-bit (compat) support: `ION_IOC_FREE`/`MAP`/`SHARE`/`IMPORT`/`SYNC` use
  only plain `int` fields (handle/fd), so their struct layout and full
  ioctl cmd encoding are identical in 32-bit and 64-bit — a 32-bit caller
  lands on the exact same cmd value and is handled by the same
  `ion_ioctl_dispatch()` code, no translation needed. `ION_IOC_ALLOC` and
  `ION_IOC_CUSTOM` (and the `PHYS`/`MSYNC`/`MAP_KERNEL` custom sub-structs)
  use `size_t`/`unsigned long`, which differ in width, so
  `ion_compat_ioctl_dispatch()` has dedicated `compat_*` struct variants and
  field-by-field copies for those, modelled on `compat_ion.c` /
  `compat_sprd_ion.c` from the 4.4 tree.
* `ion_dma_buf_is_ion()` was added to `ion.c` (checks `dmabuf->ops ==
  &dma_buf_ops`) so `ION_IOC_IMPORT` rejects a dma-buf fd that didn't
  actually come from this ion instance, matching the intent of the
  original `ion_import_dma_buf()`.

No existing 4.14 ioctl numbers were touched. There is no collision: the
native ABI only uses `_IOWR('I', {0,8,9,10}, ...)`, all with struct sizes
that differ from the legacy structs at the same `nr`, and the legacy ABI
additionally uses `nr` values 1/2/4/5/6/7 that the native ABI never touches.

## Verified ioctl numbers

Computed from the real 4.4-tree uapi headers with a throwaway host-compiled
probe (`_IOWR` macro expansion, not hand arithmetic), then cross-checked
against disassembly of the actual stock libraries:

| ioctl           | nr | 64-bit cmd   | 32-bit (compat) cmd |
|-----------------|----|--------------|----------------------|
| ALLOC           | 0  | `0xc0204900` | `0xc0144900`         |
| FREE            | 1  | `0xc0044901` | `0xc0044901`         |
| MAP             | 2  | `0xc0084902` | `0xc0084902`         |
| SHARE           | 4  | `0xc0084904` | `0xc0084904`         |
| IMPORT          | 5  | `0xc0084905` | `0xc0084905`         |
| CUSTOM          | 6  | `0xc0104906` | `0xc0084906`         |
| SYNC            | 7  | `0xc0084907` | `0xc0084907`         |

Sprd `ION_IOC_CUSTOM` sub-command values (the `cmd` field of
`struct ion_custom_data`, not an ioctl number): `PHYS=0`, `MSYNC=1`,
`FENCE_CREATE=2`, `FENCE_SIGNAL=3`, `FENCE_DUP=4`, `MAP_KERNEL=5`,
`UNMAP_KERNEL=6`, `INVALIDATE=7`.

## Verified against the stock libraries

Extracted read-only from the device images with `debugfs -R dump` (never
mounted, never modified) and disassembled with the aarch64/arm cross
`objdump` (both architectures are supported by the one aarch64 toolchain's
binutils build; the extracted files were never executed):

* **`libion.so`** (`system.bin`, both `/lib` 32-bit and `/lib64` 64-bit
  copies): disassembly confirms `ION_IOC_ALLOC`, `FREE`, `MAP`, `SHARE`,
  `IMPORT`, `SYNC` at exactly the cmd values above, in both ABIs
  (`ion_alloc`/`ion_alloc_fd`/`ion_free`/`ion_map`/`ion_share`/`ion_import`/
  `ion_sync_fd` symbols). No `ION_IOC_CUSTOM` use.
* **`libmemion.so`** (`vendor.bin`, 32- and 64-bit): uses `ALLOC`/`FREE`/
  `SHARE` plus heavy `ION_IOC_CUSTOM` use at exactly `0xc0104906` /
  `0xc0084906`. The `MemIon` class methods map 1:1 onto the sub-commands:
  `Get_phy_addr_from_ion` → `PHYS` (cmd=0), `Flush_ion_buffer` → `MSYNC`
  (cmd=1), `Invalid_ion_buffer` → `INVALIDATE` (cmd=7), `get_kaddr` →
  `MAP_KERNEL` (cmd=5), `free_kaddr` → `UNMAP_KERNEL` (cmd=6). No
  `FENCE_*` use found.
* **`gralloc.sp9832e.so`**: does not call ioctl() for ION itself; it
  imports `ion_alloc`/`ion_free`/`ion_share`/`ion_sync_fd`/`ion_open`/
  `ion_close` from `libion.so`. Covered transitively.
* **`camera.sp9832e.so`**: imports the `MemIon` symbols directly from
  `libmemion.so` (not `libion.so`). Covered transitively. The higher camera
  HAL layers (`camera.device@3.x-impl-sprd`, `camera.provider@2.4-impl-sprd`,
  `libcamera_client`) carry no direct ION symbols.
* **Mali (`libGLES_mali.so`)**: no ION strings/symbols at all; its `ioctl`
  import is for the Mali GPU driver, unrelated to `/dev/ion`.

This confirms the full legacy ioctl set plus the five sprd custom
sub-commands implemented here (`PHYS`/`MSYNC`/`INVALIDATE`/`MAP_KERNEL`/
`UNMAP_KERNEL`) is both necessary and sufficient for this vendor userspace;
`FENCE_*` was correctly left unimplemented.

## Build

```
scripts/kconfig/merge_config.sh -O $O -m arch/arm64/configs/sprd_sharkle_defconfig arch/arm64/configs/watch_414.fragment
make O=$O olddefconfig
make O=$O W=1 drivers/staging/android/ion/
make O=$O Image
```

Builds clean (no new warnings beyond pre-existing, unrelated W=1 warnings
in `ion.c`/`ion_system_heap.c`/`sprd_ion.c`), and links into a full
`Image`/`vmlinux`.

## Known limitations / simplifications

* `ION_IOC_IMPORT` does not dedupe: importing the same underlying dma-buf
  fd twice from the same client allocates two distinct legacy handles
  (each holding its own reference), rather than returning the same handle
  both times as the original per-client `ion_handle_lookup()` did. This is
  a behavioural difference from the historical ABI, but it is
  spec-compatible (handles are meant to be opaque) and none of the
  verified vendor libraries rely on handle identity across repeated
  imports.
* `ION_SPRD_CUSTOM_MAP_KERNEL`/`UNMAP_KERNEL` never leave a kernel mapping
  pinned across the two calls (see above); this matches the already
  neutered (kaddr-zeroing) 4.4 `security-backports` behaviour, not the
  original unhardened upstream ION.
* `ION_IOC_PHY` (the native 4.14 path) carries no uid/permission gate in
  this tree, unlike the 4.4 `security-backports` branch's
  `sprd_ion_phys_allowed()` check on `ION_SPRD_CUSTOM_PHYS`. The legacy
  shim added here does not add one either, since doing so would be
  inconsistent (the same capability is already reachable unrestricted via
  the native `ION_IOC_PHY` ioctl in this tree) and porting that hardening
  is a separate, pre-existing gap outside this task's scope.
