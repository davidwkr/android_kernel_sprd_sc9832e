# DM63 boot anomalies (bench captures, 4.4.302)

Source: the bench captures `dm63-v4-014150`, `dm63-v3wipe-012847`, `dm63-loop-005219` (38 boots) and the older
`dm63-v3-*`. The dmesg files are ring-buffer tails (about 2000 lines, early boot cut off), so counts are per capture,
not per boot. Nothing here needed a device.

## 1. `i2c-N: i2c report: no ack error`

`I2C_ADDR_CFG` holds `addr << 1` (`i2c-sprd-v2.c`), so the dumps decode as:

| Bus (DT) | Address | Who | Per boot | Verdict |
|---|---|---|---|---|
| i2c-0 (70500000) | 0x3e, 0x6e | camera HAL: `sprd_sensor_read_reg fail, addr 0x3e` through the sensor ioctl, 3 tries each, about 23 s after boot | 6 | candidate sensors from the HAL's table that are not fitted; userspace probing, not a kernel probe |
| i2c-2 (70700000) | 0x0c | `af6837` (`compass,af6837`), `probe of 2-000c failed with error -5` | 1 | chip not fitted; node and driver are in stock |
| i2c-3 (70800000) | 0x5a | `hyn_ts` (`CONFIG_TOUCHSCREEN_CST9217`), 2 tries | 2 | chip not fitted; node and driver are in stock |

The DT client nodes (address, compatible, status) of all five buses are identical to the stock DT, and the stock
`.config` has `AF6837`, `CST9217` and `QMAX981` built in too, so stock probes the same addresses
(`devices/dm63/SPEC.md` lists 0x0c, 0x12, 0x2d and 0x5a as "unbound" on the running stock unit). The 0x5a driver is
kept on purpose (`dm63_touch.fragment`: insurance for a Hynitron revision). No mismatch with stock, so nothing
changed. Optional: dropping `CONFIG_SENSORS_COMPASS_AF6837` and `CONFIG_TOUCHSCREEN_CST9217` from the DM63 fragments
would remove 3 dump blocks per boot at the cost of differing from stock; not done.

## 2. Repeated messages (more than 50 in one capture)

| Message | Max count | Verdict | Action |
|---|---|---|---|
| `zram: Compression failed! err=-22` (+ `Write-error on swap-device`, 682) | 1364 (v3wipe-25), 216 (v4-13) | **real fault**: see section 3 | fixed |
| `spipe 5-4/5-6/5-21/6-6 not ready to open!` | 137 per capture | harmless retry: `refnotify`, `slogmodem` and others reopen `/dev/spipe_lte*` about once a second while the modem side (dst 5/6) is down; the open returns -ENODEV | `pr_info_ratelimited` |
| `sprd_adf_restruct_post_config: data size is ilegal` + `call restruct_post_configs failed` | 136 | real, already fixed by `adf: accept the stock hwcomposer's 48-byte layer records` (`ADF_STOCK_HWC.md`); absent from the v4 and v3wipe captures, present in older loop/boot2 builds | none |

Below 50 but repeating: `sblock_receive:sblock-5-17 not ready!` + two `[saudio]` lines (45 each, the audio
service polling the absent modem), `dfs_inquire, inquire fail:1` and `ddrinfo_freq_table_show, inquire fail:-1` (30
each, polled by a user-space reader). Candidates for `pr_*_ratelimited` if they ever cross 50; not changed.

## 3. Other anomalies

* **zram incompressible pages (fixed).** `e96acb3c31c1` (lz4 v4.11 API) made `zcomp_lz4_compress()` call
  `LZ4_compress_default(.., PAGE_SIZE, PAGE_SIZE, ..)`. A page that does not shrink returns 0, mapped to -EINVAL, so
  `zram_bvec_write()` fails, the swap write errors and the page is lost to the swapper. The old `lz4_compress()` wrote
  into the two-page `zstrm->buffer` and zram stored the page uncompressed. Output capacity is now `2 * PAGE_SIZE`.
  `crypto/lz4.c` was converted in the same commit and is not used by zram here (`CRYPTO_LZ4` is off).
* `ttyBT: probe of wcn_fm failed with error -22` (2 to 3 per boot): the BT tty driver and the FM driver both match
  `sprd,wcn_internal_chip` (`tty.c:663`, `fmdrv_ops.c:275`); the BT tty probe returns -EINVAL when `stty_driver_init()` fails
  for the `fm` node (cause not traced). Same compatible in the stock DT; FM/BT are unaffected on the captures. Message comes from the driver core, so it cannot be rate-limited from the driver.
* `trusty: Error: [-13] opening storage session` / `KEYBOXTOOLS ... file.error() = -13` (attestation key and cert
  length reads): secure storage has no attestation provisioning on these bench units. Not a kernel fault; stock shows
  the same when the storage is empty (unverified on this unit).
* `WCN: WCND at cmd read:ERROR!` (2): wcnd command reply from the WCN daemon at boot; Wi-Fi/BT still came up.
* `binder: ... DecRefs 0 refcount change on invalid ref 6 ret -22`: an app releasing a dead handle, userspace.
* The SELinux `avc: denied ... permissive=1` lines come from the bench shell (`su` domain), not the watch's own
  services. The unit is permissive.
* No `WARNING:`, `Call trace`, `BUG:` or oops in any capture.
