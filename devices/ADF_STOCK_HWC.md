# ADF post layout of the stock Android 8.1 hwcomposer

The stock vendor `hwcomposer` posts ADF custom data with **48-byte layer
records**, not the 68-byte `struct sprd_adf_hwlayer_custom_data` of this tree's
uapi. Before the fix every post was refused in `sprd_adf_restruct_post_config`
("data size is ilegal data_size = 60": 12 + 1 * 48 < 12 + 1 * 68) and
SurfaceFlinger logged `presentAndGetReleaseFences: failed for display 0`.

## Evidence (stock DM63 boot.img, kernel 4.4.83)

The image is a raw arm64 `Image` (relocatable, kallsyms offsets zeroed, so no
symbol addresses): the two stock `data size is ilegal` format strings were
located and their ADRP/ADD references disassembled with
`aarch64-linux-gnu-objdump -D -b binary -m aarch64`.

* `sprd_adf_device.c` `sprd_adf_restruct_post_config`: `ldr x3, [post, #24]`
  (`custom_data_size`), `cmp x3, #0x3b; b.hi ok`: the only size test is
  `custom_data_size >= 60` (= 12 + 48). No relation to `n_bufs`.
* `drv/sprd_adf.c` `sprd_adf_restruct_post_config`: same `cmp x2, #0x3b`; the
  per-layer loop advances the custom-data pointer by `add x20, x20, #0x30`
  (48) and the `adf_buffer` pointer by 0x68.
* Fields read per layer (offset from the record start, record 0 at
  `custom_data + 12`): interface_id +0, alpha +12, start_x/y/w/h +16..+22,
  dst_x/y/w/h +24..+30, blending +32, rotation +36, scale +40,
  compression +44; end of record +48. `height`, `header_size_r/y/uv` and
  `y2r_coef` are never read, and the stock config struct (0x1c0 bytes per
  layer, `kzalloc`ed) has no member for them.

The uapi header has a single commit in this tree (`git log -p
include/uapi/video/sprd_adf.h`): no older layout in history. `kernel-4.14`
carries the identical 68-byte header, so it has the same mismatch.

## Handling

`sprd_adf_hwlayer_stride()` (`sprd_adf_common.h`) is used by both size checks:

* `size >= 12 + n * 68`: new layout (unchanged behaviour);
* `size == 12 + n * 48`: stock layout, each record is copied into a zeroed
  `struct sprd_adf_hwlayer_custom_data`, so `height`, `header_size_*` and
  `y2r_coef` are 0 as in the stock kernel (the dispc code does not consume
  them; `compression` is part of the stock record);
* `n > SPRD_N_LAYER` or any other size: refused;
* `n == 0` (empty post): accepted from the stock minimum, 12 + 48.

## Mali ioctl 0x4004801d (nr 29)

The stock `mali.ko` (r21p0-01rel0) handles `_IOW(0x80, 29, u32)` in
`kbase_ioctl`: it copies 4 bytes from the user and stores them in the context
(`kctx + 0x23460`). `kbase_jd_submit` passes that value to
`kbase_platform_set_boost()`, which raises the module-wide
`gpu_boost_level` used by `kbase_platform_modify_target_freq` (GPU DVFS
floor). It is a SPRD GPU boost hint: without it the ioctl fails with
-ENOTTY, rendering is unaffected and only the DVFS boost is lost.
