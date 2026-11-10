# ADF display port for 4.14 (K18 / sp9832e / sharkle)

Status: skeleton, builds, nothing run on a unit. Scope: port the 4.4 ADF display
framework and the sprd ADF driver so the stock Android 8.1 `hwcomposer.sp9832e.so`
and `gralloc.sp9832e.so` (libadf: `adf_device_open`, `adf_device_post`,
`adf_device_post_v2`, `adf_blank`) can drive the K18 panel on 4.14, as an
alternative to the DRM/KMS stack already in this tree
(`drivers/gpu/drm/sprd`). Worktree: `kwt-414-adf`, branch `kernel-4.14-adf`,
based on `kernel-4.14`.

## What this is a port of

4.4 tree (`android_kernel_sprd_sc9832e`, `security-backports`):

- Framework: `drivers/video/adf/*.c` + `include/video/adf*.h` +
  `include/uapi/video/adf.h`, Kconfig symbol `ADF`.
- sprd driver: `drivers/video/adf/sprd/{core,drv}`, Kconfig symbols
  `ADF_SPRD`, `DISP_LITE_R2P0`, UAPI `include/uapi/video/sprd_adf.h`.

Verified first, before porting anything: K18's actual DISPC/DSI/DPHY backend is
**`dpu-lite-r1p0` / `sprd,dsi-ctrl-r1p0` / `sprd,megacores-sharkle`**
(`devices/k18/stock.dts`: `dispc@20800000 { sprd,ip = "dpu-lite-r1p0"; }`,
matching `dsi@21800000` and `dphy@0`), not r2p0. `CONFIG_DISP_LITE_R2P0`
(set `=y` on K18/DM63/MT107 alike in the 4.4 defconfigs) only gates two
unrelated quirks inside `sprd_dispc.c` (a 64-bit DMA mask and a `0x80000000`
IOVA offset in the boot-logo-restore helper) — it does not select the r2p0
backend. All dispc/dsi variants are always `obj-y` in the 4.4 Makefiles; the
real selection happens at runtime by matching the DT `sprd,ip` string. This
port therefore only carries the lite_r1p0 backend, not lite_r2p0.

## What was ported, and how

### ADF core framework (`drivers/video/adf/`)

Ported essentially whole: `adf.c`, `adf_client.c`, `adf_fops.c`/`.c32`,
`adf_fbdev.c`, `adf_format.c`, `adf_memblock.c`, `adf_sysfs.c`, plus
`include/video/adf*.h` and `include/uapi/video/adf.h` (byte-identical to 4.4 —
checked with `diff`, so the ioctl numbers and struct layouts are unchanged).

The one real rewrite: 4.4's ADF used `drivers/staging/android`'s old sync
framework (`sync_fence`, `sync_pt`, `sw_sync_timeline_create`), removed from
upstream well before 4.14 and replaced everywhere by `dma_fence`/`sync_file`.
Mapped:

| 4.4 | 4.14 |
|---|---|
| `struct sync_fence *` (acquire/complete fences, `adf_buffer.acquire_fence`, `adf_device_ops.present_fence/release_fence`, `adf_device_post()` etc.) | `struct dma_fence *` |
| `sync_fence_wait()` | `dma_fence_wait_timeout()` |
| `sync_fence_put()` | `dma_fence_put()` |
| `sync_fence_fdget()` (import an acquire fence fd from userspace) | `sync_file_get_fence()` |
| `sync_fence_install()` (export a complete fence as an fd) | `sync_file_create()` + `fd_install()`, new helper `adf_install_complete_fence()` in `adf_fops.c` |
| `sw_sync_timeline_create/sw_sync_pt_create/sync_fence_create` (the software completion-fence timeline `adf_device_post()` falls back to when a driver implements none of `present_fence`/`release_fence`/`advance_timeline`) | New file `adf_fence.c`: a small dma_fence-backed timeline (`adf_sw_complete_fence()`/`adf_sw_advance_timeline()`/`adf_sw_fence_timeline_init/destroy()`) reimplementing the exact same two-counter semantics (`timeline_max` = next target handed out, `timeline_cur` = watermark advanced once per completed post), so present/release fence timing relative to vsync is unchanged. |

