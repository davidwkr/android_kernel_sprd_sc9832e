# Thermal protection, watchdog, TRUSTY, USB gadget and crown on 4.14

Scope: `kernel-4.14-thermal-wdt` branch, worktree `../kwt-414-thermal`. All changes are in
`arch/arm64/configs/watch_414.fragment` and `arch/arm64/boot/dts/sprd/sp9832e-mach.dtsi` (the
mach dtsi shared by all seven `sp9832e-1h10-*` boards: k18, dm63, dm82, mt107, mt108,
dw100u-silver, dw100u-v13m). Compile-only: nothing here has run on a device.

## Correction to the gap414.md config gap review

`/home/blackbox/.cache/qa-out/gap414.md` scoped its DTS check to
`sp9832e-1h10-k18.dts` + `sharkle.dtsi` + `sc2721.dtsi` and concluded "no `thermal-zones` node"
and that thermal protection "is not actually wired even if the Kconfig is flipped on". That
missed `sp9832e-mach.dtsi`, which every `sp9832e-1h10-*` board `#include`s and which **already
has** a fully wired `thermal-zones` node (`cpu-thmzone`, `gpu-thmzone`) plus a `cooling-devices`
node (`cluster0-cooling`), with cpu trip points and hysteresis byte-identical to the stock K18
and DM63 `cpu-thmzone` (70000/85000 m°C passive, 110000 m°C critical, hysteresis 1000/1000/2000).
Each board DTS additionally extends the shared `&thm_zone` with its own `board-thmzone` bound to
a `tboard_thermistor` `generic-adc-thermal` node built from that board's stock NTC table (K18:
`sp9832e-1h10-k18.dts:140`, DM63: `sp9832e-1h10-dm63.dts:129`) — this was also already done, not
part of this change. The DM63/DM82/MT107/MT108 stock `rotary-encoder` crown node was likewise
already present in the 4.14 board DTS (e.g. `sp9832e-1h10-dm63.dts:299`); K18 and the DW100U
variants have no such node, matching their stock DTs (no crown hardware).

So the actual gap was narrower than reported: one Kconfig flag for the CPU/GPU sensor, one real
DTS gap (GPU zone had no critical trip), and Kconfig-only flips for the watchdog, TRUSTY and the
USB gadget functions — nothing else needed inventing.

## Thermal

### cpu-thmzone (unchanged, already present and correct)

`sp9832e-mach.dtsi`'s `cpu_thmzone` already matches stock: passive trips at 70000/85000 m°C,
critical at 110000 m°C (hysteresis 1000/1000/2000 m°C), cooling-map to `cluster0_cooling`
(`THERMAL_NO_LIMIT`/`THERMAL_NO_LIMIT`, contribution 1024, `sustainable-power = <1000>`). This
needed no DTS edit, only the sensor driver Kconfig (below).

The `cluster0_cooling` node (`compatible = "sprd,cluster-cooling"`) is not matched by any
`of_device_id` table — `drivers/thermal/sprd_cpu_device.c:create_cpu_cooling_device()` finds it
by literally walking a node named `cooling-devices` and reading the `cooling-deviceN` alias
(`sp9832e-mach.dtsi:15`, `cooling-device0 = &cluster0_cooling`), then reads the `sprd,leak-*` /
`sprd,dynamic-*` / `sprd,*-scale` IPA power-model properties straight off that node. So the
cooling path is driver + DT, no of_device_id compatible match involved; `SPRD_CPU_COOLING` +
`SPRD_CPU_COOLING_DEVICE` gate `sprd_cpu_cooling.o` / `sprd_cpu_device.o` in
`drivers/thermal/Makefile`.

Mainline's generic cpufreq cooling (`CONFIG_CPU_THERMAL`, `drivers/thermal/cpu_cooling.c`) is
`depends on !ARCH_SPRD` in this tree's `drivers/thermal/Kconfig:144` — it cannot be turned on for
this SoC at all. The ranked list in gap414.md ("cooling maps (cpufreq cooling, CPU_THERMAL)")
and the task brief both assumed the mainline path; the correct, buildable path on `ARCH_SPRD` is
the SPRD driver pair above, which the DT was already written for.

### gpu-thmzone (DTS change)

