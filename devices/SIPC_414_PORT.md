# SIPC peer-hardening port, 4.4 to 4.14

Compile-only port, nothing has run on a device.

Threat model: the modem and WCN peer CPUs write ring headers, pointers and
block descriptors in shared memory. A compromised or buggy peer must not be
able to make the AP kernel read or write out of bounds, or copy peer-sized
data to user space. Reports about problems found while doing this go to the
security team.

## Flavour built by the watch config

`devices/merge_watch_414.sh` (sprd_sharkle_defconfig + watch_414.fragment)
gives `CONFIG_SPRD_SIPC=y`, `# CONFIG_SPRD_SIPC_V2 is not set`, so
`drivers/soc/sprd/modem/Makefile` descends into `sipc/` (v1). `sipc_v2/` is
not built and was not touched. Built objects in `sipc/`: sipc, smsg, smem,
sbuf, sblock, sipc_debugfs (always), spipe, spool, seth, sipx, swcn_blk
(all enabled: SPIPE, SPOOL, SETH, ZERO_COPY_SIPX, SWCNBLK). The WCN user
`drivers/misc/sprdwcn/sipc/wcn_sipc.o` is built (`CONFIG_WCN_SIPC=y`).

All paths below are under `drivers/soc/sprd/modem/sipc/` unless noted. The
4.14 files are the same code base as 4.4 with different includes, wakeup
source handling, exports and `volatile struct` ring types, so every 4.4 change
had a direct counterpart.

## Per-commit verdicts

| 4.4 commit | 4.14 equivalent | Verdict |
|---|---|---|
| f162912575a8 sbuf bound ring indexes, length, peer pointers | sbuf.c: sbuf_write, sbuf_read, sbuf_poll_wait, sbuf_register_notifier | PORT. None of the checks existed (bufid vs ringnr, INT_MAX clamp, rdptr/wrptr occupancy). 4.14 sbuf_poll_wait returns negative errno as a mask, so the new check returns -EINVAL like its neighbours. |
| 423f1d240ef7 sbuf AP-side ring sizes, validate event bufid | sbuf.c: sbuf_create_ex, sbuf_thread, sbuf_write/read/poll_wait; sbuf.h: struct sbuf_ring | PORT. Ring sizes were still read from the shared header. |
| d6e0b49271dc sbuf snapshot peer pointers once per copy | sbuf.c: sbuf_write, sbuf_read | PORT. Rx/tx wakeup_source code is outside the hunks. |
| fb18c4753669 sbuf notify on fresh pointers | sbuf.c: sbuf_write, sbuf_read | PORT. |
| 24d3aa9232e9 spool validate file offset | spool.c: spool_read | PORT. Same default_llseek exposure. |
| d76b0879a628 spool never copy peer-sized block | spool.c: spool_open, spool_read | PORT. Limit is DT rxblocksize aligned to sizeof(long) (equals SBLOCK_ALIGN_BYTES, what sblock_create_ex rounds to). |
| ead763f0bb05 sblock validate descriptors/pointers | sblock.c: sblock_put/get/send_ex/receive/release/recover/create_ex; sblock.h: struct sblock_mgr | PORT. 4.14 additionally has sblock_send_prepare/finish, which go through sblock_send_ex and are covered. |
| b015b15e0e5e swcn_blk validate | swcn_blk.c: swcnblk_put/get/send_ex/receive/release/recover/create, SWCNBLK_GET_BLK_MGR; swcn_blk.h | PORT. File exists and is built. The lookup macro also indexed swcnblks[][] after an invalid channel; fixed as part of this commit. |
| c9e6af71e9b9 sipx validate fifo entries | sipx.c: sipx_get_item_from_pool/ring, sipx_drain_ring (new), sipx_recover_channel | PORT. |
| ef30cf667535 skip bad descriptors, ignore tx pool length, local bases | sblock.c, swcn_blk.c, sipx.c (same functions, plus create_sipx_pool_ctrl/create_sipx_ring_ctrl) | PORT. |
| 573be79c6fc4 AP-side layout bases, barriers, short wcn blocks, sipx block return | sblock.c, swcn_blk.c, sipx.c, sipx.h; `drivers/misc/sprdwcn/sipc/wcn_sipc.c: wcn_sipc_sblk_recv` | PORT. 4.4 `sprdwcn_bsp/sc2332/sipc.c` has no 4.14 counterpart (N/A for that file). `sprdwcn_temp/sipc/wcn_sipc.c` has the same underflow but is not in any Makefile; left alone. |
| 9fde531f692e write pool array in *_put, dl record pools, sipx drain order | sblock_put, swcnblk_put, sipx_recover_channel, sipx_drain_ring | PORT. 4.14 had the same r_txblks/p_txblks and swapped-pool defects. |
| bce857662946 snapshot headers once in debug dumps | sbuf_get_status, sbuf_debug_show, sblock_debug_show, swcnblk_debug_show, sipx_debug_show | PORT. |

PRESENT: none. N/A: only the sc2332 file named above.

## Not in the 4.4 series, looks unsafe, not fixed

- smsg.c smsg_irq_handler: the rx loop runs while the peer-written wrptr
  differs from rdptr, with no bound on the occupancy. A wrong wrptr keeps
  the handler spinning for up to 2^32 messages. Slot index is masked, so
  this is a hang, not an out-of-bounds access. smsg_send/smsg_send_irq test
  occupancy against the peer rdptr the same way (overwrite, not OOB).
  Fixed separately (`kernel-4.14-smsg-bound`): at most rxbuf_size messages per
  interrupt, with a rate-limited warning. The 4.4 watches build the
  CONFIG_SPRD_MAILBOX_FIFO handler (one message per call), so they do not have
  this loop.
- smsg.c: the message channel index is `channel2index[msg->channel]` with a
  u8 channel and a 256-entry table (fine today, depends on SMSG_CH_NR=255).
- sblock.c sblock_get_arrived_count / sblock_get_free_count,
  swcn_blk.c and sipx.c equivalents (sipx_get_ring_item_cnt etc.): return
  peer-derived pointer differences unclamped. Callers (saudio) compare them;
  no memory access depends on them in the sipc code.
- sipx.c sipx_put_item_to_ring / sipx_put_item_back_to_pool: the full/empty
  decision uses the peer-written rdptr. A bad value can overwrite entries the
  peer has not consumed, or lose a block; not an out-of-bounds access.
- sbuf.c sbuf_poll_wait: the POLLIN/POLLOUT tests use raw peer pointers
  (only a wrong readiness answer); error paths return negative errnos
  through an unsigned mask (pre-existing 4.14 quirk).
- sbuf.c sbuf_read/sbuf_write: ring contents are copied without any
  per-message validation by design; payload of an owned block can always be
  changed by the peer.
- sipc_v2/ is not built and carries none of these fixes.
- seth.c rx path: relies on sblock_receive() clamping blk.length (now done);
  no independent check.
