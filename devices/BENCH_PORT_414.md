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

## 2. GPU cap

4.14 `drivers/gpu/arm/midgard_r21p0/backend/gpu/mali_kbase_devfreq.c` had the same `kbase_devfreq_target()`:
`freq = *target_freq;` followed straight by `devfreq_recommended_opp(dev, &freq, flags)`, with no use of
`gpu_dfs_ctx.freq_range_max` (sharkle platform file). `kbase_platform_max_freq()` is added and the request is clamped
to it. Only `8585d3aa2d18` is ported. The follow-up for the floor/ceiling fallback edge case is being made on
`fixes/mali-thermal-boost-44` by another worker and is **not** in this branch; port it when it lands.

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
