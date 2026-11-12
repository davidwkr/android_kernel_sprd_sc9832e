# DM63 bench fixes ported to 4.14

Branch `fixes/bench-port-414`, from `kernel-4.14`. The 4.14 tree runs the same stock Android 8.1 vendor on the same
watches (DM63, DW100U silver). Nothing here was run on a device; the result is a compile check (item 7).

| # | Item | Result |
|---|---|---|
| 1 | `fixes/mali-evict-node-414`, `fixes/adf-stock-hwc-414` | Merged (`--no-ff`, both fast-forwardable): `4e6454752526` (INIT_LIST_HEAD of `evict_node`), `94e9a0c35849` (ADF takes the stock hwcomposer's 48-byte layer records). |
| 2 | GPU cap `8585d3aa2d18` | Same bug, cherry-picked (`-x`), applied without conflict. |
| 3 | SC7A22 interrupt storm | Same driver, ported: 4 cherry-picks (1 hand-resolved conflict set) and the doc. |
| 4 | zram/lz4 `f7b3e49f774d` | Not affected, no change. |
| 5 | Log floods | Both ported by hand (different paths). |
| 6 | loglevel | `build_414_stock81.sh` and the three docs. |
| 7 | Build | Image and both DTBs build, no new warnings. |
| 8 | GPU cooling, boost ioctl (4.4 `fixes/mali-thermal-boost-44`) | Ported where it applies; see section 5b. |

## 2. GPU cap

4.14 `drivers/gpu/arm/midgard_r21p0/backend/gpu/mali_kbase_devfreq.c` had the same `kbase_devfreq_target()`:
`freq = *target_freq;` followed straight by `devfreq_recommended_opp(dev, &freq, flags)`, with no use of
`gpu_dfs_ctx.freq_range_max` (sharkle platform file). `kbase_platform_max_freq()` is added and the request is clamped
to it. `8585d3aa2d18` is ported, plus `305bad276f41` from `fixes/mali-thermal-boost-44` (hand-resolved: the 4.14
OPP core returns a referenced OPP, so the first one is `dev_pm_opp_put()` before the floor retry): an OPP above
`sprd,dfs-range-max` is never kept; the lookup is retried as a strict floor and fails if none exists. Strengthened
beyond the 4.4 commit: the floor lookup is forced when the request is `>=` the cap (a request equal to the cap with
normal flags could round up above it when the cap OPP is unavailable).

4.4 (`fixes/mali-thermal-boost-44`, worktree in use by another build, not touched): change `freq >
kbase_platform_max_freq()` to `freq >= kbase_platform_max_freq()` in the first cap check of
`drivers/gpu/arm/midgard_r21p0/backend/gpu/mali_kbase_devfreq.c` (comment: request equal to the cap is floored too).
The post-lookup check already exists there.

## 3. SC7A22

`drivers/input/misc/sc7a22.c` is built in (`CONFIG_SENSORS_SC7A22=y` in `watch_414.fragment`) and is the same driver
(same `request_irq(..., IRQF_TRIGGER_HIGH, "sc7a22-hand-up-irq")` with the `disable_irq_nosync` / work / `enable_irq`
loop). It differs from the 4.4 file only in the screen-state notifier (4.14: sprd DRM panel notifier, 4.4: ADF), which is
why the cherry-picks conflicted in context lines; the resolutions keep the 4.14 notifier code.

Ported: `b324aef99c0c` (arm on the opposite of the pin level), `3046fae1257c` (threaded IRQ, `IRQ_NONE` without a
source bit), `5153c0d63831` (pin sampled around the flag read, retry of failed reads) and the suspend-gate commit
`b9a5c0eda0ac` ("keep the flag retry from queuing during suspend"), which exists on the branch now although it was
reported as pending: if that commit is amended on `fixes/sc7a22-irq-storm-44`, re-port it. `devices/SC7A22_IRQ.md` is
copied from that branch as its own commit. All with `-x` trailers.

## 4. zram/lz4

Not broken on 4.14. `zcomp_compress()` sets `*dst_len = PAGE_SIZE * 2` over a two-page `zstrm->buffer`
(`__get_free_pages(..., 1)`), `crypto/lz4.c` passes `*dlen` on as the output capacity
(`LZ4_compress_default(src, dst, slen, *dlen, ctx)`), and `zram_bvec_write()` turns `comp_len >= huge_class_size` into a
stored page. The 4.4 failure came from the 4.4 `zcomp_lz4.c` passing `PAGE_SIZE`; 4.14 has no such file. Checked by
reading the code, not run.

## 5. Log floods

- spipe: `drivers/soc/sprd/modem/sipc/spipe.c` had the same `pr_info("spipe %d-%d not ready to open!\n", ...)`
  (`CONFIG_SPRD_SIPC_SPIPE=y`); now `pr_info_ratelimited`.
- WCN: `drivers/misc/sprdwcn/boot/wcn_integrate_boot.c` has the same `WCN_INFO` per poll in the BTWF, GNSS boot, GNSS cali
  and deep-sleep loops, and `platform/rf/rf.c` the per-entry `pr_info("[g_table]...")`. Same change (first poll, every
  32nd, timeouts with the magic value; `WCN_DBG` for the rest, as 4.14 has no `WCN_HEADER`).

## 6. loglevel

The default build uses `sp9832e-1h10-{dm63,dw100u-silver}-adf.dtb` (stock `loglevel=1`) and still merges
`watch_414_ramoops.fragment`. `LOGLEVEL7=1` selects the `-ramoops` DTBs and `*_ramoops_candidate*.img` names. With
`loglevel=1` the pstore console record holds little; oops/panic records still work.

## 7. Build

`sprd_sharkle_defconfig` + `watch_414.fragment` `watch_414_adf.fragment` `watch_414_stock81.fragment`
(`merge_config.sh -m`, `olddefconfig`), `O=` in a temporary directory, `Image` and the dm63 / dw100u-silver adf DTBs:
exit 0. Four warnings, none in a touched file (host `modpost.c`, `dtc` `dtbo_merge_check`, `dtc.c`, and
`dpu_lite_r1p0.c` misleading indentation, all in untouched code). The build directory was deleted; no image was packed.

## 4. Review fixes

- SC7A22 screen notifier: the registration, callback and both unregistration guards in `sc7a22.c` now use
  `CONFIG_SPRD_PANEL_NOTIFIER` instead of `CONFIG_DRM_SPRD`. `sprd_adf.c` calls the same `sprd_panel_notify()` chain
  (ON/OFF) that the DRM driver does, so with the ADF config the key is no longer suppressed. Merged stock81 + ADF
  config: `SPRD_PANEL_NOTIFIER=y`, `ADF_SPRD=y`, `DRM_SPRD` unset, `SENSORS_SC7A22=y`.
- GPU cap edge case: see section 2.
- Build (stock81 merged config, temp `O=`, deleted): `Image` rc 0, no warnings in `sc7a22.c` or
  `mali_kbase_devfreq.c`.

## 5b. GPU cooling and boost hint (from `fixes/mali-thermal-boost-44`)

What 4.14 already had: `drivers/thermal/sprd_gpu_device.c` (same driver, `cluster_data[]`, power model),
`SPRD_GPU_COOLING_DEVICE` Kconfig, `of_devfreq_cooling_register_power()` with `struct devfreq_cooling_power` taking the
`struct devfreq *` (4.14 devfreq cooling API; the 4.4 one does not), the IPA files, and the cap (`305bad276f41` and its
edge fix `0cc8de19e132`, not touched). What it lacked: the thermal fixes, the Kbuild fix, the boost ioctl, any
`gpu-cooling-devices` node (the DT had a critical-only `gpu-thmzone`, `THERMAL_414.md`), and the config.

| 4.4 commit | 4.14 |
|---|---|
| `a0beabda13c4` registration | Ported by hand in `kbase_devfreq_init()/term()`, adapted: `-ENODEV` from `create_gpu_cooling_device()` (no DT node, or no `lwfq/type` property, which the 4.14 helper checks) is a warning, not a GPU init failure. Config in `watch_414.fragment` (`DEVFREQ_THERMAL`, `SPRD_GPU_COOLING_DEVICE`), not the defconfig; new `sp9832e-watch-gpu-cooling.dtsi` (included by DM63 and DW100U only, as on 4.4) holds the `gpu-cooling0` node, alias, 70/85 C passive trips, `sustainable-power` and the cooling map, with the 4.4 DM63/DW100U values. The defconfig and `watch_lib.sh` hunks do not exist here. |
| `e38ce9675887`, `f3844096706a` (thermal), `ad4a2e096dcb` | One commit on `sprd_gpu_device.c`: bounded lookup, devname size, partial-registration unwind, alias bound with `-EINVAL`, NULL cooling device no longer success, ERR_PTR zone lookup (now looked up when read, as it was cached once possibly before the zone registered), zero coefficient guard. Skipped: Hz to MHz and mV fixes, 4.14 already converts Hz to MHz and takes mV. |
| `f3844096706a` IPA unwind | Applied (`kbase_ipa_term()` when the cooling registration fails). |
| `70b49b23478f` ipa/Kbuild | Same wildcard guard, applied. |
| `ef78370b733d` + boost part of `f3844096706a` | Cherry-picked without the defconfig hunks; `CONFIG_MALI_BOOST=y` in `watch_414.fragment`. The cap is applied after the boost floor, so it wins. |

Not run on a device. Bench to check: the cooling device appears under `/sys/class/thermal` and the GPU zone binds it (if
`lwfq/type` is unset on the stock bootloader the warning is logged and the GPU runs unthrottled); boost ioctl
`0x4004801d` stops returning ENOTTY. Build: stock81 config (`sprd_sharkle_defconfig` + `watch_414`, `watch_414_adf`,
`watch_414_stock81` fragments, `merge_config.sh -m`, `olddefconfig`), temp `O=` (deleted), `Image` and all DTBs: exit 0,
the options set, four warnings, none in a touched file (`dtc.c`, `modpost.c`, `dtbo_merge_check.c`, `dpu_lite_r1p0.c`).
