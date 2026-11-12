# WCN (BTWF/GNSS CP2) boot handshake against the stock firmware

DM63 (SL8541E) runs the stock Android 8.1 `wcnmodem` / `gpsgl` / `gpsbd`
partitions with this tree's `drivers/misc/sprdwcn_bsp/sc23xx/` (the driver the
dm63 config builds: `CONFIG_SC23XX=y`; `sc2342/` is not built). The stock boot
image carries the older `sc2342`-style driver of kernel 4.4.83. This note
records what was compared and what is still open.

## Result

The BTWF boot handshake of this tree is functionally identical to the stock
one. No code difference that explains the variable CP2 ready time was found,
so none was "fixed" to match; the candidates are listed below.

## Method

Stock `boot.img` (copy) unpacked with `tools/sprd_bootimg.py unpack`; the
`kernel` is a raw arm64 `Image` (4.4.83, base `0xffffffc000080000`, file offset
0 = base). Format strings were located with `strings -t x`, their ADRP/ADD
references with a small scanner, and the code disassembled with
`aarch64-linux-gnu-objdump -D -b binary -m aarch64 --adjust-vma=...`. The stock
DTB was cut out of the unpacked `dt` and decompiled with `dtc`. The stock
`wcnmodem.img` was only read (never executed).

## Identical to stock

| Item | Stock (disassembly) | This tree |
|---|---|---|
| order | clear flags, load image, write cali data, ctrl regs, poll | same |
| ready flag | word at CP2 base + `0x17cf54`, ready `0xababbaba` | `marlin.init_status`, same |
| start marker | `0x5a5a5a5a` written to `0x17cf54` before the image load | same |
| poll | `msleep(0x14)` (20 ms), 256 polls (`cmp w26, #0x100`) | same |
| timeout | writes `0x88888888` to `0x17cf54`, status 2, "MARLIN boot cp timeout" | same |
| success | writes `0x13579bdf` to `0x17cf54`, `0x12345678` to `0x17cf50` | same |
| cali data | `0xf44` bytes at `0x17c000`, flag `0xefeffefe` at `0x17cf50` | same sizes (pinned by `BUILD_BUG_ON`) |
| efuse temp | magic `0x432ff678` at `0x17cf6c`, efuse 44 at `0x17cf70` | same |
| GNSS first | include-gnss `0`, gnss flag `0x450028`, start, stop, then BTWF | same |
| GNSS cali | poll GNSS base + `0x150028` for `0x1314520`, 256 x 20 ms | same |
| ctrl regs | DT list from probe-num, `usleep_range(d, d+40)` if d >= 10 | same |
| image read | `kernel_read(file, 0, buf, size)` loop, 16 open retries, `file-length` | same |
| power wq | vddcon, 10-30 ms, vddwifipa, sys domain, start | same |
| DT `cpwcn_btwf` / `cpwcn_gnss` | regs, masks, values, delays, `reg`, `file-length` | identical |
| partition | `wcnmodem.img` is zero from `0x156ccd`, file-length `0x178000` < `0x17c000` | no overlap with the shared area |

## Differences found (none explains the symptom)

* `wcn_sbuf_status(3, 4)` between "CP ready" and the success flag: new in this
  release, waits at most 1 s, only after the ready flag was seen.
* `wcn_power_enable_sys_domain()` has an extra `!enable &&` in its power-down
  branch (the stock-vintage `sc2342` source in this tree has no `!enable`; not
  disassembled in the stock image).
* GNSS cali poll printed before the check; no log at the GNSS timeouts; the
  stock timeout path dumps debug registers, this tree does not.
* Calibration dump to `/mnt/vendor/wcn/connectivity_calibration_bak.ini`
  (`cali_save_file(), open error!` in every capture): the stock kernel has no
  such file. Harmless but the calibration is never persisted.

## What the captures show

The flag at `0x17cf54` is not just ready/not ready: the CP2 firmware writes
stage markers (`0x23456789`, `0x45678901`, `0x56789012` are constants of
`wcnmodem.img`), then `0xababbaba`. Per boot (`dm63-boot2-010410`,
`dm63-loop-005219`, `dm63-v3wipe-012847`):

* fast boots: stages pass within 7-12 polls (0.3-0.4 s); `dm63-loop` and
  `v3wipe` first boots, and every second boot in the same session;
* slow boots (`boot2` try3/4/8/9/10): the CP2 sits in `0x45678901` for
  218-240 polls (6.9-7.7 s), then continues by itself; try3/try8 left the
  capture inside that wait;
* during the stall the CP2 has sent only the SIPC open for channel 3-5; the
  other channels (3-4, 3-7, 4-4, 5-x, 6-x) open all at once in the same
  millisecond the stage advances, so the CP2 is waiting for something in a
  roughly fixed 7 s window. The AP side of the handshake (flags, cali data,
  ctrl-reg writes) is the stock sequence and is complete before the stall.
* 22 polls of `GNSS: finish OK` is the GNSS cali, identical in fast and slow
  runs. `0x45678901` is therefore not the GNSS flag.

## Candidates (not confirmed)

1. A CP2 firmware stage that waits on a platform input with a ~7 s timeout:
   26M/XTL or PLL state (`wcn_sys_ctrl_26m`, PMU regs `0x338`, `0x1244`),
   vddcon/vddwifipa settle, or the PMU soft-reset/release pair around the
   first start. Compare the PMU/ANLG register dump (`wcn_show_debug_reg_info`
   of the sc2342 driver) between a slow and a fast boot; it is not in sc23xx.
2. Calibration input: first boots write the calibration flag (`0xefeffefe`) and
   the CP2 may calibrate when the ini data is not accepted; second boots skip
   it (`btwf_calibrated`) and are always fast. Check
   `/vendor/etc/connectivity_calibration.ini` against the stock ini.
3. SIPC: this tree's SIPC/smsg code is newer than the firmware's. The channel
   opens are bunched at the end of a stall; a lost or late doorbell would show
   exactly that. Needs a mailbox/IPI trace of a slow boot.
4. DDR bandwidth/frequency while the CP2 loads from `0x88000000`.

To separate them, capture a slow boot with the register dump and with
`wcn_print_level` raised; nothing else in the handshake differs.

## Log flood

The pollers printed one line per 20 ms poll (at least 241 lines in one boot) and
the ini parser one `[g_table]` line per table (about 100 per parse), all at
`pr_info`, on a 115200 console with `loglevel=7`: ~7 ms of console time per
line, plus logd. They now log the first poll, every 32nd poll and the timeout
with the last value (`pr_debug` otherwise). Still noisy and outside the WCN
driver: `spipe N-M not ready to open!` (131 per boot), `sblock_query ... not
ready` (40), and the display `restruct_post_config` pair (197 each, the ADF
layout fix on `fixes/adf-stock-hwc-44`).

## saudio

`saudio-5-14` does not spin. `saudio_ctrl_thread` blocks in
`sblock_receive(..., timeout -1)` for the handshake; a failed wait sleeps 1 s
before retrying and the modem-clear path is bounded by `CMD_MODEM_RESET_TIMEOUT`.
Per capture there are 6 `saudio_wait_monitor_cmd error -5` lines in total (one
per channel at start, before the modem is up), and the `top` snapshots show no
saudio task. The busy `irq/7-70700000` thread is not audio or WCN: 0x70700000
is `i2c@70700000` (accel `qmax981`/`sc7a22`, compass `af6837`); the captures
also show `i2c i2c-0 no ack error` dumps, a separate bus to triage.
