# OCH1970 stock interface and the in-tree driver

Evidence tags: **[D]** disassembly of a stock kernel Image (function name from kallsyms, offset from the function start); **[DT]** stock device tree (`devices/*/stock.dts`); **[H]** hypothesis or unverified, not backed by disassembly. No unit was available; nothing here is hardware-tested.

Images used (read-only, boot images unpacked into scratch directories): K18 (`kernel.Image`, full symbols), DM82, MT107, MT108, DW100U silver / v13m (probe and work functions located by string cross references, callees matched by code signature against K18). The stock `/system` keylayouts were checked read-only.

## Three stock generations

The stock driver is not one driver. The device tree selects it:

| Gen | DT marker | Devices | Axis / thresholds |
|---|---|---|---|
| 1 | no thresholds, no axes ([DT] dm82, mt107) | DM82, MT107 | fixed in the driver: DM82 X axis, MT107 Y axis |
| 2 | `open-threshold`, `rotate-threshold` = `<bop brp>` u32 pairs ([DT] mt108, dw100u) | MT108, DW100U | `open-axis` x1/y1/z1, `rotate-axis` x2/y2/z2, `control-axis`, `control-data` |
| 3 | `open-bop`, `open-brp`, `rotate-bop`, `rotate-brp` as strings ([DT] k18) | K18 | same axis properties |

Gen 1 hard-coded values [D]: DM82 `och1970_i2c_probe+0x604` (values built on the stack at +0x40..+0x7c): bop_o -4000, brp_o -6000, bop_r -100, brp_r -280 (X). MT107 `och1970_init` (+0x68..+0x98, called from probe): bop_o 4500, brp_o 4000, bop_r 180, brp_r 50 (Y). In the driver the gen-1 variant is chosen with the non-stock DT string `och1970,variant` ("dm82" default, "mt107"; set in `sp9832e-1h10-mt107.dts`).

## Input device [D]

`och1970_i2c_probe` (K18, +0x518..+0x680; same in all generations): `input_allocate_device`, name **"magnetic"**, bustype `BUS_I2C` (0x18), evbit = EV_KEY | EV_REL, relbit mask 0x207 (REL_X, REL_Y, REL_Z, REL_MISC), keybits F1 (59), F2 (60), F3 (61), F8 (66) set unconditionally plus the four configured codes. No EV_ABS: the stock driver never reports raw XYZ as ABS. The previous in-tree driver (name "och1970", ABS X/Y/Z) was therefore not stock and is gone.

Keys are pure press + release pulses, each followed by `EV_SYN` (`och1970_irq_work_fun`, K18 +0x1ec..+0x350). The stock sensors HAL lists no magnetic sensor [H]; the device name has no `.kl` in the stock `/system/usr/keylayout` (only `gpio-keys`, `fp-keys`, `gf-keys`, touch layouts), so Android falls back to `Generic.kl`, which maps 59/60/61/66 to F1/F2/F3/F8. Consumers are the framework or a vendor app [H]; the init script chmods `/dev/och1970` and `/dev/hall` to 666 ([D] K18 ramdisk `init.sl8541e_1h10.rc`), i.e. something in userspace opens `/dev/och1970`.

## DT properties [D] `och1970_i2c_probe`

- `linux,open`, `linux,close`, `linux,back`, `linux,front`: u32 key codes. Defaults differ by generation (a trap):
  - gen 1: open 0x3b (F1), close 0x42 (F8), back 0x3d (F3), front 0x3c (F2)
  - gen 2 and 3: open 0x42 (F8), close 0x3b (F1), back 0x3c (F2), front 0x3d (F3)
  - K18 has no key properties [DT], so it uses the gen-3 defaults. MT107's DT gives front 0x3d / back 0x3c, the reverse of the gen-1 defaults.
- `irq-gpio` (required; requested as "och1970-irq", input), `rst-gpio` (required in stock, "och1970-rst", output low).
- `debounce-interval` u32 ms, default 5 (gen 1 only).
- `control-axis` "x"/"y"/"z", `control-data` u32 (optional, default 0), `open-axis`, `rotate-axis` (gen 2/3).
- gen 3: `open-bop`, `open-brp`, `rotate-bop`, `rotate-brp` as decimal strings (`simple_strtol`). gen 2: `open-threshold`, `rotate-threshold` u32 pairs stored as s16.

