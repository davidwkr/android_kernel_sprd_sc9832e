# Camera on 4.14

**Compile-only. Nothing here has run on a watch.**

The stock Android 8.1 camera HAL of the watches (DM63, DW100U; all the watches have a camera) talks to the 4.4
Spreadtrum camera driver through `/dev/sprd_image` (DCAM), `/dev/sprd_isp`, `/dev/sprd_cpp` and `/dev/sprd_sensor`
(the stock ueventd sets them to `system:camera`). The sensor drivers are in the HAL: the kernel powers the sensor
(clocks, LDOs, reset/power-down GPIOs) and gives the HAL I2C access through `/dev/sprd_sensor`.

The 4.14 tree had no camera driver, only DT nodes written for Unisoc's newer one. `drivers/misc/sprd_camera` is
the 4.4 driver with the sharkle variants only:

| Block | Directory | Kconfig |
|---|---|---|
| DCAM (capture) | `dcam/dcam_if_r4p0` | `SPRD_CAMERA_DCAM_IF_R4P0` |
| ISP | `isp/isp_r6p11` | `SPRD_CAMERA_ISP_R6P11` |
| CPP (scale/rotate) | `cpp/lite_r3p0` | `SPRD_CAMERA_CPP_LITE_R3P0` |
| CSI-2 receiver | `sensor/csi2/csi_sprd/r2p0v2` | `SPRD_CAMERA_CSI_R2P0V2` |
| Sensor power, OTP, flash | `sensor`, `flash` | `SPRD_CAMERA` |

All are enabled in `watch_414_adf.fragment` (the camera serves the stock system, which also needs ADF). The 4.4
uapi headers (`sprd_img.h`, `sprd_isp_r6p11.h`, `sprd_sensor_k.h`, `sprd_cpp.h`) replace the newer, incompatible
4.14 ones, which nothing else used: the HAL was built against them.

## 4.14 changes

- ION: 4.14 has no ION clients. The ISP buffers are allocated with `ion_new_alloc()` and kept as a `dma_buf`,
  mapped with `sprd_ion_map_kernel()` and their phys address read with `sprd_ion_get_phys_addr_by_db()`. 4.14 has
  no `ION_FLAG_NOCLEAR`, so the offline buffers are cleared at allocation. `sprd_ion` gains the 4.4 helpers the
  camera uses: `sprd_ion_get_buffer_ref()` (holds the fd's `dma_buf` until `sprd_ion_put()`),
  `sprd_ion_check_phys_addr()`. All the `sprd_ion` lookups now reject `dma_buf`s ION did not export.
- Power domain: the sharkle `cam-domain` driver of `drivers/soc/sprd/domain` (`SPRD_CAM_PW_DOMAIN_R5P1`, already in
  `sprd_sharkle_defconfig`, same clocks as the 4.4 camera copy) provides `sprd_cam_pw_on/off` and
  `sprd_cam_domain_eb/disable`; the camera copies are gone and the DCAM probe no longer initialises it. The DCAM
  calls `sprd_cam_pw_on()` when it is opened, after the domain driver has probed.
- 4.4 headers the drivers still include (`linux/mfd/syscon/sprd-glb.h` with the sharkle registers, the
  `linux/wakelock.h` wrapper on wakeup sources) are in `drivers/misc/sprd_camera/compat`.
- `ktime_to_ns()` for the frame boot time; `linux/module.h` where it was implicit.

## DT

`sp9832e-watch-camera.dtsi`, included by the DM63 and DW100U ADF boards, as in the stock DTs:

- main sensor: `i2c0` 0x20 (400 kHz), reset `ap_gpio` 45, power-down 47, `host = "dcam0"`, graph endpoint to
  `mipi-csi-phy1`, whose `sprd,csi` is `csi0`; clocks `CLK_SENSOR0`/`CLK_SENSOR0_EB` and the TWPLL taps, supplies
  `vddcamio`, `vddcama`, `vddcamd`, `vddcammot` (the stock LDOs 19, 5, 17, 6);
- the DCAM, ISP, CPP and their IOMMUs enabled; the 4.4 ISP and CPP drivers look up `sprd,cam-ahb-syscon`, which
  the sharkle.dtsi nodes (written for the newer driver) lack, so it is added.

The DM63 ADF board adds the stock sub sensor: `i2c0` 0x21, reset 44, power-down 46, `host = "dcam1"`,
`mipi-csi-phy0_m` and `csi1`. The DW100U has only the main sensor. Neither has a camera flash in stock (the flash
IC bus is disabled and there is no PMIC flash node); `FLASH_SC2721` still depends on the 4.4 `PMIC_SC2721` (its
register header does not exist on 4.14) and stays off.

## Security

The ioctls are reachable from the camera HAL process (`/dev/sprd_*` are `system:camera`). The 4.4
`security-backports` hardening of the CPP buffer slots (reference held while mapped, user offsets checked against
the buffer size) came with the copy. Three review rounds of the port found and fixed (the same code is in the 4.4
tree, branch `security/camera-ioctl`):

- sensor: stack overflows in the generic I2C read/write (`i2c_count` into `cmd[2]`/`cmd[64]`) and the multi-sensor
  AEC write (`msize`/`ssize` into 64-entry arrays); 32-bit truncated register-table allocation; unchecked
  `sensor_id` indexing the CSI table;
- DCAM: unchecked `channel_id` (`PATH_FRM_DECI`, `SET_SHRINK`) and `buffer_count` indices; `write()` copying the
  user length into a stack struct; the ioctl dispatcher returned 0 for every error; a failed DCAM/ISP enable left
  the resource and the DCAM enabled;
- ISP: 2D-LSC weight count unbounded by the copied weights and the register area; fetch offset and frame
  (`pitch x height`, also for slices) unbounded by the imported buffer; a block-buffer allocation failure reported
  as success; a failed enable or clock enable left counts, clocks and the domain on;
- lifetime: ISP/DCAM-LSC parameter ioctls and the raw capture now run under `isp_module_sema` and fail with
  `-ENODEV` once the module is disabled (they used buffers `PUT_DCAM_RES` frees); a failed offline-buffer IOMMU
  map unmaps the completed ones; a partial `pfiommu_get_sg_table()` import is dropped as a whole; a failed
  DCAM power-on, domain, clock or IRQ enable unwinds the stages and `s_dcam_users`, as the ISP does;
- CPP: the scaler did not check the plane extents (rotation and DMA did);
- `sprd_ion`: every lookup cast any `dma_buf`'s `priv` to `ion_buffer`; non-ION buffers are now rejected (DRM
  PRIME imports of non-ION buffers are accepted only when contiguous).

Known and not fixed (vendor ABI: the HAL passes device addresses, not fds, so there is nothing to check against):

- the ISP raw-capture store path programs DMA destinations from user space, and the fetch U/V addresses too;
- the fetch row length is not bounded (only `pitch x height`): the row span depends on the format and the MIPI
  packing; read DMA within the camera IOMMU domain;
- the fetch bounds are global, not per ISP (the watches have one ISP, `sprd,isp-count = <1>`);
- buffer references taken by `pfiommu_get_sg_table()` live until the module is released (the list is bounded
  to 256 buffers, further imports fail with `-ENOSPC`; an entry cannot be dropped on `pfiommu_free_addr()`
  because the same buffer is queued again and the frames keep a raw `dma_buf` pointer that only the list
  reference protects), and a failed unmap in the CPP keeps its buffer pinned on purpose (the mapping may still
  be live).