Also renamed for the 4.14 API: `init_kthread_worker`/`init_kthread_work`/
`queue_kthread_work`/`flush_kthread_worker` → `kthread_init_worker`/
`kthread_init_work`/`kthread_queue_work`/`kthread_flush_worker`
(`include/linux/kthread.h` renamed these), and `ktime_equal()` (removed) →
`ktime_compare() == 0`. `drivers/video/adf/Kconfig`'s `depends on SYNC`
(symbol gone) became `depends on SYNC_FILE`.

**Evidence**: `drivers/video/adf/` alone, `CONFIG_ADF=y`, `make W=1
drivers/video/adf/` with the `sprd/` subdir temporarily disabled — 9 objects,
**0 errors, 0 warnings**.

### sprd driver skeleton (`drivers/video/adf/sprd/`)

Ported: `core/` (device/adapter/interface/overlayengine/hw_information — the
ADF object-model glue) whole; `drv/` trimmed to exactly what K18 needs:
`sprd_adf.c`/`sprd_adf_bridge.c` (probe, dpms/blank), `sprd_dispc.c` +
`dispc/{dispc,dpu,adf_config,adf_config_dpu}_lite_r1p0.c`, `sprd_dsi.c` +
`dsi/{mipi_dsi_api,mipi_dsi_hal}.c` + `dsi/core/mipi_dsi_r1p0{,_ppi}.c`,
`sprd_dphy.c` + `dphy/mipi_dphy_api.c` + `dphy/pll/megacores_sharkle.c`,
`sharkle/global_{dispc,dphy,dsi}.c`, `sprd_panel.c` + `panel-if/panel_ops_mipi.c`,
`sprd_round_corner.c`, `disp_lib.c`/`disp_notify.c`, `sysfs/{sysfs_class,
sysfs_panel}.c`.

Not ported (not needed for K18/sharkle, or for a probe-and-post skeleton):
the other seven SoC `drv/{whale2,iwhale2,sharkl2,isharkl2,sharklj1,sharkl3,
pike2,sharkl5}/` directories and dispc variants (r7p0/r8p0/lite_r1p0's r2p0
sibling/swdispc); `encoder/` (DSC/BV3 frame compression); the DesignWare DSI
core (`dwc_mipi_dsi_host*.c`, a different IP block); `panel-if/panel_ops_{rgb,
spi}.c` and `sprd_spi_intf.c` (K18 is MIPI DSI); `sprd_aod_idle.c` (AOD has no
4.14 port on the DRM side either, per `devices/k18/DTS_414.md`).

Fixed for 4.14 API/header changes, all mechanical:

- **ION**: 4.14's `drivers/staging/android/ion/ion.h` dropped the
  `ion_client`/`ion_handle` model entirely — `ion_alloc()` now returns an fd,
  and a new `ion_new_alloc()`/`ion_free()` pair hands back a `dma_buf *`
  directly. `sprd_adf_interface.c`'s framebuffer-allocation helper was
  simplified accordingly (no more `sprd_ion_client_create`/`ion_share_dma_buf`
  dance); the `fb_ion_client` field was dropped from `struct
  sprd_adf_interface`.
- **IOMMU**: `sprd_iommu_get_kaddr`/`sprd_iommu_free_kaddr` were renamed to
  `sprd_iommu_map`/`sprd_iommu_unmap` in `include/linux/sprd_iommu.h`
  (`struct sprd_iommu_map_data`/`sprd_iommu_unmap_data` are unchanged, so this
  is a pure rename) — this is the real per-frame buffer IOVA mapping path
  `sprd_dispc_iommu_map/unmap()` use on every post, now fixed.
