# Status and handover (2026-10-30)

Where the kernel work stands, what was reviewed, what is open. Details live in the other docs:

- [`TODO.md`](TODO.md): the plan, item by item.
- [`KERNEL_TWEAKS.md`](KERNEL_TWEAKS.md): battery, flash-write, GNSS, charging and AOD changes, with hardware test steps.
- [`SECURITY_BACKPORTS.md`](SECURITY_BACKPORTS.md): stable and CIP merges, targeted backports, UNISOC bulletins, hardening config.
- [`devices/`](devices/README.md): per-device specs, stock configs and device trees.
- [`devices/CONFIG_PARITY.md`](devices/CONFIG_PARITY.md): every stock config option our builds lacked, classified (enabled, optional, kept off).

**Nothing in this tree has run on a watch yet.** Everything below is built and code-reviewed only. The first booting build (TODO item 7) has not been done; the tools for it (configs, DTs, boot-image and ramdisk repack) are ready.

## Branches

All finished work is merged into **`security-backports`** (head `3ae1686a4`). The topic branches are kept for history:

| Branch | Content | Merged |
|---|---|---|
| `drivers/hrs3xxx-heartrate` | HRS3xxx heart-rate input driver (no HR algorithm: closed vendor code) | yes |
| `drivers/cst0xx-touch` | DM63 CST0xx touch, rewritten DW100U `hyn_ts` | yes |
| `tweaks/battery-wakeups` | GNSS / BT rx wakelocks, PM print thread opt-in | yes |
| `security/hardening` | `watch_hardening.fragment`, `watch_release.fragment` | yes |
| `security/cip-4.4-st` | CIP 4.4 post-4.4.302 fixes | yes |
| `security/unisoc-bulletins` | UNISOC bulletin fixes | yes |
| `tweaks/gnss-power` | GNSS stop-path analysis, `gnss_engine_state` sysfs | yes |
| `tweaks/aod-idle` | DM63 `aod_idle` ported in tree | yes |
| `tweaks/aod-dw100u` | `sprd_aod_idle` also on the DW100U (`hyn_ts`, `dw100u_aod.fragment`) | yes |
| `tweaks/charge-cv` | DM63 `batt_idle` CV override in the SC2721 charger driver | yes |
| `drivers/otg-role` | `USB_MUSB_SPRD_ROLE_SYSFS` (`watch_otg.fragment`): host/device switch, replaces `otg_role.ko` | yes |
| `devices/mt107-mt108-dm82`, `devices/new-board-support` | MT107, MT108, DM82 specs, fragments, HRS3300 ID, `qma6981` fixes | yes |
| `tools/bootimg`, `tools/ramdisk-modules`, `tools/watch-build` | `tools/sprd_bootimg.py`, `tools/sprd_ramdisk.py`, `scripts/watch_build.sh` | yes |
| `fixes/gnss-failpaths` | GNSS failed-start unwind, retry rate limit, state guards | yes |
| `drivers/och1970` | OCH1970 Hall magnetometer driver (from the public datasheet) | yes |
| `tweaks/otg-idle` | `host_idle_timeout` for USB host mode | yes |
| `security/sipc-peer` | SIPC sblock/swcn_blk/sipx/spool checks against a misbehaving modem peer | yes |
| `tweaks/gauge-resync` | `batt_idle` fuel-gauge resync during the CV hold | yes |
| `devices/dts-in-tree` | per-watch DTS in tree, `tools/dtcompare.py`, `tools/dts_label.py` | yes |
| `security/ion-lifetime` | ION buffer lifetime (probable CVE-2023-49672), all in-tree callers | yes |
| `tools/first-boot` | `scripts/first_boot.sh` (backup, recovery-only flash, verify, restore) and a fake-adb test suite | **no**: latest fixes uncommitted, test suite not run yet |
| `drivers/goodix-fp` | Goodix fingerprint | **no**: empty, needs the vendor GPL source |
| `backup/security-backports-pre-trailer-strip` | backup ref from before the commit-trailer cleanup | – |

**Push state:** `origin/security-backports` is at `18f4c2ac8` (4.4.302 stable merge docs). 1467 commits since then are local only. The fork is public: check before pushing that no device data is in the tree. A few CIP commits carry `Assisted-by:` trailers written by their upstream authors; they are kept as upstream wrote them. Commits made for this project carry no tool attribution.

## Review process

Each work item went through: implementation, a first code review, fixes, then an independent final review, repeated until both reviews passed. Findings that changed the code are listed per item below.

## What was done

### Battery (KERNEL_TWEAKS.md section 1 and 3)

- **GNSS rx wakelock** (`sbuf-4-4-0`, `sttygnss0`): per-ring `rx_wake_timeout` and release-on-read in `drivers/staging/sprd/modem/sipc/sbuf.c`. The GNSS ring holds the CPU at most 100 ms per notification (stock 500 ms) and releases it about 20 ms after the HAL drained the ring, never while unread data is left. A per-ring spinlock makes the drained check and the release atomic against the notification thread, which arms the hold before waking readers. Other rings and the tx path are unchanged.
  - Review fixes: release raced with a new notification (could drop the hold with data pending); state cleared on partial reads; readers woken before the hold was armed; an empty read with no notification took the lock.
