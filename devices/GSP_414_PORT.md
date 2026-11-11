# GSP (2D blender) on 4.14

**Compile-only. Nothing here has run on a watch.**

The stock Android 8.1 hwcomposer (`hwcomposer.sp9832e.so` through `gsp.sp9832e.so`, 32- and 64-bit builds) blends,
scales and rotates layers with the SC9832E GSP through `/dev/gsp`. It uses nothing else of the driver (no sysfs: the
only paths in the HAL are `/dev/gsp`, `/dev/ion` and `/data/gsp.cfg`). 4.14 has no such driver for the ADF display
stack (`DRM_SPRD_GSP` is a DRM component and needs `DRM_SPRD`, off with ADF). `drivers/media/gsp` is the 4.4
driver (`security-backports`) cut down to what the watches use:

| Part | Path |
|---|---|
| core, device (misc `gsp`), kcfg pool/queue, layers, sync | `drivers/media/gsp/gsp_*.c` |
| SoC glue (sharkle: nothing to do) | `gsp_interface/gsp_interface_sharkle.c` |
| the lite_r2p0 core (registers, scaler coefficients) | `gsp_lite_r2p0/` |
| uapi and kernel cfg headers (unchanged from 4.4) | `include/uapi/video/gsp_*cfg.h`, `include/video/gsp_*cfg.h` |

`CONFIG_SPRD_GSP` (depends on `ION_SPRD`, `SYNC_FILE`, `SPRD_IOMMU`, `!DRM_SPRD_GSP`: both bind
`sprd,gsp-lite_r2p0-sharkle`) is in `watch_414_stock81.fragment`.

Pruned: the other cores (`r1p1`, `r3p0`, `r4p0`, `lite_r1p0`, `r5p0`, `r6p0`, `lite_r3p0`), the other interfaces,
their of_match entries, the debug sysfs (it was global state keyed by device, and the HAL does not read it) and the
trace points. The runtime PM of 4.4 only toggled a state machine around work that already switches the clocks, the
IOMMU and the interface on and off per kcfg; it is replaced by system sleep ops that wait for the kcfg in flight.

## ABI kept

- `/dev/gsp` (misc, dynamic minor, name `gsp`), ioctl magic `'G'`: `GSP_GET_CAPABILITY` (`_IOWR`, nr `0x40`) and
  `GSP_TRIGGER` (nr `0x20 | async << 4 | split << 3 | kcfg count`), same structs, same numbers; the cfgs have no
  pointers, so 32-bit callers need only `compat_ptr()` (`compat_ioctl` is there).
- Sync trigger returns when the kcfgs are done. Async returns the release fence fd through the `sig_fd` of the last
  cfg's destination layer; the layers' `wait_fd` are acquire fences, waited by the core before it starts the kcfg.
- DT as the stock one: `sprd-gsp` (`core-cnt`, `io-cnt`, `cores`) and `gsp@20a00000` (`core-id`, `kcfg-num`, clocks,
  `iommus`); `sprd,sys-ap-ahb` is added to the sharkle one (the stock has it) for the block soft reset. MT108 (no
  display) keeps GSP off.

## 4.14 changes

- Sync: the legacy `sync_timeline`/`sync_pt`/`sync_fence` are a `dma_fence` timeline (`gsp_sync.c`, as the ADF
  port): one fence per async trigger, on the core's context, handed out as a `sync_file` fd; the acquire fences come
  from `sync_file_get_fence()`. Fences signal in the order of the core queue; an error signals with the error set.
- ION/IOMMU: the layer takes `sprd_ion_get_buffer_ref()` (holds the `dma_buf`, rejects non-ION fds) and hands the
  `ion_buffer` to `sprd_iommu_map()` of the core device, as the camera does; the attach/map_attachment of 4.4 is
  gone. Carveout buffers keep using their physical address.