## Registers [D]

All via I2C block read/write. Slave 0x0d [DT].
- 0x00 (2 bytes): ID, must be 0x48 0xc0 (stock checks after reset).
- 0x10 (2 bytes): second byte bit 0 = data ready (`och1970_work_func`, polling).
- 0x17 (8 bytes): `och1970_get_xyz_data`: Z = b[2..3], Y = b[4..5], X = b[6..7], big endian s16. No data-ready check, no single-shot trigger.
- 0x21: `och1970_measurement_mode(m)`: `(v & 0xf0) | m`; `och1970_powerdown_mode`: `v & 0xf0`. Modes used: 0x0a (suspend, enable 5, write "close"), 0x0e (enable 4, write "open").
- 0x20 (2-byte block, 0x20 and 0x21): per-axis enable bits in the **second byte** (`och1970_en_open_axis` / `_en_rotate_axis`): x1 0x02, x2 0x04, y1 0x08, y2 0x10, z1 0x20, z2 0x40.
- 0x22..0x27: thresholds, written as 4 bytes `{bop_hi, bop_lo, brp_hi, brp_lo}` at x1 0x22, x2 0x23, y1 0x24, y2 0x25, z1 0x26, z2 0x27 (the 4-byte writes overlap neighbouring registers; reproduced as is).
- Probe sequence (K18 `och1970_i2c_probe`, +0x76c..+0xaa4): reset (rst high 20 ms, low 20 ms, high 200 ms); CNTL2: `&0xf0`, `|0x0a`, `&~0x10`, `|0x20`; then clear the six axis bits; block byte0 `|0x04`; block byte1 `&~0x01`; then write the open and rotate thresholds. Gen 1: thresholds first, then DM82 clears x1, sets x2, clears y1/y2/z1/z2; MT107 clears x1/x2/y1, sets y2, clears z1/z2. Gen 2 writes thresholds on `enable=1` (`och1970_set_axis_threshold`, also arms the rotate axis bit).
- **Open item:** the axis-enable bits live in the same byte as CNTL2 (MODE 0x0f, SDR 0x10, SMR 0x20 per the public datasheet), so stock axis enables alias mode bits. Reproduced exactly; whether the chip really behaves that way (or the 0x20 block is a 16-bit register) needs a unit or the full datasheet. The old driver's soft reset (reg 0x30) and single-shot mode had no stock evidence and were dropped.

## Key emission logic [D] (`och1970_irq_work_fun`)

Values: O = open-axis sample, R = rotate-axis sample, C = control-axis sample, all s16. Flags (all initially 1): `wake` (stock +496), `state` (+497), `extra_back` (+498, gen 3). Keys: front/back pick by `control_data <= C ? back : front`.

Gen 3 (K18):
1. O > open_bop and R > rotate_bop: state = 1, emit back/front.
2. else O > open_bop and R < rotate_brp: enable rotate axis; if wake and state: state = 0, emit **open**.
3. else O < open_brp: disable rotate axis; if wake and not state: state = 1, emit **close**, then **back** if extra_back.

Gen 2 (sign of open_bop selects polarity), negative: R > rotate_bop: state = 1, back/front; else O > open_bop: state ? (state = 0, back) : (wake: open); else O < open_brp: wake: close. Positive: R < rotate_brp: state = 1, back/front; else O < open_brp: state ? (state = 0, front) : (wake: open); else O > open_bop: wake: close.

Gen 1, v = X (DM82) or Y (MT107), z = Z: DM82 zones A = v > rot_bop, B = open_brp < v < rot_brp, C = v < open_bop and v < rot_brp. MT107: A = v < rot_brp, B = rot_bop < v < open_brp, C = v > open_bop and v > rot_bop. A: state = 1, z >= 0 ? back : front. B: state ? (state = 0, back) : (wake: close). C: wake: open.

"bop"/"brp": operate and release points of the hall switch, used as the hysteresis pair; the same values are programmed into the chip so it raises the interrupt, and the driver re-evaluates the sample in software (no hardware state is read back).