- **BT rx wakelock** (`sprdbt_tty`): 200 ms instead of 500 ms; the setter is looked up with `symbol_get()`.
- **`pm-sharkle` PM print thread:** opt-in, off by default.

### Security (SECURITY_BACKPORTS.md)

- 4.4.147 → **4.4.302** stable merge (per-release commits), f2fs excluded from 4.4.172 on.
- Targeted backports: TCP SACK, epoll, `seq_file`, `af_unix` GC, `fget`; Mali CVE-2022-38181 and CVE-2023-4211; binder own-process guard, `sender_euid` and `getsecid` from the cred.
- **CIP 4.4-st after 4.4.302:** about 1257 fixes for code this config builds (f2fs excluded); hand adaptations marked `[ backport: ... ]`.
- **Hardening:** `watch_hardening.fragment` for every build (hardened usercopy, strong stack protector, read-only data, SW PAN, `DEBUG_LIST`, dmesg and perf restricted, BPF JIT always on, unprivileged `bpf()` off, no user namespaces, no `/dev/mem` / `/proc/kcore`). KASLR and KPTI are left as in `sprd_sharkle_defconfig` (on). `watch_release.fragment` (no debugfs or ftrace, SELinux not switchable to permissive) is **not ready** until the SELinux policy runs enforcing and a release build is checked on each device.
- **UNISOC bulletins:** fixes in sysdump, ION custom ioctls and helpers, SIPC sbuf/spool, cproc/modem-control mini-dump. Only the sysdump defect is a confident match; the others are "probable" (UNISOC publishes one-line descriptions only). Table in SECURITY_BACKPORTS.md.
- **Bugs found during review, fixed here:**
  - `470aacefb` USB RNDIS gadget: 32-bit wrap in `rndis_rm_hdr()` let a USB host loop the receive path forever (DoS). **Stock firmware is affected.** Escalation to the security team is pending (draft kept outside this public repo).
  - `f1c3dc049` musb host: `qh->is_ready` written through a possibly freed `qh` after giveback (left over in the upstream fix too).
  - `9b8774cc4` `read_pnet_rcu()` / `write_pnet()`: keep RCU load/publish semantics (matters with `CONFIG_NET_NS=y` only).
  - `fb18c4753` sbuf: the peer notification is decided on fresh pointers (an earlier snapshot change could lose a wakeup and hang a modem channel).
  - SIPC (`security/sipc-peer`): sblock, swcn_blk, sipx and spool used modem-written block addresses, lengths and ring pointers without checks; now validated against AP-side values, bad entries skipped, barriers added. Three older vendor bugs fixed on the way: `*_put()` wrote `r_txblks` instead of `p_txblks`, sipx DL recovery swapped the ack and normal pools, and a `HEAD_RESERV` length underflow in `wcn_sipc` / `sc2332`.
  - ION (`security/ion-lifetime`): buffer lifetime use-after-free in every in-tree caller (vsp, jpg, vpp, cpp, isp, dcam_if, cam_sl5). Also fixed: camera CPP rotate/DMA with an empty buffer programmed address 0, which the IOMMU out-of-range bypass turns into a DMA write to physical address 0 (present in the vendor code); unchecked user offsets in CPP. Notes for the security team and hardware test items in SECURITY_BACKPORTS.md.

### Item 5: GNSS, charging, AOD