- `kthread_create_worker`, `timer_setup`, `of_iomap`/`of_clk_get_by_name` released on remove, no `devm` on the core
  device (it never unbinds), `kvzalloc` for the core (the coefficient cache and pool are about 23 KiB).

## Hardening

The ioctl is reachable from the HAL domain. The 4.4 driver had no bounds on the user cfg and raced on the kcfgs.

- The kcfg lifetime is a refcount (ioctl, core, sync waiter); a kcfg goes back to the pool when the last drops it.
  4.4 handed it back in the core while the sync waiter still walked its list, and `cancel()` after a failed push
  decremented the fill count of kcfgs which were never queued.
- One kthread worker does trigger, release (irq) and recover (hang timer). Every started job has a generation;
  the irq and the hang timer record the one they fired for and the worker drops the events of any other, so a
  kcfg is ended exactly once and a late recover/release cannot end or unmap the next job. The irq handler reads
  `INT_GSP_STS`/`INT_GERR_STS`, returns `IRQ_NONE` on the shared line if neither is set and clears only what is
  set; a hung core is reset (stop, soft reset) before its buffers go.
- The release fence of a batch gets the first error of any of its kcfgs, not only the last one's.
- The ioctl size must be the core's cfg size and the count 1..`io-cnt` (4.4 sized the copy from the ioctl number);
  the capability copy is clamped to the capability struct (it copied `size` bytes from a smaller allocation).
- Every enum and field of the cfg is range-checked (formats, endian, rotation, scale taps: the coefficient code
  indexed its buffers with `8 - 2 * tap`), sizes are 1..8191.
- Addresses from user space are never used. An enabled layer needs a `share_fd` (4.4 programmed `src_addr` as given:
  DMA to any physical address); the offsets and the clip/work area extents are checked against the size of the
  held dma-buf before the kcfg is queued; the buffer is looked up once, so the fd cannot be swapped afterwards.
- Everything a kcfg holds (dma-buf refs, acquire fences, the release fence, the IOMMU mapping) is released on
  every path: fill errors, failed power-on or map, hang, remove (queued kcfgs are failed with their fences
  signaled). `copy_from_user`/`put_user` results are checked; the fd is reserved first and installed only when
  nothing can fail. Probe/enable error paths unwind in order (4.4 disabled a clock it had not enabled).

## Assumptions to check on hardware

- The destination bound takes both work areas (scaled, rotated by odd angles) as written from their positions
  inside `pitch x height`, and every plane of the format up to the lowest row written must be inside the buffer. If the HAL trips it, `dmesg` shows `cfg does not fit its buffers`.
- A hardware error interrupt fails the kcfg with `-EIO` (4.4 only logged it); the error code is still logged.
- A hung core is only stopped through the AHB soft reset; 4.4 had no reset for sharkle. `sprd,sys-ap-ahb` is
  mandatory: probe fails with a log if the node lacks it. Every regmap write of the reset is checked; if the reset
  fails the core is marked broken: the job in flight is signaled with `-EIO` but its mappings, dma-buf refs, kcfg and
  clocks are leaked on purpose (the block may still do DMA into them), the error is logged once, and new jobs are
  refused with `-EIO` until reboot. The release fence of that job is signaled with `-EIO` too, and the queued
  jobs are failed with `-EIO` and released normally (the block never touched them).
- Only the kthread worker ends a job. Admission is closed on the queue itself (broken or removal), so an ioctl
  past the early check cannot enqueue; removal quiesces the irq line and the timer, then a shutdown work on the
  worker resets the block, cancels the job in flight and fails the queued ones, and only then is anything freed.
- The last events of a job are read from the raw interrupt bits (the status register is 0 once masked), so an
  error raised after DONE fails the job.
- Found while porting, not fixed here: `drivers/video/adf/adf_fence.c` has no `.wait` in its `dma_fence_ops`, and
  `dma_fence_init()` `BUG_ON`s on that.