Before this change `gpu_thmzone` had a sensor but no `trips` at all and `polling-delay(-passive)
= 0` (never periodically polled). Stock has passive trips at the same temperatures as
`cpu-thmzone` plus a cooling-map to a `sprd,mali-power-model` device
(`SPRD_GPU_COOLING_DEVICE`, `depends on DEVFREQ_THERMAL`). This tree has **no Mali/kbase driver
at all** (confirmed: no `drivers/gpu/drm/sprd/*kbase*` or equivalent, matches gap414.md's
"No Mali kbase GPU driver anywhere in the 4.14 tree"), so there is nothing to throttle and no
GPU devfreq device for `sprd,mali-power-model` to bind to; wiring that cooling-map would
reference a driver that doesn't exist and do nothing.

Added instead: a critical trip only (`gpu_crit`, 110000 m°C / 2000 m°C hysteresis, same value as
stock and as `cpu_crit`), and set `polling-delay-passive = <100>` / `polling-delay = <1000>` (same
as `cpu-thmzone`) so the zone is actually polled — a critical trip under a zone with 0/0 polling
delays and no interrupt-driven sensor update would never be evaluated. A critical trip needs no
cooling device: `thermal_core` calls `orderly_poweroff`/shutdown directly on crossing it,
independent of any cooling-map. No passive GPU trips were added, since a passive trip with no
bound cooling device is a no-op and would only add a misleading "looks wired" DT entry.

### Kconfig (`watch_414.fragment`)

```
CONFIG_SPRD_THERMAL=y
```

Everything this depends on transitively (`THERMAL`, `THERMAL_OF`, `SPRD_CPU_COOLING`,
`SPRD_CPU_COOLING_DEVICE`, `SPRD_DEDICATE_THERMAL`, `GENERIC_ADC_THERMAL` for the board NTC,
`THERMAL_GOV_POWER_ALLOCATOR`, `IIO`) was already `=y` in `sprd_sharkle_defconfig` — confirmed by
building the merged `.config` before this change and grepping each symbol. `SPRD_THERMAL` itself
(the `sprd,sharkle-thermal` driver for `cpu_thm`/`gpu_thm`, `drivers/thermal/sprd_thermal.c`) was
the only one off. `THERMAL_GOV_STEP_WISE` is correctly left off: the DT's `sustainable-power` /
`contribution` properties are the IPA (`THERMAL_GOV_POWER_ALLOCATOR`) binding, already the
enabled governor, not a gap.

## Watchdog

DT node already present and unconditional: `sharkle.dtsi` `watchdog@40310000`
(`sprd,sharkle-wdt`), `timeout-sec = <12>`. Added:

```
CONFIG_WATCHDOG_CORE=y
CONFIG_SPRD_WATCHDOG=y
```

(`SPRD_WATCHDOG` already `select`s `WATCHDOG_CORE`; both are listed for clarity.)

**Does not auto-start.** Read `drivers/watchdog/sprd_wdt.c:sprd_wdt_probe()`: it enables the
peripheral clocks (`sprd_wdt_enable()`) and calls `devm_watchdog_register_device()`, but never
calls `sprd_wdt_start()` at probe. The hardware countdown only begins once userspace opens
`/dev/watchdog0` and issues `WDIOC_START` (or a resume from a suspend that found it already
active). `WATCHDOG_NOWAYOUT` is set, so once started it cannot be stopped by closing the device —
but it is never started by the kernel on its own.

Checked whether stock userspace pings it: unpacked the K18 candidate boot image
(`~/.cache/k18-candidate/k18_candidate.img`, via this tree's own `tools/sprd_bootimg.py unpack` /
`tools/sprd_ramdisk.py`, run with `python3 -I` into fresh empty scratch directories, nothing from
the image executed) and grepped every `init*.rc` in the stock ramdisk for `watchdog`. No
`watchdogd` service, no `/dev/watchdog` reference anywhere; the only two hits are AOSP's generic
`init.rc` comment about the Android (userspace ANR) watchdog before shutdown, and
`setprop debug.android_watchdog.disable 1` in `init.sl8541e_1h10.rc`, both unrelated to the
kernel watchdog character device. Conclusion: nothing in the stock image pings `/dev/watchdog0`,
so enabling the driver is safe as shipped — it sits idle unless something later opens the device
and starts it. If a future `init.rc` fragment or vendor service adds a `watchdogd` that *doesn't*
ping on the stock 12 s period, the watch will reboot; that would be a userspace change to review
at the time, not a kernel-side risk introduced here.

## TRUSTY

```
CONFIG_TRUSTY=y
```