- **GNSS** (`drivers/misc/sprdwcn_bsp/sc23xx/`, the driver actually built): the engine stop powers the GNSS core, rail and WCN domain down when Wi-Fi and BT are off (no change needed). New read-only `/sys/class/misc/gnss_common_ctl/gnss_engine_state` (`off`, `transition`, `assert`, `error`, `sleep`, `on`, `unknown`; `off` is the driver's control state). Failure paths fixed (`fixes/gnss-failpaths`): a failed start unwinds exactly what it acquired, restarts are rate limited (30 s, burst 3, up to 600 s, boottime), `gnss_engine_state` reports `error` after a failed start.
- **DM63 charge CV override** (`drivers/power/sprd_2721_charge.c`): replaces `batt_idle.ko`. Off by default; `echo 4100 > /sys/module/sprd_2721_charge/parameters/cv_override_mv` (or cmdline / DT `sprd,cv-override-microvolt`). With the override on, the driver models the CV register exactly and programs the highest representable CV at or below min(request, `chg-end-vol`, override); unreachable values are rejected at set time. Override off: stock path unchanged. Fuel-gauge resync ported (`gauge_resync`, off by default; KERNEL_TWEAKS.md section 4). The safety timer still stops charging briefly every 90 min after the first 6 h (same as `batt_idle`).
- **DM63 `aod_idle`** (`drivers/video/adf/sprd/drv/sprd_aod_idle.c`, `CONFIG_SPRD_AOD_IDLE`, `dm63_aod.fragment`): same sysfs and `aod_dtap` interface as the stock module, real structs instead of offsets. Idle-mode commands are DCS writes (as stock); `idle_state` is cleared only when the panel is re-initialised at unblank. As a module it is `sprd_aod_idle.ko`: the DM63 `aod_idle.rc` needs the new name, or build it in. On the DW100U the same driver also matches `hyn_ts` (`dw100u_aod.fragment`; CO5300 command set equal to the DM63's per the stock DTs); untested, userspace helper/overlay to be written.
- **USB host mode** (`drivers/usb/musb/musb_sprd.c`, `CONFIG_USB_MUSB_SPRD_ROLE_SYSFS`, `watch_otg.fragment`): `/sys/kernel/otg_role/role` (`host`/`device`) as the stock `otg_role.ko`, but in the glue with real structs; same glue on DM63 and both DW100U. VBUS comes from the FAN54015 boost via `sprd_extic_otg_power()` (not verified on hardware); host mode holds `musb-sprd` so the watch does not suspend. Off by default; never plug a charger while in host mode. Idle timeout added (`host_idle_timeout`, default 300 s: back to device mode when nothing is enumerated). Hardware test pending (KERNEL_TWEAKS.md section 5); suspend in host mode is still open (TODO.md).
- **OCH1970** (`drivers/input/misc/och1970.c`, MT107/MT108/DM82 fragments): new driver from the public datasheet; build-checked only.
- **Device trees:** `sp9832e-1h10-<device>.dts` for all six watches, byte-identical to the stock DTBs (`tools/dtcompare.py --strict-order`).

## Build

Cross compiler `aarch64-linux-gnu-` (GCC 16). Output goes outside the repo (default `../watch_out/DEVICE/`).

```sh
scripts/watch_build.sh DEVICE [OUTDIR]   # DEVICE: dm63 dw100u-silver dw100u-v13m mt107 mt108 dm82
```

The script runs `sprd_sharkle_defconfig`, merges `watch_base`, `watch_stock_parity` (stock-config options we were missing, see [`devices/CONFIG_PARITY.md`](devices/CONFIG_PARITY.md)), `watch_hardening` and the device fragments (dm63: `dm63_touch watch_hrs watch_sc7a22 dm63_aod`; dw100u-*: `dw100u_touch watch_hrs watch_sc7a22 dw100u_aod`; mt107, mt108, dm82: their own fragment), runs `olddefconfig`, checks that every fragment line took effect (fails listing mismatches), builds `Image` and modules, then builds the ramdisk modules `mali`, `sprdbt_tty`, `sprd_fm` against the same `O=` (`sprdwl_ng` is in-tree `=m`; never mix configs), cleans the module source dirs and copies `Image` + the four `.ko` + `sha256sum.txt` to `OUTDIR/DEVICE/`. `ARCH`, `CROSS_COMPILE`, `JOBS` are overridable by environment. `watch_release` (not ready) and `watch_otg` (off by default) are never merged.

Last full build of `security-backports` (head `3ae1686a4`, 2026-10-30): all six devices rc=0; the four modules are stripped with `--strip-debug`. One older warning remains (implicit `tty_schedule_flip` in the BT `tty-sipc` driver).

## Open

- **MT107, MT108, DM82:** specs added from the stock boot dumps (TODO section 8b); per-device fragments `mt107/mt108/dm82.fragment` added (build-checked); `hrs3xxx` accepts the HRS3300 ID (probe and standby only, no measurement, as for the HRS3918); the vendor `qma6981` driver's probe stack overflow is fixed; Betterlife touch, BD1xxx, OCH1970 and MSP891 drivers are open (no GPL source found).
- **First boot** (TODO 7): needs the owner's choice of watch and OK for backup and the recovery flash. `scripts/first_boot.sh` (branch `tools/first-boot`) is not merged: its latest review fixes are uncommitted because the fake-adb test suite run was blocked by a local tool hook; it has to be run first.
- **Hardware tests** for every change above (steps in KERNEL_TWEAKS.md and SECURITY_BACKPORTS.md section 6). Most important: GNSS fixes continuous at 1 Hz with the CPU suspending; charge override on a supervised DM63; AOD idle shows power mode `0xdc`.
- **Security, not fixed:** Mali CVE-2021-28663, CVE-2021-28664, CVE-2022-36449 (need Arm's fixes); UNISOC camera, WCN and WLAN CVEs not located; dcam_r6p0 passes a kernel `sg_table` pointer to user space and back (not built in our configs; needs a uapi change); the iommuex out-of-range bypass and stale IOVA reuse after a failed unmap (vendor design); unlocked `s_isp_pipeline[id]` in `sprd_isp_path_set` (unbuilt ISP variants). For the security team, see SECURITY_BACKPORTS.md.
- **KASLR:** check on hardware whether the bootloader passes a seed; if not, dropping KASLR and KPTI saves battery on the A53.
- **Charging:** check that SELinux lets init write the override parameter.
- **Heart rate:** the driver reports no data; the HR/SpO2 algorithm is closed vendor code. Next step is a GPL source request to the vendor.
- **Fingerprint:** Goodix driver needs the vendor GPL source (request draft kept outside this repo).
- **Push** to the public fork after a final check, only on the owner's OK.
