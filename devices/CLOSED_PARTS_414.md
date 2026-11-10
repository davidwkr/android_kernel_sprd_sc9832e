# Closed vendor parts without a 4.14 driver: HEALTH_GOODIX, INPUT_FALL_OFF, SUNWAVE_FINGERPRINT

Evidence tags: **[D]** disassembly of a stock 4.4 Image; **[DT]** stock device trees (`devices/*/stock.dts`, all six); **[HAL]** stock vendor/system images and ramdisk; **[SPEC]** live-unit notes in the per-device SPEC; **[H]** hypothesis.

Images: K18 `kernel.Image`, DM63 and MT107 AOD kernel Images, all copied to scratch (read-only). They are flat arm64 Images with RELA relocations (base 0xffffff8008080000, `R_AARCH64_RELATIVE` records at the end), so `platform_driver` / `i2c_driver` structs are found by decoding the relocation table; of_device_id compatibles are inline 128-byte strings. MT107 and DM82 `vendor.bin` were dumped with `debugfs rdump` and grepped as data (nothing executed). `system.bin` of both has no hit for any of the three parts (only the generic fingerprint framework).

Method per driver: located the compatible in the of_device_id table, found the pointer to the table in the relocation records, backed up to the driver struct (probe / remove / name / of_match_table), disassembled probe and init, and checked the init function for any `platform_device_register*` (none in any of the three).

## Summary

| Part | Bus / binding | Compatible | Stock DT node? | Userspace consumer | Verdict |
|---|---|---|---|---|---|
| `HEALTH_GOODIX` (DM63, MT108) | **I2C client** (`i2c_register_driver`) | `goodix,health` | none in any DT | none live; dormant HAL remnants (DM82 vendor) | dead |
| `INPUT_FALL_OFF` (DM63, MT108) | platform | `hsc,fall-off` | none in any DT | none | dead |
| `SUNWAVE_FINGERPRINT` (DM63, DM82, K18, MT107, MT108) | platform | `sunwave,fingerprint` | none in any DT | HAL + TA shipped, no device to open | dead |

None of the three registers its own device: they only call `platform_driver_register` / `i2c_register_driver`, and probe reads DT properties (fails without `of_node`). So they bind only if a DT node with the compatible exists. A search of all six stock DTs for the three compatibles, their property names (`fall-off-gpio`, `goodix,gpio_reset`, `goodix,gpio_irq`, `sunwave,reset-gpio`, `sunwave,irq-gpio`) and for any `goodix`/`sunwave`/`hsc`/`fall`/`wrist` string gives only the DW100U `goodix_fp_ree@0` (`goodix,fingerprintree`, a different driver, `GOODIX_TEE_FINGERPRINT`, already ported). The only enabled SPI controller with no children is `spi@70a00000` on DM63/K18/MT107 [DT] (what Sunwave would hang off); on DM82 and MT108 all SPI controllers are disabled. Live DM63 input device list has no `fall-off`, `gh-keys` or `sf-keys` [SPEC], whereas the DW100U list has `gf-keys` (the Goodix FP driver, which does bind).

---

## 1. HEALTH_GOODIX (DM63, MT108)

Present only in the DM63 Image of the three (K18 and MT107 Images lack it, matching their configs).

### Binding [D]
- `i2c_driver` (relocation records: probe `gh_probe`, remove `gh_remove`, `driver.name` "goodix_health", of_match_table, id_table name "goodix_health"). Init prints "gh30x driver installing.." and calls `i2c_register_driver` (the callee stores owner at `driver+0x48` and is shared with the other I2C drivers).
- of_device_id: single entry `goodix,health`. An unreferenced second table with `goodix,fingerprintree` sits next to it (no pointer to it anywhere: dead object code [H]).
- Not bound on DM63: no `goodix,health` child under any I2C bus in `dm63/stock.dts` or `mt108/stock.dts` [DT]. The heart-rate part actually wired is `tyhx,hrs3xxx` on i2c-1 @0x44 (DM63) / `tyhx,hrs3xxx` + `heartrate,bd1xxx` (MT108) [DT][SPEC]; both already have 4.14 interface drivers.

### What it is [D][H]
Goodix GH30x-family PPG heart-rate/SpO2 AFE ("gh30x", `GH_IOC_*`, "V1.01.04"). The kernel part is a thin transport; the algorithm and register access live in userspace.