## Polling vs interrupt, wake [D]

Interrupt: `request_threaded_irq(handler NULL, thread och1970_handle_fun, flags 0x2008 (LOW | ONESHOT)`, DM82 0x2002 (FALLING | ONESHOT)). Thread: `disable_irq_nosync`, `__pm_wakeup_event("och1970-time", jiffies_to_msecs(500))` (5000 ms at HZ 100), `pm_stay_awake` if may-wakeup, read XYZ, queue the evaluation work (gen 1: delayed by `debounce-interval`); the work does `pm_relax`, evaluates, `enable_irq`. Nothing generates interrupts until userspace sets enable (axis bits are cleared in probe).

Polling (sysfs `enable` 2 or 3): `och1970_work_func` every 100 ms: if ST bit0, read XYZ and emit EV_REL REL_X/Y/Z + SYN. This is the only raw XYZ output.

Wake: `device_init_wakeup(dev, 1)` at probe (wake flag initially set). Suspend (K18): not may-wakeup: `disable_irq`, power down; may-wakeup: mode 0x0a, `irq_set_irq_wake(irq, 1)`. Resume: not may-wakeup: power down, `enable_irq`; else mode 0x0a, `irq_set_irq_wake(irq, 0)`. DM82 additionally switches the irq type to LEVEL_LOW in suspend and back to FALLING in resume, and its resume calls `irq_set_irq_wake(irq, 1)` again (looks like a stock bug; the driver disables wake there).

## sysfs and misc [D]

On the input device (`/sys/class/input/inputN/`; group created on `input->dev.kobj`): `enable`, `wakeup`, `chipinfo` ("och1970"), `thresholdx1 x2 y1 y2 z1 z2` (show: `BOP:%d\nBRP:%d\n` from the chip; store: two numbers, ignored if either is 0 or 0xffff; hardware registers only, the software thresholds are not changed).
`enable`: 1 = wake on, `device_init_wakeup(1)`, open axis enabled; 2 / 3 = polling (3 also enables the open axis); 4 = extra_back off, mode 0x0e; 5 = extra_back on, mode 0x0a if wake else power down; other = wake off, axis off, power down, stop polling.
Misc device `/dev/och1970` (dynamic minor): write "open" (rotate axis on, mode 0x0e) or "close" (rotate axis off, mode 0x0a if wake else power down); stock also has a no-op read and fasync, not ported.

## Not ported / differences

- Gen 2 reads a calibration file under `/productinfo` and has `enable` 6..10 (axis select, auto-align); not ported, and no NV/product partition was read.
- sysfs `enable` stores use `kstrtouint` (stock `simple_strtoul`, any text = 0); `enable` show prints the last value written (stock prints a field that is never set).
- Misc write is bounded to 15 bytes and trims whitespace (stock copies the length unchecked and compares exactly).
- Axis strings that are not x/y/z (with the right level digit) fail the probe; stock silently used 0.
- Chip ID is checked before touching rst-gpio and again after reset; rst-gpio is optional (stock requires it).
- Old ABI removed: input name "och1970", ABS X/Y/Z, `enable`/`delay` on the I2C device.

## 4.14 port notes (`kwt-414k`, branch `kernel-4.14`, not modified)

The 4.14 tree's `drivers/input/misc/och1970.c` is byte-identical to the previous 4.4 file, and the APIs used by the new file exist there (`wakeup_source_register`, `devm_input_allocate_device`, `of_find_property`, `devm_gpio_request_one`, REL_MISC). To port: copy `drivers/input/misc/och1970.c` and the Kconfig help text from the 4.4 commit, add `och1970,variant = "mt107";` to the 4.14 MT107 DTS if it has one. Check on 4.14: `i2c_driver.probe` still takes `(client, id)` (yes up to 5.x); `return ret ? ret : (ssize_t)count` and `of_find_property` need no change; confirm that `irq_set_irq_type` is visible via `linux/interrupt.h` (it is declared in `linux/irq.h`; add `#include <linux/irq.h>` if the build complains); build-check `och1970.o` for the 4.14 K18/MT107 configs.
