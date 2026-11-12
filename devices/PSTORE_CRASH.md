# DM63 v5 panic at ~77 s: not pstore, `memset` in ALSA `hw_params`

Kernel evidence only. Source: console-ramoops of the crashed boot of the 4.4.302
image v5 (kernel commit 885eb904b7c9), dm63.

## Record

    Internal error: : 96000061 [#1] PREEMPT SMP
    Process writer (pid: 2934, ...)
    Code: 910?1?08 54ffff4a 8b040108 cb050042 (d50b7428)
    Kernel panic - not syncing: Fatal exception

ESR 0x96000061: data abort, write, DFSC 0x21 (alignment fault). Some characters
of the record are corrupted by RAM retention. `PC is at` / `LR is at` are not in
the record.

## Image match

The record prints `KTXT [ffffff8008081000--ffffff8008b4d000]`. A plain
`scripts/watch_build.sh dm63` of 885eb904b7c9 ends its text at `0x...8b34000`;
with `HRS_STOCK_BOOT_IMG` (HRS blob, see HRS3XXX_INTERFACE.md) it ends at
`0x...8b4d000`, which is the crashed image. All symbols below are from that
build.

## Resolved stack

Return addresses in the frame chain, innermost first (each one follows a
`bl`/`blr`):

| address | symbol |
|---|---|
| ffffff8008459400 (callee) | `__memset` |
| ffffff80088a2758 | `snd_pcm_hw_params` (`sound/core/pcm_native.c:593`) |
| ffffff80088a2940 | `snd_pcm_ioctl_hw_params_compat+0x170` |
| ffffff80088a6d70 | `snd_pcm_ioctl_compat+0x830` |
| ffffff80082a0b48 | `compat_SyS_ioctl+0x1a8` |
| ffffff8008085b30 | `el0_svc_naked+0x24` |

The `Code:` words are `add x8,x8,#0x40; b.ge; add x8,x8,x4; sub x2,x2,x5;
(dc zva, x8)`, which is `__memset+0x19c..0x1ac`: the zero-block loop of
`arch/arm64/lib/memset.S`. The faulting instruction is `dc zva, x8` at
`__memset+0x1ac` (ffffff80084595ac).

## Root cause

`snd_pcm_hw_params()` clears `runtime->dma_area` with `memset()` ("clear the
buffer for avoiding possible kernel info leaks", added by the 4.4.208 and
4.4.213 stable updates; stock 4.4.83 does not have it). The SPRD platform
driver (`sound/soc/sprd/platform/sprd-2stage-dmaengine-pcm.c`) provides that
area as an `ioremap_nocache` / `pgprot_noncached` iram buffer or a
`dma_alloc_writecombine` buffer. arm64 `memset()` zeroes large blocks with
`DC ZVA`, which raises an alignment fault on Device memory. The first
`SNDRV_PCM_IOCTL_HW_PARAMS` of a 32-bit audio process panics the kernel.

pstore is not involved. `fs/pstore/ram_core.c` (4.4 and 4.14) updates the ring
counters with `atomic_read()`/`atomic_set()` (plain `ldr`/`str`) under
`buffer_lock`, with no exclusive accesses, and writes the data with
`memcpy_toio()` / `copy_from_user`. The userspace log lines in pmsg-ramoops are
a coincidence of the same boot. Stock enables `CONFIG_PSTORE_PMSG=y` (extracted
from the stock Image), so it stays enabled.

## Fix

`sound/core/pcm_native.c`: clear `SNDRV_DMA_TYPE_DEV` / `SNDRV_DMA_TYPE_DEV_IRAM`
buffers with `memset_io()` (`__memset_io`: plain 8/1-byte stores); other buffer
types keep `memset()`. Same change on kernel-4.14 (`fixes/pstore-crash-414`).

Other `dma_area` accesses (`copy_from_user` into it, silence fills) are
unchanged from stock and not touched.
