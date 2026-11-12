# DM63 on 4.14: first hardware boot, kernel evidence

Inputs: the dmesg and logcat captures of the first 4.14 boots (redacted), the stock DM63 boot image (4.4.83 kernel,
DTB and ramdisk, unpacked read-only) and the 4.4 working tree. Source-level comparison only: nothing here was
reproduced on the device. Sections 1-4 are the first analysis (no kernel change); section 5 records the fixes made
afterwards and what is still open. Tags: [D] seen in a capture, [S] seen in source or stock image, [H] hypothesis.

Caveat on the captures: `dmesg-latest.txt` and `logcat-latest.txt` are rolling tails that kept growing while this was
written, and the early boot (first ~30 s, where probe messages live) is not in them. `pstore-prev.txt` is the
console of the previous boot, which was the 4.4 kernel (it contains `sprd_set_reboot_mode`, a 4.4-only function),
so it says nothing about the 4.14 reboot.

## 1. Trusty / keystore

The premise "4.14 has no Trusty" does not hold.

- `drivers/trusty` is the same driver as in 4.4 apart from the cpuhp and virtio-ring API ports (`trusty-irq.c`,
  `trusty-virtio.c`, `trusty.c` return types) [S]. The device node is created as `trusty-ipc-%s` from the virtio
  device name, i.e. `/dev/trusty-ipc-dev0`, as on 4.4 [S].
- The merged build config (`sprd_sharkle_defconfig` plus the four watch fragments) has `CONFIG_TRUSTY=y`,
  `TRUSTY_LOG=y`, `TRUSTY_VIRTIO=y`, `TRUSTY_VIRTIO_IPC=y`, `VIRTIO=y` [S].
- The `trusty` node of `sp9832e-1h10-dm63.dts` (smc, irq, virtio, log) is the same as the stock/4.4 one; the irq
  templates and ranges decode to identical cells [S].
- The stock ramdisk's `ueventd.sl8541e_1h10.rc` has `/dev/trusty-ipc-dev0 0660 system system` and
  `ueventd.rc` has the `trusty_version` sysfs rule; nothing else Trusty-specific [S].
- The captures do contain `trusty:` lines: the TEE log is forwarded by `trusty-log` (`welcome to lk/MP`,
  `trusty_gatekeeper_ta`, `crypto-ipc`, verified-boot `avb` messages, `IFAA` handling a request from the normal
  world, `there is no public key yet, give you fake data` while keymaster starts) [D]. So the SMC channel, the TEE
  and the TIPC services are alive on 4.14. `dmesg-final.txt` has no `trusty` line because it is a later window of
  the ring buffer (starts at 36 s) [D].
- The newest logcat shows keystore working: `addRngEntropy`, `generate key`, `attest key` all return rc = 1 [D],
  `system_server` (pid 602) stable for the whole capture.

What the captures do show around the keymaster failures [D]:

- `init: Service 'keymaster-3-0' / 'rpmbproxyd' / 'storagensproxyd' / 'refnotify' killed by signal 1` (SIGHUP), the
  keymaster ones in a 5 s cadence in `dmesg-final.txt`, i.e. a userspace restart loop, not a driver that never
  appears. The kernel does not send SIGHUP to these processes through any Trusty code path [S].
- TA side: `Error: [-13] opening storage session` and `SecureStorageRead ... attest.ecdsa.* failed` while the
  storage proxies are down. The Trusty secure-storage path needs `rpmbproxyd` / the storage proxy to be running.
- In the same window the eMMC reports `mmcblk0: error -84 transferring data` (CRC), `CMD25 int 0x1008001` with an
  ADMA dump, `mmc0: cache flush error -110` and one `I/O error, sector 0` (see 4).

Unknown: why the proxies receive SIGHUP; whether `/dev/trusty-ipc-dev0` exists at that moment (no `ls /dev`
capture); what the very first boot looked like (not in the captures).

## 2. Mali r21p0