- **Missing `mfd/syscon/sprd/sharkle/glb.h`**: the 4.14 tree does not carry
  the vendor syscon register-map headers at all (the DRM driver reaches the
  same hardware through `pm_runtime`/genpd instead of raw syscon pokes). The
  `sharkle/global_{dispc,dphy,dsi}.c` glue only needed two register blocks
  from it (`REG_AP_AHB_AHB_RST`/`REG_AP_AHB_MISC_CKG_EN` and
  `REG_AON_APB_PWR_CTRL` with their bits) — these are SoC register facts, not
  kernel-version-dependent code, so they were copied unchanged into a new
  local `sharkle/glb_regs.h`, cited from the 4.4
  `include/linux/mfd/syscon/sprd/sharkle/{ap_ahb,aon_apb}.h`. Everything else
  in these three files (clocks via `of_clk_get_by_name`/`clk_set_parent`,
  `syscon_regmap_lookup_by_compatible()`) is already generic mainline API,
  unchanged between 4.4 and 4.14.
- **Debug sysfs**: `sysfs_{dispc,dsi,dphy}.c` and `sysfs_refnotify.c`
  (~1,900 lines of `/sys/class/display/*` tuning/debug attribute groups) were
  not ported; `sprd_dispc.c`/`sprd_dsi.c`/`sprd_dphy.c` probe still call
  `sprd_{dispc,dsi,dphy}_sysfs_init()` unconditionally, so a new
  `sysfs/sysfs_stubs.c` provides them as no-ops. `sysfs_panel.c` (brightness,
  which the lights/backlight path actually needs) **is** ported.
- **Write-back buffer**: `sprd_dispc_wb_buf_alloc()` (screen-mirroring)
  relied on the removed ION client/handle API with no clean 4.14 equivalent
  (no physical-address/kernel-map query on a `dma_buf`). Stubbed to return
  `-ENOTSUPP`; every caller already treats a non-zero return as "write-back
  unavailable" and falls back to a plain flip, so this does not touch the
  primary panel path.

**Evidence**: full `drivers/video/adf/` (framework + sprd skeleton),
`CONFIG_ADF=y CONFIG_ADF_SPRD=y`, `make W=1` — **0 errors**. 138 warnings
remain, all pre-existing vendor-code style issues carried over from the 4.4
source (`-Wmissing-prototypes` on functions declared in their own headers,
`-Wimplicit-fallthrough`, `-Wtype-limits` on `unsigned >= 0` checks,
`-Wunused-but-set-variable`) — not touched, since they're style, not
correctness, and the port's job was API compatibility, not a vendor-code
cleanup.

## Kconfig: ADF vs. DRM, not both

`drivers/video/adf/sprd/Kconfig`: `config ADF_SPRD` gained `depends on
!DRM_SPRD`. This is intentionally **one-directional**, not a `choice` block
or a matching `depends on !ADF_SPRD` added to `DRM_SPRD`: Kconfig's recursive-
dependency checker treats a same-pair `depends on !X`/`depends on !Y` on both
sides as a cycle (verified — `scripts/kconfig/merge_config.sh` reports
"recursive dependency detected" and refuses the merge if both directions are
added). A `choice` was also rejected: `DRM_SPRD` is used by boards with no ADF
driver at all, and forcing a choice would break their configs. One direction
is sufficient for the stated requirement: in an interactive menuconfig
session, turning `DRM_SPRD` on while `ADF_SPRD=y` immediately unselects
`ADF_SPRD` (its `depends on` is no longer met), so the two can never end up
simultaneously enabled through normal config editing. The caveat is that a
**batch** config merge (like the one below) still has to turn `DRM_SPRD`
off explicitly — Kconfig does not retroactively flip the *other* symbol off,
it only refuses to turn *this* one on. `arch/arm64/configs/watch_414_adf.fragment`
does this (`# CONFIG_DRM_SPRD is not set` alongside `CONFIG_ADF_SPRD=y`).

## Panel notifier: shared between both display stacks