### Interface summary (only if the DT ever gets a node)
- Probe (`gh_probe`): chardev via `alloc_chrdev_region` + class `goodix_health` + `device_create` named `goodix_health` (**/dev/goodix_health**), `cdev_add`; threaded IRQ named `goodix_fp_irq` (name copied from the Goodix FP driver); input device **`gh-keys`**; a netlink socket (kernel to a registered userspace pid, "netlink create success") used to forward IRQ events.
- Ops: `gh_ioctl` with `GH_IOC_INIT` (reports driver version string "V1.01.04", vendor_id/mode/operation), `GH_IOC_EXIT`, chip reset, `GH_IOC_ENABLE_IRQ` / `DISABLE_IRQ`, `ENABLE_POWER` / `DISABLE_POWER`, `ENTER_SLEEP_MODE`, `REMOVE`; `gh_ioctl_transfer_{write,read,raw}_cmd` for register transfers; `read` / `write` return "not support, please use ioctl".
- DT properties (`gh_get_gpio_dts_info`, `of_get_named_gpio`): **`goodix,gpio_reset`** (requested as `goodix_reset`), **`goodix,gpio_irq`** (requested as `goodix_irq`). The I2C address is the client's own.
- sysfs: none seen.

### Userspace [HAL]
- DM82 `vendor.bin` (whose config has `HEALTH_GOODIX=n`): `lib{,64}/hw/vendor.sprd.hardware.health@1.0-impl.so` implementing `vendor.sprd.hardware.health@1.0::IGoodixHealthDaemon` (GH30x init/FIFO/int handlers, `hal_gh30x_i2c_*`/`spi_*` over ioctl on **`/dev/goodix_health`**, scans `/dev/input`, state in `/data/goodix/health/`, `/data/gh_data/memory/`), listed in `manifest.xml`. But there is no `vendor.sprd.hardware.health@1.0-service` binary and no rc for it in the vendor image (only the stock `android.hardware.health@1.0-service`). So nothing starts the daemon [HAL].
- DM82 and MT107 sepolicy/file_contexts carry `ghealthd`, `ghealthd_device` (`/dev/goodix_health`), `hal_igoodixhealth_hwservice`: leftovers [HAL].
- Sensors HAL (`sensors.sp9832e.so`) knows only `heartrate_sensors` (the TYHX input), accelerometer, proximity: no Goodix input [HAL].
- No DM63 vendor/system image is available here; the DM63 live input list shows `heartrate` (hrs3xxx) and no `gh-keys` [SPEC].

### Verdict: **dead on the stock watches** (no DT node, no consumer). A Goodix GH30x module would be an alternative HR part on a hardware variant none of the six DTs describes. Do not port. If a variant surfaces, an I2C client with a chardev + two GPIOs + netlink is small, but the userspace daemon is the real dependency and is not shipped in any available image.

---

## 2. INPUT_FALL_OFF (DM63, MT108)

### Binding [D]
- `platform_driver` at relocation-decoded struct: probe `fall_off_probe` (the only populated slot; `fall_off_remove` string exists but the struct has no remove pointer), `driver.name` **"fall-off"**, of_match_table with a single entry **`hsc,fall-off`**. Init is a plain `__platform_driver_register`.
- Not bound: no `hsc,fall-off` and no `fall-off-gpio` property in any DT, DM63 and MT108 included [DT]. Not in the live DM63 input list [SPEC]. Absent from the K18 and MT107 Images (configs agree).

### What it is [D][H]
A single-GPIO level-detect input: not a chip, a pin. Probe: `devm_kzalloc(0x18)`, `of_get_named_gpio(np, "fall-off-gpio", 0)` (rejects > 511 "fall-off-gpio is invalid"), `gpio_request("fall-off")`, input direction, `gpio_to_irq`, then `request_threaded_irq` (handler, no thread) named **"fall-off-irq"** with the trigger chosen from the current pin level (level 1 to LEVEL_LOW, level 0 to LEVEL_HIGH, i.e. it always arms for the opposite state). Input device name **"fall-off"**, vendor 1 / product 1, evbit EV_KEY, keybit **KEY_F2 (60)** and **KEY_F3 (61)**. The handler re-reads the pin, flips the trigger polarity, and emits a press+release pulse plus `EV_SYN`: pin high gives KEY_F3 (61), pin low gives KEY_F2 (60). No `enable_irq_wake`, no sysfs, no DT properties other than `fall-off-gpio`. Copy-pasted log macro `[HRS3XXX-heartrate]` in its init/remove, so it came from the same BSP author as the HR driver [H].

### Hardware [H]
"Fall-off" in this BSP means wrist-off / device removed from the wrist: one GPIO from a wear switch or a sensor with a digital wear-detect output (the vendor prefix "hsc" is unidentified). The watches already do wear/raise detection with the SC7A22 accelerometer (`wrist_flage` log) and the HR sensor, which is probably why the DT does not wire this pin.