- The gpu node of the built DTB equals the stock one: three interrupts `GIC_SPI 39` level high named JOB, MMU, GPU
  (stock has the same three identical triples), the same 7 clocks in the same order (gate, mux, 256/307.2/384/512
  MHz TWPLL, GPLL), the same dfs table, `system-coherency = <31>`, and `sprd,syscon-aon-apb`/`sprd,syscon-pmu-apb`
  [S]. One `/proc/interrupts` line `35: GICv2 71 Level 60000000.gpu x3` is therefore expected: it is one shared SPI
  with three handlers (kbase requests every IRQ with `IRQF_SHARED`) [S].
- The kbase platform glue (`platform/sharkle`) and the backend/irq code are byte-identical to the 4.4 r21p0 tree;
  only `mali_kbase_core_linux.c` (comment), `mali_kbase_devfreq.c`, `mali_kbase_sync_file.c` and `ipa/Kbuild` differ [S].
- `gpu-supply` is `&vddcore` (DCDC_CORE, 400000..1996000 uV, 3125 uV step, same as stock `dcdc_1` "vddcore") [S].
- `Continuing without Mali regulator control / clock control` and `Early backend initialization failed` are the
  probe-deferral paths: the device later logs `Probed as mali0` [D].
- `Reset interrupt didn't reach CPU` is printed by kbase only when `GPU_IRQ_RAWSTAT` has RESET_COMPLETED set, i.e.
  the GPU did complete the reset and the interrupt was not serviced [S]. The first soft reset (probe) worked.
- Timing [D]: the failures at 65.0, 67.8, 68.6, 89.5, 92.1 s each sit within ~0.3 s of an eMMC data-CRC/ADMA error
  (64.46 s CMD25 ADMA error, 65.33, 66.76, 67.80, 68.92, 89.76, 92.17 s `error -84`). A common cause (rail,
  DDR/bus load, clock) is more likely than a DT mistake in the gpu node [H].
- No `nobody cared` / `Disabling IRQ` message for IRQ 35 in the captures [D].

No DT or glue difference to stock or 4.4 was found, so none was changed.

## 3. Reboot

- 4.14 has no PSCI SYSTEM_RESET hook (`drivers/firmware/psci.c` has no restart code and `arm_pm_restart` is not
  set), so `machine_restart()` calls `do_kernel_restart()` and the only handler is the one in
  `drivers/spi/spi-sprd-adi.c` (priority 128, `sprd,sharkl3-adi` data = SC2721: status register 0xed8, module/clock
  enable 0xc08/0xc10, watchdog base 0x40) [S].
- That handler does what 4.4's `sprd_pmic_wdt.c` does (record the mode in `ANA_REG_GLB_POR_RST_MONITOR`, enable the
  PMIC watchdog clocks, unlock, load a 50 ms timeout, start it with reset enabled, relock), with the same mode
  values (normal 0x40, recovery 0x20, bootloader/fastboot 0x30, ...) [S].
- Differences [S]: 4.14 read-modify-writes the status register (clears 0xf0 only, so the sysdump bit 0x200 and low
  bits survive; 4.4 writes exactly the mode, 0x40 for `adb reboot`), and 4.14 has no `sprd_ddr_force_light_sleep()`
  call (4.4 only acts on it when the DT has a `sleep-ctrl` node; the DM63 DT has none, so it is a no-op there).
- `sprd_adi_write()` takes hardware spinlock 0; one `Hardware spinlock [0] lock failed!` (status 0x10) was logged at
  33.4 s from the battery work [D]. The restart handler ignores write failures [S].
- No capture of the 4.14 reboot itself exists (the pstore file is from 4.4) [D].

Unknown: whether the status register value the 4.14 handler leaves (0x240 if the bootloader had set 0x200) is what
the bootloader mishandles, or whether the reset never reached the PMIC. Nothing was changed.

## 4. Other errors and failed probes in the 4.14 captures