Requirement: ADF's blank should also raise the sprd panel notifier
(`include/linux/sprd_panel_notifier.h`), so touch (`cst0xx`/`hynitron`) and
`sc7a22` keep getting screen on/off events regardless of which display driver
is active. Problem: `sprd_panel_notifier.c` (the chain + the `screen_on`
state + the `sprd_panel_notify()` raise function) lived inside
`drivers/gpu/drm/sprd/`, whose *entire directory* is gated by `obj-
$(CONFIG_DRM_SPRD) += sprd/` in `drivers/gpu/drm/Makefile` — with
`DRM_SPRD` off (as it is whenever `ADF_SPRD` is selected), that file would
never even be compiled, and the public header's `IS_ENABLED(CONFIG_DRM_SPRD)`
guard would make every caller (touch drivers, `sc7a22`) get the inline
`-ENODEV` stub instead of real events.

Fix, all in files that already exist for this purpose, no behaviour change
when `DRM_SPRD=y`:

1. New hidden `config SPRD_PANEL_NOTIFIER` (`drivers/gpu/drm/sprd/Kconfig`),
   `select`ed by both `DRM_SPRD` and `ADF_SPRD`.
2. `sprd_panel_notifier.o` moved out of `drivers/gpu/drm/sprd/Makefile`'s
   `obj-y` list into a new line in the *parent* `drivers/gpu/drm/Makefile`:
   `obj-$(CONFIG_SPRD_PANEL_NOTIFIER) += sprd/sprd_panel_notifier.o` — built
   directly, independent of the `obj-$(CONFIG_DRM_SPRD) += sprd/` directory
   recursion.
3. `include/linux/sprd_panel_notifier.h`'s guard changed from
   `IS_ENABLED(CONFIG_DRM_SPRD)` to `IS_ENABLED(CONFIG_SPRD_PANEL_NOTIFIER)`.
4. `sprd_adf.c`'s `sprd_adf_interface_dpms_on/off()` (reached from
   `adf_blank()` via `intf->ops->blank` → `sprd_adf_intf_blank()` →
   `sprd_adf_dpms_{on,off}()`, the same functions that already raise ADF's
   own internal `ADF_EVENT_BLANK` notifier) now also call
   `sprd_panel_notify(SPRD_PANEL_EVENT_{ON,OFF})`, forward-declared locally
   with a comment pointing at its real definition in
   `drivers/gpu/drm/sprd/sprd_panel_notifier.c`.

**Evidence**: built `drivers/video/adf/` + `drivers/gpu/drm/` together with
`ADF_SPRD=y`/`DRM_SPRD=n` (0 errors), and separately rebuilt
`drivers/gpu/drm/` from a clean `sprd_sharkle_defconfig` +
`watch_414.fragment` config (`DRM_SPRD=y`, `ADF` unset — the tree's existing
default) to confirm the DRM build is unaffected: `sprd_panel_notifier.o`
still builds, 0 errors, 0 new warnings.

## K18 panel data (ICNA3311 148)

New `arch/arm64/boot/dts/sprd/lcd/lcd_icna3311_oled148_adf.dtsi`: the stock
`lcd3311148@3311148` node from `devices/k18/stock.dts` (4.4 ground truth),
copied property-for-property into `sprd_panel.c`'s DT binding (`if-type`,
`work-mode`, `init-data`, `sleep-in`/`sleep-out`, `display-timings`,
`oled-backlight` — the same scheme `sprd_panel.c`'s `panel_info_attach()`
parses via `panel-drivers` phandles). This is the exact panel the K18 DRM
variant also uses (`lcd_icna3311_oled148_mipi.dtsi`, see
`devices/k18/DTS_414.md` for the decoded init/sleep streams and brightness
table — same panel, same facts). Nothing was invented: every property value,
including the full 256-entry identity brightness table, is copied byte-for-
byte from the stock node.

## Board variants