DT node already wired in `sp9832e-1h10-dw100u.dtsi` (included by all seven `sp9832e-1h10-*`
boards): `trusty` (`android,trusty-smc-v1`) with `irq`/`virtio`/`log` children. `TRUSTY_VIRTIO`,
`TRUSTY_VIRTIO_IPC` and `TRUSTY_LOG` all default to `y` once `TRUSTY` is on (`drivers/trusty/
Kconfig`), confirmed in the built `.config`; not listed explicitly in the fragment to avoid
redundant lines. Build was clean (see "Build").

## USB gadget configfs functions

```
CONFIG_USB_CONFIGFS_ACM=y
CONFIG_USB_CONFIGFS_ECM=y
CONFIG_USB_CONFIGFS_ECM_SUBSET=y
CONFIG_USB_CONFIGFS_EEM=y
CONFIG_USB_CONFIGFS_F_HID=y
CONFIG_USB_CONFIGFS_NCM=y
CONFIG_USB_CONFIGFS_OBEX=y
```

All seven are generic `drivers/usb/gadget/Kconfig` options (not SPRD-specific), `USB_CONFIGFS`
and `USB_GADGET` were already on in `sprd_sharkle_defconfig`. No DT or driver work needed.

## Crown (rotary encoder)

```
CONFIG_INPUT_GPIO_ROTARY_ENCODER=y
```

Checked every 1h10 board's 4.4 stock DT and the matching 4.14 board DTS:

| Board | Stock `rotary-encoder` node | 4.14 DTS | Action |
|---|---|---|---|
| DM63 | yes (`ap_gpio` 78/92) | yes, `sp9832e-1h10-dm63.dts:299` | Kconfig only |
| DM82 | yes | yes, `sp9832e-1h10-dm82.dts:315` | Kconfig only |
| MT107 | yes | yes, `sp9832e-1h10-mt107.dts:322` | Kconfig only |
| MT108 | yes | yes, `sp9832e-1h10-mt108.dts:306` | Kconfig only |
| K18 | no | no (correct) | none |
| DW100U silver/v13m | no | no (correct) | none |

No board needed a new DT node; the single Kconfig flip (off in `sprd_sharkle_defconfig`) covers
all four crown boards at once, since the driver is a no-op on boards without the node.

## Build

Merged config: `devices/merge_watch_414.sh $O` (sprd_sharkle_defconfig + watch_414.fragment,
olddefconfig) — all nine new symbols landed as `=y` with no "previous value" conflicts beyond the
expected fragment-overrides-defconfig diagnostics, and no symbol was silently dropped by
`olddefconfig` (checked by grepping each one in the resulting `.config`).

- `make O=$O ARCH=arm64 CROSS_COMPILE=<gcc-arm-8.3 aarch64>- Image`: links; `vmlinux` layout
  `text 11017316 data 5718083 bss 1099112`, no errors.
- `make O=$O ARCH=arm64 CROSS_COMPILE=... dtbs`: every `.dtb`/`.dtbo` in the tree builds,
  including all seven `sp9832e-1h10-*.dtb`; no dtc warnings or errors (checked by grepping the
  full build log for `warn|error`, case-insensitive, after touching `sp9832e-mach.dtsi` to force
  a rebuild of every board that includes it).
- Decompiled `sp9832e-1h10-k18.dtb` and `sp9832e-1h10-dm63.dtb` with `dtc -I dtb -O dts` and
  confirmed the new `gpu_crit` trip reads back `temperature = <0x1adb0>` (110000),
  `hysteresis = <0x7d0>` (2000), matching the stock and `cpu_crit` values.
- Build directory was a scratch `/tmp` dir, removed after the check; nothing was run on a device
  and nothing from the stock boot image was executed.

## Open items

- GPU passive throttling has no actuator until a Mali/kbase driver exists for 4.14 (tracked
  already in `gap414.md`'s display/GPU section); only the critical shutdown trip is wired.
- Board/charger/PA auxiliary thermal zones from stock (`chg-thmzone`, `pa-thmzone`) are not
  wired in 4.14 — same as before this change, stock has them `polling-delay = 0` (monitor-only,
  no trips) and the 4.14 battery/charger port (`devices/SC2721_CHARGER_MAP.md`) does not yet
  expose the needed sensor channels; out of scope here.
- Watchdog is enabled but inert; if a later userspace change opens `/dev/watchdog0` without a
  ping loop on a &lt;12 s cadence, the watch will reboot. Re-check this note if stock `init.rc`
  content changes.
- TRUSTY was only build-checked (vmlinux links, DTB has the node); no device boot was attempted
  (offline-only task).