- `ion_carveout_heap_allocate: failed to alloc heap id: 0, size: 835584` x46: `sprd_ion_probe()` creates every
  DT heap with `ion_carveout_heap_create()`, including heap@0 "system" (type 0, no memory-region, size 0), next to the
  real system heap of `ion_system_heap.c`; two heaps share id 0 and the empty carveout fails first (the allocation
  then falls through to the system heap). 835584 = a 416x502x4 buffer rounded to pages (panel size). Noise, but
  the buffers of that size are system-heap, not carveout.
- `[iommu] sprd_iommu_map()-null buf pointer!`, `iommu addr error`, `iommu map failed`, `adf adf0: flip failed:
  -22` in a continuous stream (every capture): `sprd_dispc_iommu_map()` (same code as 4.4) fills `iommu_data.table`
  but never `iommu_data.buf`, and `sprd_iommu_map()` rejects a NULL `buf`. Layers in the carveout (reserved) heaps
  skip this path; layers on the system heap fail every flip. This is the most visible display problem; it is
  probably linked to the ION heap items above (which heap gralloc ends up with) [H].
- eMMC `mmcblk0: error -84`, `CMD25 int 0x1008001` with ADMA descriptor dump, `mmc0: cache flush error -110`,
  `I/O error, dev mmcblk0, sector 0`: the delay line is the DT one (`hs400es final timing delay value 0x30307f5a` =
  `sprd,hs400es-dly = <0x5a 0x7f 0x30 0x30>`, same as stock). Data corruption at this rate can by itself kill
  userspace processes.
- `dfs_msg_send, dfs send failed, freq:0..3, cmd:1285` and `dfs_msg, dfs msg send failed: 505` (DDR DFS request to
  the other core fails).
- `WCN BASE warning: WCN module have not open`, `at cmd read:ERROR!` (x2), `saudio ... sblock-5-13/17 not ready`,
  `sblock_query:sblock-5-5 not ready`, `spipe 5-4 / 6-6 not ready to open` repeating: modem-side channels not up.
- `binder_alloc: ... no vma` / `no address space` (userspace dying/restarting).
- `Hardware spinlock [0] lock failed!` once (see 3).
- SELinux runs permissive in the captures (adbd in `su`); avc denials are not kernel failures.

## 5. Fixes after the analysis, and what is still open

Build-tested only (Image, modules, `sp9832e-1h10-dm63-adf.dtb`); none of it has run on the device.

### Fixed

- ADF flips of system-heap buffers (`adf0: flip failed: -22`). `sprd_dispc_iommu_map()` never set `iommu_data.buf`
  and `sprd_iommu_map()` rejects NULL [S]. The 4.4 ADF code has the same gap [S], so 4.4 only works because its
  gralloc buffers sit in the reserved fb carveout and take the `fb_reserved` branch; 4.14 gralloc lands on the system
  heap (see the ion item). The DRM dpu of this tree passes `dma_buf->priv` (the `ion_buffer`), which is what
  `sprd_ion_get_sg()` casts, so ADF now does the same and refuses non-ion dma-bufs. Unmap needs no change: it finds
  the buffer from the iova.
- ion carveout with id 0. `sprd_ion_probe()` made a carveout for every DT child, including heap@0 "system" (type 0,
  no region). 4.4 created heaps through `ion_heap_create()`, which maps type 0 to the system heap; 4.14 builds the
  system heap in `ion_system_heap.c` (heaps[0], id 0) and the probe only knows carveouts [S]. The probe now skips
  the system types and keeps heaps[1..n] without holes (the show-mem notifier walks 0..num_heaps).
- Restart. A non-panic restart now writes exactly the mode to the PMIC status register, as 4.4 `arch_reset()` does
  (clears 0x200 and any other bit). The panic mode keeps the read-modify-write. Independent of
  `fixes/sysdump-default-off-414`, which clears 0x200 at boot.

### eMMC errors, first 4.14 boot: root cause not established (see the next section)