The stock dispc, dsi, dphy, sprd-panel-if and sprd-adf nodes are identical on the K18, DM63 and DW100U silver stock
DTs apart from phandles. They live in `sp9832e-watch-adf.dtsi`: the DRM display nodes disabled and the stock ADF
nodes added, clock phandles translated to the 4.14 providers (the mapping is in the file header). Each ADF board file
includes its board dts and that dtsi, adds its panel nodes under `/lcds` and sets `panel-drivers`:

| Board file | Panels | iommu_dispc iova size (stock) |
|---|---|---|
| `sp9832e-1h10-k18-adf.dts` | `lcd/lcd_icna3311_oled148_adf.dtsi` (the unit's panel) | 0x9600000 |
| `sp9832e-1h10-dm63-adf.dts` | `lcd/dm63_stock_lcds_adf.dtsi`, all 14 stock nodes | 0x8000000 (sharkle default) |
| `sp9832e-1h10-dw100u-silver-adf.dts` | `lcd/dw100u_stock_lcds_adf.dtsi`, all 23 stock nodes | 0x9600000 |

The DM63 and DW100U panel files are generated from the decompiled stock DTB by `devices/stock_lcds_to_dtsi.py`
(drops the phandle lines, labels each node `<board>_lcd<id>`, prints the `panel-drivers` line). The stock nodes are
already in the binding the ported `sprd_panel.c` parses and use raw GPIO numbers, so nothing else needs
translating. Panels with the same ID differ between boards (init and sleep sequences), so each board keeps its own
file. Checked: every generated node matches its stock node property for property (phandles aside), and the K18 ADF
dtb is unchanged by the move into the shared dtsi.

A K18 boot image candidate is described in `k18/BOOT_414.md`. DM63 and DW100U silver images need the Mali port merged
first (see `KERNEL_4.14_PLAN.md`).

## Build results

| Check | Result |
|---|---|
| `drivers/video/adf/` framework only, `CONFIG_ADF=y`, W=1 | 0 errors, 0 warnings |
| `drivers/video/adf/` incl. sprd skeleton, `CONFIG_ADF_SPRD=y`, W=1 | 0 errors, 138 pre-existing vendor-style warnings |
| `drivers/video/adf/` + `drivers/gpu/drm/`, `ADF_SPRD=y`/`DRM_SPRD=n` | 0 errors |
| `drivers/gpu/drm/`, default config (`DRM_SPRD=y`, ADF unset) | 0 errors — DRM build unaffected |
| Merged config `sprd_sharkle_defconfig` + `watch_414.fragment` + `watch_414_adf.fragment`, `olddefconfig` | `CONFIG_ADF=y`, `CONFIG_ADF_SPRD=y`, `CONFIG_DISP_LITE_R2P0=y`, `CONFIG_SPRD_PANEL_NOTIFIER=y`, `CONFIG_DRM_SPRD` unset — no Kconfig recursive-dependency warning |
| Full `Image` link with the merged config above | links, 0 errors (also after the review hardening below, and on the merged `kernel-4.14`: `Image`, modules and 38 dtbs in both the DRM and the ADF configuration) |

All builds: `ARCH=arm64`, the project's `aarch64-linux-gnu-` cross compiler,
out-of-tree (`O=/tmp/...`, deleted after use).

## Review hardening

Ten review rounds (gpt-6.1-sol) after the initial port. Most findings are bugs inherited unchanged from the 4.4 vendor
driver; the 4.4 tree still has them (see "Open items").

Userspace-reachable memory safety:
- `post_config`: no double free when `sync_file_create()` fails after the post is queued (the post owns `bufs` and
  `custom_data` by then).
- Copied posts (`adf_device_post()`) take their own acquire-fence reference; the simple-post ioctl drops its imports.
- Custom data must cover `n_bufs` layers; `n_bufs` is capped at the 4 hardware layers (an empty post is still
  accepted, as in 4.4).
- Crop and scanout extent: `start_x/y` >= 0 and `start + crop` and `start + dst` within the buffer (these cores have
  no scaler: the dispc fetches `dst_w` x `dst_h` from the crop origin).
- Non-ION dma-bufs never reach the ION-specific helpers in the dispc IOMMU map/unmap.
- Panel sysfs parsers (`hporch`, `vporch`, `esd_return`) are bounded to the destination array; the DT
  `esd-return-code` count and the ESD/ID DCS reads are bounded to their 4-entry buffers.
- 32-bit `ADF_POST_CONFIG` converts into kernel memory and writes the fence fd to the caller before installing it.

Fences and the post worker:
- Every accepted post reserves its own sequence number, comparisons are wrap-safe, and a rejected post gives its
  number back.
- The software timeline is refcounted separately from the device, so a fence fd outliving the device is safe.
- Completion is claimed once per dispatched post (`post_gen`/`signaled_gen` under `signal_lock`), so the dispc
  fence-signal irq and the worker cannot both advance the timeline, and a late irq cannot run ahead of the posts.

Flip failures (the post op returns `int`):
- Error: the previous onscreen post stays mapped and the failed one is freed.
- `-ETIMEDOUT` (update-done or stop-done timeout, a flip on a stopped DPI controller): the hardware may scan either
  config, so both are kept; the old one moves to `stale_posts` and is freed only after a later flip succeeds.
- At 4 stale posts new posts fail without touching the hardware (the display freezes instead of leaking).
  `sprd_dispc_run()` and `refresh_restore()` call `adf_device_scanout_restarted()`, which allows one probe flip.
- The boot logo is freed only after the first update latched; `config_saved` only ever points at a config that
  reached the hardware.

Configuration:
- `drivers/video/adf/sprd/` is built only with `CONFIG_ADF_SPRD`.
- Only single-dispc pipes are supported; the double-dispc `sprd,display-mode` values are refused (no board sets it).

Accepted on purpose:
- A failed flip still advances the timeline, so the onscreen post's release fence can fire while it is displayed
  (userspace may redraw it early: a visual glitch; ADF keeps its buffers and mappings).
- IOMMU unmap is skipped while the dispc is uninitialized (it writes display-MMU registers in the display power
  domain, which can hang the bus), as in 4.4. A post retired while suspended leaks its IOVA range and ION pin.

## Open items (need a unit, or more work)

- **Nothing has run on a watch.** Everything above is a compile-time check.
- The lite_r1p0 DISPC/DSI/DPHY register sequencing itself (layer config,
  IOMMU mapping order, DPHY PLL timing, DSI command mode handshaking) is
  carried over from the 4.4 source with only API renames, not independently
  re-verified against real 4.14 clock/power-domain bring-up for this board —
  that can only be checked on a unit.
- `sharkle/glb_regs.h`'s two register blocks were taken from the 4.4 header
  pair, not re-derived from a 4.14 datasheet; they are SoC facts that should
  not have changed, but have not been cross-checked against the 4.14 DRM
  driver's own power sequencing for the same hardware.
- Debug sysfs (`sysfs_stubs.c`), AOD, write-back, RGB/SPI panel interfaces,
  the DesignWare DSI core, the other seven SoC directories: not ported, as
  detailed above.
- ADF board variants exist for the K18, DM63 and DW100U silver; MT107, MT108, DM82 and the DW100U V13M use DRM
  only (the V13M stock image is not available).
- 138 pre-existing W=1 warnings in vendor code, not cleaned up (see "sprd
  driver skeleton" above).
- The 4.4 `adf_sysfs.c`/`adf_fops.c` prototypes flagged `-Wmissing-prototypes`
  were not touched either, for the same reason: this port's job is API
  compatibility with the 4.4 ABI, not a general vendor-code lint pass.
- The memory-safety fixes in "Review hardening" are not in the 4.4 tree, which has the same bugs; they need a
  backport to `security-backports`.
- The DRM sprd driver (`drivers/gpu/drm/sprd`, already in `kernel-4.14`) has its own unbounded copy of the sysfs
  array parser (`disp_lib.c`, nine callers in `sysfs/`): same fix still to do.
