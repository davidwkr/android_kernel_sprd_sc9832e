# DM63 on 4.14: first hardware boot, kernel evidence

Inputs: the dmesg and logcat captures of the first 4.14 boots (redacted), the stock DM63 boot image (4.4.83 kernel,
DTB and ramdisk, unpacked read-only) and the 4.4 working tree. Source-level comparison only: nothing here was
reproduced on the device and no kernel change came out of it. Tags: [D] seen in a capture, [S] seen in source or
stock image, [H] hypothesis.

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