What was compared and found equal to 4.4 and stock [S]: the `sdio@20600000` node of the built DTB (clock tree
entries, `sprd,hs400es/hs400/hs200/ddr52-dly`, `bus-width 8`, `mmc-hs400-enhanced-strobe`, 1.8 V HS400/HS200/DDR,
`cap-mmc-hw-reset`, `sprd,sdio-adma`, `no-sd/no-sdio`, `vmmc` = always-on vddemmccore); `sprd-sdhcr10.c` (diff to
4.4: only dev_* logging, the DLL lock loop no longer tests DLL_ERROR, `mmc_regulator_get_supply()` result checked,
`MMC_CAP2_HC_ERASE_SZ` removed because 4.14 has no such flag and always uses the HC erase size); the SDCLK source
(`emmc-2x` mux reg 0x90 with the same parent order, rpll-390m, divider math unchanged); the HS400ES switch sequence
of the core (4.14 only adds `mmc_select_driver_type()`, a no-op without host drive-type caps). The driver already
runs HS400ES at 200 MHz as 4.4 does, so there is no "safer 4.4 mode" to fall back to.

What the capture shows [D]:

- The first failure (64.46 s) has `INT 0x1008001` = command complete + error + Auto-CMD error, Auto-CMD error status
  0x0002 (Auto-CMD timeout), ADMA error 0, ADMA pointer still at the 2nd descriptor, trans-mode 0x2b (auto CMD23,
  multi-block write). So the auto CMD23 got no response and the data phase never started; it is not an ADMA fault
  and not a data CRC in the strict sense (`-84` is the driver's mapping of any Auto-CMD error).
- The same happens on reads (CMD18 at 91.3 s) and the re-init after each failure sees CMD6 timeouts (`0x18001`):
  the card stops answering commands in HS400ES, which points at the command/strobe path or the card state, not at
  one bad write.
- 0.86 s pass between the ADMA dump and the `-84` message. `irq_error_handle()` busy-waits for up to 10 M reads of
  INT_STATE in hard-IRQ context on CPU 0 (the driver pins the eMMC IRQ there). The GPU
  `Reset interrupt didn't reach CPU` at 65.01 s falls inside that window, so the Mali errors are probably a
  consequence of the eMMC fault, not an independent one [H].
- No eMMC error in the 28 s before 64 s, although userspace was already writing. The `dfs_msg_send` failures start at
  91.9 s, after the first eMMC errors, so they are not the trigger.

Differences left that could matter but are not proven [H]: (a) vddemmccore: the 4.4 regulator driver sets the
midpoint of the requested window (2.95 V for 2.9-3.0 V), the mainline-style 4.14 driver picks the lowest step (2.9 V);
(b) 4.14 builds `DEVFREQ_SPRD_AUTO_DFS` (SIPC requests to the DFS core, failing here), 4.4 has `DEVFREQ_SPRD_DFS`;
(c) the early boot (probe line with the real base clock, regulator and pinctrl messages) is missing from the
captures, so the actual SDCLK and voltage are unknown. A capture of the first 30 s with `mmc0`/`sprd-sdhcr10`
lines, or `/sys/kernel/debug/clk/clk_summary` and the `vddemmccore` voltage from `/sys/class/regulator`, would settle (c).

### eMMC write errors, second 4.14 boot (vddemmccore pinned 2.95 V)

Facts [D]: HS400ES at 200 MHz comes up at 1.55 s, reads and the ext4/f2fs mounts work; the first error is at 4.493 s,
`CMD25 int 0x1008001` on an 8-block write (sector 6512640), register dump `0x0C = 193a002b` (CMD25, auto CMD23,
multi-block, DMA), ARG2 (0x00) = 8, Auto-CMD error register (0x3C) = 0x0002 (timeout), ADMA error 0, ADMA still at
descriptor 2. So the controller sent the automatic CMD23 and got no response; the data phase never starts. Host ctrl2
(0x3E) = 0x3807. The pin to 2.95 V did not change the failure, so the supply voltage is not the cause.

Compared with 4.4 [S]: sbc is filled by the same core rule (`MMC_BLK_CMD23`, rel-wr bit 31, tag bit 29), the driver
has the same Auto-CMD23 path, `card_busy_detect()` and the CMD6/flush handling are unchanged, ADMA limits and the
runtime-PM clock gating are the same. `sdio_2x_eb`/`sdio_1x_eb` are optional and warn on 4.4 too; the eMMC gate is
`sdio_ahb_enable` (EMMC_EB). The one functional driver difference is the DLL lock loop: 4.4 waits for
`DLL_STS0` locked (bit 18) AND error (bit 17) clear, 4.14 only for locked.

Commits on `fixes/414-emmc-write`:

1. DLL wait restored to the 4.4 condition (most likely cause [H], moderate confidence: a DLL reporting an error
   while locked gives a wrong sampling phase for the response, which fits a CMD23 response timeout; it cannot be
   shown from the existing logs because the status was never printed).
2. Failed-command log: arg, block count, sbc and its argument, auto mode, `acmd_err`, host flags, clock, timing,
   `DLL_STS0/1`; "dpll locked done" now prints `DLL_STS0`.
3. `vddemmccore` window 2.95-3.1 V: the core requests 3.0-3.1 V (OCR bit 18 from `voltage-ranges <3000 3000>`),
   which the 2.95 V pin made fail with -EPERM (`could not set regulator OCR (-1)`). Hygiene fix, unlikely [H] to be
   the write failure (the first boot with the 2.9 V default failed the same way).

Next boot must show: `dpll locked done, DLL_STS0 0x...` (bit 17 = 0x20000 must be clear; any `dpll locked faile`
lines mean 4.4's loop was needed); no `could not set regulator OCR`; `/sys/class/regulator` vddemmccore at 3.0 V; and
either no `CMD25 int` or the new `CMD25 arg ... acmd_err ...` line. If errors persist, `acmd_err` 0x0002 with
`sbc arg` bits 31/29 set means reliable-write/tag CMD23s are the trigger; the same with a plain block count points at
the response sampling (retry with `sprd,hs400es-dly` re-tuned or HS400 without ES). A DLL_STS0 error bit with the
error persisting would make the DLL itself the suspect.

### Keymaster / rpmbproxyd / storagensproxyd

Not explained by the eMMC errors. `rpmbproxyd` dies with signal 1 at 38.2 s, `keymaster-3-0` at 49, 54, 59, 64 s and
every ~5 s after (init's restart delay), all before or independent of the first eMMC error at 64.5 s; only the
`storagensproxyd` death (65.326 s) lines up with an eMMC reset (65.325 s). A second `keymaster-3-0` instance is
started next to `tsupplicant` while pid 296 keeps working in logcat. No 4.14 kernel code sends SIGHUP to these
processes (no `send_sig`/`kill_pid` in drivers/trusty, drivers/soc/sprd), so the sender is userspace (vendor rc/HAL)
or the tty layer. Trusty error -13 is `ERR_TIMED_OUT` (lk error table): the TA's storage session could not reach the
storage proxy in time, which fits proxies that are down or restarting, and the RPMB partition is on the same eMMC, so
the eMMC faults will make it worse once they start. The vendor/system rc files are not in the boot image, so the
SIGHUP source needs the `/vendor/etc/init` files of the stock system image or an `strace -f`/`logcat -b all` from init.

### Open blockers for a clean boot

1. eMMC Auto-CMD/command timeouts in HS400ES (cause unknown, above).
2. SIGHUP loop of the Trusty client daemons (userspace sender unknown) and the resulting missing keystore/RPMB.
3. Modem side not up (`sblock/spipe/saudio not ready`, `dfs_msg` failures, `WCN module have not open`).
4. Display with the three fixes above is untested; `sprd_dispc_wb_buf_alloc()` still returns -ENOTSUPP.
5. Reboot: the normal path is untested; the `Hardware spinlock [0] lock failed!` from the battery work can also make
   the restart handler's ADI writes fail silently.
