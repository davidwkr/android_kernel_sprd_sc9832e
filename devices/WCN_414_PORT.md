# SC2342 Wi-Fi, Bluetooth and FM clients on 4.14

**Compile-only. Nothing here has run on a watch.**

The 4.14 tree has the WCN core of the integrated SC2342 (`drivers/misc/sprdwcn`: boot, platform, SIPC transport) but
not its three client drivers. They are ported from the 4.4 `drivers/misc/sprdwcn_bsp` tree, SIPC variants only, into
`drivers/misc/sprdwcn/clients/`:

| Module | Source (4.4) | Kconfig | Function |
|---|---|---|---|
| `sprdwl_ng.ko` | `sprdwcn_bsp/sc2332` | `SPRDWL_NG` | Wi-Fi, cfg80211 |
| `sprdbt_tty.ko` | `sipc_modules/bluetooth/driver/tty-sipc` | `SPRD_BT_TTY_SIPC` | Bluetooth HCI tty |
| `sprd_fm.ko` | `sipc_modules/fm/driver/fm_sipc` | `SPRD_FM_SIPC` | FM radio |

They are modules with the stock names, so the stock ramdisk's `insmod` lines load them (the boot image must carry
the 4.14 builds in place of the 4.4.83 ones, as the 4.4 candidates do). `watch_414.fragment` sets all three to `m`.
Their DT nodes (`sprd-wlan`, `wcn_bt`, `wcn_fm`) are identical in the stock and the 4.14 watch dtbs; all three link
against this tree's WCN core without unresolved symbols.

## 4.14 API changes (Wi-Fi only; BT and FM built unchanged)

- `NL80211_BAND_*`, `NUM_NL80211_BANDS`; `cfg80211_scan_info` for scan done.
- `cfg80211_roamed()` with `cfg80211_roam_info`, including the target bssid and channel (cfg80211 looks the BSS up
  by them when the firmware sent no beacon).
- cqm rssi level argument; scheduled scans: request ids, `max_sched_scan_reqs`, and the request stays active after
  `cfg80211_sched_scan_results()` until stop (abort paths run under RTNL; cfg80211 stops the scan itself there).
- `add/change_virtual_intf` without the (unused) flags argument; `netif_trans_update()`; `needs_free_netdev`;
  `ktime_to_ns()`; `nla_parse()` extack; `nla_put_u64_64bit()`.
- Generic netlink: ops in the family. The NPI (factory test) family id is now allocated by the kernel; userspace that
  sends to the old fixed id 1022 instead of resolving the name `SPRDWL_NL` will not reach it.
- `linux/wakelock.h` is gone: the drivers use their local `wakeup_source` wrapper.

## SIPC rx wakelock (battery)

Ported from 4.4: `sbuf_set_rx_wakelock_timeout()` (per-ring hold time, optional release on read). spipe uses 100 ms,
release on read, for the GNSS data pipe (`sbuf-4-4-0-rx` was held 507 s of the first 575 s after boot on stock);
`sprdbt_tty` sets its rx ring to 200 ms.

## Security fixes (also applied to the 4.4 modules, branch `security/wcn-clients`)

Four review rounds (gpt-6.1-sol). Everything below was inherited from the vendor drivers:

- GSCAN: bucket/channel indices and the bucket count bounded, declared counts checked, parse errors propagated; the
  cached-result expiry no longer computes a negative copy size; the cache is locked (`gscan_lock`) between the vendor
  commands and the result path; `gscan_res` fully zeroed.
- Vendor commands: typed nla policies (GSCAN, valid channels, LLSTAT clear, SAR), length check in the cached-results
  attribute loop, no reply after a rejected attribute, country code bounded to its attribute.
- NPI genetlink: only sprdwl interfaces, IFINDEX as u32, `CAP_NET_ADMIN`, command header required, no reply from an
  uninitialized buffer, no unbalanced vif reference release.
- Private ioctls (`SIOCDEVPRIVATE`, not permission-checked by the core): `CAP_NET_ADMIN`, one bounded, zero-padded copy
  helper, whitelist entries bounded by the data present, `str2mac()` rejects partial MACs, terminated 11V reply,
  per-subtype minimum lengths.
- `sprdbt_tty`: the HCI parser and the dump ring live for the driver lifetime (every close used to free them while
  other opens could write), the parser is reset on the first open and run under a mutex.
- `sprd_fm`: command responses read under the mutex with the caller's buffer size, receive packets bounded to the
  response buffers, `read()` requires a full-size buffer.

Accepted (not fixed):
- Unbinding the BT or FM platform driver (root, through sysfs) while a file is open leaves the open file pointing at
  freed state.
- `sprdbt_tty`'s `write` may sleep (the vendor `sbuf_write()` already did); a PPP line discipline on the BT tty would
  call it in atomic context. Needs the bluetooth user.
- Responses from the WCN CP firmware are trusted beyond the bounds above.

## First-boot checks

`lsmod` (or the init log) for the three modules, `wlan0` up and scanning (`iw dev wlan0 scan` or the Wi-Fi settings),
Bluetooth enable/scan, FM tune; `/sys/kernel/debug/wakeup_sources` for `sbuf-4-4-0-rx` while GNSS runs.