### Userspace [HAL]
No hit for `fall-off`/`fall_off`/`falloff` in MT107 or DM82 vendor and system images, the K18 ramdisk, the sensors HAL, `hwc_dm63.so`, or the AOD notes. MT107/DM82 do not enable the config anyway; no DM63 or MT108 userspace image is available, but the sensors HAL has no input matching and KEY_F2/F3 would need a `.kl` mapping that is not in any available image.

### Verdict: **dead on the stock watches** (no node, no consumer). Not worth a driver. If a board ever wires the pin, the logic is small enough to rewrite from the summary above (property `fall-off-gpio`, input "fall-off", KEY_F2/KEY_F3, level-flip IRQ); `gpio-keys` cannot express the flip.

---

## 3. SUNWAVE_FINGERPRINT (DM63, DM82, K18, MT107, MT108)

Same code in the K18, DM63 and MT107 Images ("sf_ctl", version string `v1.2.1-20170707`).

### Binding [D]
- `platform_driver` `sunwave_fp`: probe `sf_probe`, remove, suspend `sf_suspend`, resume `sf_resume`; of_match_table single entry **`sunwave,fingerprint`**. Init (`sf_ctl_driver_init`) allocates a 0x18-byte struct, `__platform_driver_register`, copies the version string. No platform_device is created.
- Not bound: no `sunwave,*` anywhere in the six DTs. The one enabled SPI controller (`spi@70a00000`, DM63/K18/MT107) has no children [DT]. The driver itself never touches SPI [D]; the SPI data path is presumably userspace/TEE (`sf_trusty.elf`) [H], the kernel part is the control side (reset/irq, uevent, keys).

### Interface summary [D] (for completeness; not recommended)
- DT: platform node with `sunwave,reset-gpio` and `sunwave,irq-gpio` (`of_get_named_gpio`; "fail to get reset_gpio_number" / "irq_gpio_number" on failure). GPIOs are requested later by the ioctl `sf_ctl_init_gpio_pins` (names `sunwave_rst`, `sunwave_irq`; log strings still say `GPX1_3`, an Exynos leftover); probe prints "Do not sf_ctl_init_gpio_pins" and defers.
- Probe: wakeup source `sunwave_wl`, input device **`sf-keys`** (navigation keys), `misc_register`: **/dev/sunwave_fp**; IRQ **`sf-irq`** (`request_irq`, run after the ioctl). IRQ handler emits a kobject uevent `SPI_STATE=finger`. ioctl entry `sf_ctl_ioctl` (gpio/irq init and key reporting; command list not decoded).

### Userspace [HAL]
- MT107 and DM82 `vendor.bin` ship the full stack: `android.hardware.biometrics.fingerprint@2.1-service` (rc: `service fps_hal`, `class late_start`, user system, group system input), `lib{,64}/hw/swfingerprint.default.so` (`SunwaveFingerprintHAL`, same binary on both; chips IC8201/8202/8221/8231, SPI helpers `sf_spi_*`, opens **/dev/sunwave_fp**, `/dev/input/event*`, `/dev/trusty-ipc-dev0`), the TA `firmware/sf_trusty.elf`, `libfactorylib.so`, `etc/permissions/android.hardware.fingerprint.xml` (declares the feature), sepolicy `sf_device` for `/dev/sunwave_fp` and `/data/system/sunwave`. K18 ramdisk `init.sl8541e_1h10.rc` does `chmod 660` / `chown system system /dev/sunwave_fp` ("add for sunwave fingerprint").
- Live units: no `sf-keys` input and no fingerprint node on DM63 [SPEC] and the MT107/DM82 SPEC notes ("no fingerprint sensor node ... config leftover").

### Verdict: **dead on the stock watches** (HAL and TA are shipped in a generic vendor image, but no DT node and no sensor wired: `/dev/sunwave_fp` never exists). Do not write a driver. Worth noting for the 4.14 image, not the kernel: [H] the vendor image still starts `fps_hal`, which fails to open the missing node, and still declares `android.hardware.fingerprint`; this is the same as stock 4.4 behaviour, so it is not a regression, but removing the permissions XML (or the rc) in the product config would stop the feature being advertised. Not verified on hardware.

---

## Open questions
1. DM63/MT108 userspace images are not available here, so "no consumer" for fall-off and Goodix health on those two rests on the DT, the live DM63 input list, and the MT107/DM82 vendor images (which carry the Goodix HAL remnants only).
2. The meaning of the `hsc` vendor prefix and which physical part drives `fall-off-gpio` is a guess [H]; no schematic evidence.
3. `fps_hal` behaviour with a missing `/dev/sunwave_fp` (crash loop vs clean exit) was not checked.
