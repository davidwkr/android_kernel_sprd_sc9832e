# Betterlife (BTL) touch controller: stock kernel interface

Applies to the k18, mt107, mt108 and dm82 watches: DT node `bl_ts@2d`, `compatible = "btl,btl_ts"`,
`reg = <0x2d>` on i2c-3, sharing the reset/irq GPIOs with `cst0xx_ts@38` and `hynitron@5a`.

Evidence tags:

- **[D]** verified from disassembly of the stock 4.4.83 kernel Image of the K18 (driver built in,
  `CONFIG_TOUCHSCREEN_BETTERLIFE_COB_JR=y`). Symbols come from the kallsyms tables in the Image.
  Offsets are `function+offset` from the symbol start.
- **[DT]** from the stock board DT.
- **[I]** inferred, not directly confirmed.

No device was accessed. The stock driver has no public source; this is an interoperability
description. The in-tree driver is `drivers/input/touchscreen/btl_ts.c` (`CONFIG_TOUCHSCREEN_BTL`;
the stock symbol name is not reused, like `TOUCHSCREEN_CST0XX`).

## DT [DT]

```
bl_ts@2d { compatible = "btl,btl_ts"; reg = <0x2d>;
  gpios = <&gpio 0x91 0>, <&gpio 0x90 0>;   /* reset = 145, irq = 144 */
  vdd_name = "vddsdcore";
  virtualkeys = <0x78 0x12c 0x3c 0x2d  x3>;  TP_MAX_X/Y = per board }
```

TP_MAX: k18 480x480, mt107 412x502, mt108 360x360, dm82 468x466.

## DT properties read [D] (`bl_ts_probe`)

- `gpios`: index 0 = reset, index 1 = irq (`of_get_named_gpio_flags`, probe+0x118..0x134).
- `virtualkeys`: 12 u32 (`of_property_read_u32_array`, probe+0x150).
- `TP_MAX_X`, `TP_MAX_Y`: one u32 each (log `fail to get TP_MAX_X/Y` on error, not fatal; probe+0x168..0x1a4).
- `vdd_name` is NOT read: the regulator name `"vddsdcore"` is hardcoded (probe+0x74..0x7c). The DT
  value is the same string.

## Probe [D] (`bl_ts_probe`)

1. `regulator_get("vddsdcore")`; if it succeeds `regulator_set_voltage(2800000, 2800000)` and
   `regulator_enable`. Then 100 ms + 200 ms busy delays (probe+0x9c..0xf4). Never disabled later.
2. `gpio_request` reset ("ts_rst_pin") and irq ("ts_irq_pin"); on failure free and return -1
   (probe+0x1c4..0x1e4). `gpio_to_irq(irq)`.
3. Reset line driven high (`bl_ts_set_wakeup(1)`, raw output), `msleep(100)`, `bl_ts_wakeup`
   (reset: 1, 50 ms, 0, 50 ms, 1; `bl_ts_wakeup`+0x0..0x94), `msleep(50)`.
4. `bl_get_chip_id` (below) under the I2C mutex. Result byte 0x22 = "bl6133", 0x23 = "bl6138"
   (probe+0x244..0x258). Anything else (including 0 after an I2C failure, the byte is
   zero-initialised): log `ChipID is failed`, `gpio_free` both GPIOs, return -1
   (probe+0x290..0x2a0). The stock driver stores the ID (`+208`) for the firmware path.
5. `i2c_check_functionality` check, then create a kthread `Betterlife_ts` (`bl_ts_handler`), then
   create 7 sysfs files on the client device (see below). Failure paths print
   `bl touch screen config Failed.`

### Chip ID read [D] (`bl_get_chip_id`, helpers)

| Step | Evidence |
|---|---|
| `bl_warkup_low`: irq pin output 0, `msleep(50)`, reset 1, 50 ms, reset 0, 50 ms, reset 1 | `bl_warkup_low`+0x0..0x7c |
| 50 x 1 ms delay | `bl_get_chip_id`+0x38..0x50 |
| `bl_soft_reset_switch_int_wakemode`: write `0A F5 E7 E8` to **0x2d** (the client address), then 50 ms | `bl_soft_reset_switch_int_wakemode`+0x0..0x70 |
| write `0A F5 E7` to **0x2c**, read 1 byte from 0x2c = chip ID | `bl_get_chip_id`+0x5c..0x8c, `CTP_FLASH_I2C_WRITE/READ` (raw `i2c_transfer`, one message) |
| `bl_warkup_high`: irq pin output 1, 50 ms, reset 1, 50 ms, reset 0, 50 ms, reset 1; then 20 ms | `bl_warkup_high`+0x0..0x7c, `bl_get_chip_id`+0x90..0xa8 |

0x2c is the controller's flash/boot-mode address [I]: the irq line low during the reset pulse
selects it. Driving the irq line high during the last reset returns to the application. The irq
pin is switched back to input before `request_irq` (`bl_ts_set_irq`).

## Firmware check and update [D], NOT implemented

`bl_ts_handler` first calls `bl_auto_update_fw` (handler+0x0), which calls
`bl_update_fw_for_self_ctp(fw_image, size)` with the embedded `fwbin_bl6133` image when the chip is 0x22,
another image otherwise. It re-enters boot mode (`bl_warkup_low`, soft reset, flash commands
`0D F2 05 FC 05 FD` to 0x2c, compares a 2-byte answer), reads fw/argument/project IDs with
`bl_read_fw(0xb6)` and the cob ID, detects a blank IC, compares checksums and flashes
(`bl_update_flash`, `bl_write_flash`). A failure only logs `bl ts chip init failed.`; the driver
goes on. The in-tree driver never writes the firmware. Related, also not implemented: the `fwupdate`
sysfs file, `btl_fw_upgrade_with_app_file`, the apk misc/proc interface and the factory test code
(`bl_factory_*`, `bl_enter_factory_mode`).

## Input device [D] (`bl_ts_handler`+0x58..0x270)

- Name `Betterlife_ts`, bustype `BUS_I2C` (0x18), version 1.
- Axes: `ABS_MT_POSITION_X` 0..TP_MAX_X, `ABS_MT_POSITION_Y` 0..TP_MAX_Y, `ABS_MT_TOUCH_MAJOR`
  0..255, `ABS_MT_WIDTH_MAJOR` (0x32) 0..255 (declared, never reported).
- No `input_mt_init_slots`, no `ABS_MT_SLOT` capability: **protocol A** (`SYN_MT_REPORT`).
- Keys declared: `BTN_TOUCH`, `KEY_MENU`, `KEY_BACK`, `KEY_APPSELECT`, and gesture keys
  `KEY_F4 KEY_U KEY_UP KEY_DOWN KEY_LEFT KEY_RIGHT KEY_O KEY_E KEY_M KEY_W KEY_S KEY_V KEY_C KEY_Z`.
  Only `KEY_F4` is ever reported by the read path.
- Interrupt: `request_threaded_irq(irq, bl_ts_irq, NULL, IRQF_TRIGGER_FALLING | IRQF_ONESHOT (0x2002))`.
  `bl_ts_irq` does `disable_irq_nosync`, sets a flag and wakes the `Betterlife_ts` kthread, which reads and
  calls `enable_irq` when done. The in-tree driver uses a threaded handler instead.

## Report [D] (`bl_ts_handler`+0x560..0x740)

One `i2c_transfer` of two messages at the client address: write 1 byte (register pointer `0x01`),
then read 8 bytes (handler+0x5a4..0x5f4; buffer byte 0 is preset to 1). Under the I2C mutex.
Skipped when the suspended flag (`+168`) is set.

| Byte | Meaning |
|---|---|
| 0 | gesture ID. 0x17: report `KEY_F4` down/sync/up/sync, then `msleep(500)` (handler+0x64c..0x68c). Other values ignored. |
| 1 | touch count |
| 2 | bits 7:6 event flag, bits 3:0 X[11:8] |
| 3 | X[7:0] |
| 4 | bits 3:0 Y[11:8] |
| 5 | Y[7:0] |
| 6, 7 | read, never used |

Handling (handler+0x6a0..0x74c): flag = byte2 >> 6.
- flag bit 0 set (1 = up): if flag == 1 and count <= 1 -> `BTN_TOUCH` 0, `SYN_MT_REPORT`. Flag 3 -> nothing.
- flag 0 (down) or 2 (contact) and count == 1 -> `ABS_MT_POSITION_X`, `ABS_MT_POSITION_Y`, `BTN_TOUCH` 1,
  `SYN_MT_REPORT`.
- always `SYN_REPORT`.

So the stock driver reports a **single** contact, no pressure and no area. A report with more than
one point, or with count 0 and a down flag, reports nothing. The in-tree driver reports the same data
through slot 0 of protocol B (`INPUT_MT_DIRECT`).

## Suspend / resume [D] (`adf_notifier_callback`)

Registered on the UNISOC ADF display notifier (`adf_register_client`), not on PM ops.
- Blank off (data value 3): set the suspended flag, `disable_irq`, `bl_chip_switch_mode(3)` = write
  `A5 03` (register 0xA5 = 3, deep sleep) at the client address (`bl_chip_switch_mode`+0x2c..0x4c).
- Unblank (value 0): clear the flag, dummy read of register 0xA5, `bl_ts_wakeup` (reset 1/0/1, 50 ms
  steps), `enable_irq`, release `KEY_BACK` and every slot (`ABS_MT_SLOT`, `ABS_MT_TRACKING_ID` -1), `SYN_REPORT`.
- Related register, sysfs only: `bl_send_power_state` writes `8B <state>`.

Details [D] (K18 Image, `function+offset`):

- Notifier filter: event 0x10 (`ADF_EVENT_BLANK`), `*(int *)ev->data` 0 = unblank, 3 = blank
  (`adf_notifier_callback`+0x24..0x34, +0xec).
- `bl_chip_switch_mode(mode)` writes the two bytes `A5 <mode>` at the client address with one
  `bl_i2c_transfer` (`bl_chip_switch_mode`+0x30..0x50). Its only caller passes 3
  (`adf_notifier_callback`+0x11c). No other mode value is ever written.
- Blank order: suspended flag (`+168`) = 1, `disable_irq`, I2C mutex, `A5 03` (`adf_notifier_callback`+0x108..0x11c).
  The report path skips its read while the flag is set (`bl_ts_handler`+0x618, flag test), so nothing
  is reported while asleep.
- Unblank order: flag = 0, dummy `bl_read_reg(0xA5)` (result unused), `bl_ts_wakeup`, `enable_irq`,
  release of the key held in the struct (`input_event(EV_KEY, code, 0)`, +0x90) and of every
  slot (`ABS_MT_SLOT` n, `ABS_MT_TRACKING_ID` -1), `SYN_REPORT` (`adf_notifier_callback`+0x50..0xe0).
- Both branches are guarded by a "chip initialised" byte (`+205`); a second byte (`+281`) is set on blank
  and cleared on unblank and is never read in the driver.
- `bl_send_power_state(state)` (writes `8B <state>`) has no caller; `bl_store_powerstate` only stores
  the parsed hex value in a global. Not used for suspend.

### Gesture / double-tap-to-wake: no evidence of a gesture mode [D]

- Gesture ID 0x17 (report byte 0) is the only one handled, as `KEY_F4` (0x3e) press/release with
  `msleep(500)` (`bl_ts_handler`+0x62c..0x684). The other `KEY_*` codes declared for the input device
  (U, UP, DOWN, LEFT, RIGHT, O, E, M, W, S, V, C, Z) are never reported.
- There is no register write or command that enables gestures: the only mode-register write is
  `A5 03`, the only other register write in the runtime paths is the sysfs `reg` file (arbitrary
  user value) and `8B` (dead code). No sysfs or proc switch exists for gestures
  (attribute names: `reg fwupdate argchecksum powerstate apkdebug kernelversion irqstatus chipinfo`).
- The stock blank path disables the irq and sets the suspended flag, so the stock driver cannot
  wake from the sleeping controller. Gesture 0x17 only appears in the normal running mode,
  with the firmware's own meaning [I: unknown; not known whether it is a double tap].
- Userspace: the stock system image has no `Betterlife_ts.kl`, so `Generic.kl` applies, where
  `key 62 F4` has no `WAKE` flag. No stock system or vendor file mentions a Betterlife gesture path.
  So KEY_F4 does not wake an Android 8.1 device from the stock userspace.
- Result: DT2W cannot be implemented with evidence. The in-tree driver reports it as unsupported.

### In-tree driver

`btl_ts.c` follows the stock blank/unblank sequence on the ADF notifier (the `A5 03` write, `disable_irq`
and the not-a-wake-source state while the screen is off; dummy read, reset pulse, `enable_irq` and
finger release on unblank). The key release is not reproduced because the key is not declared. System
suspend keeps the late PM ops: with the screen on touch is still a wake source and a report that arrives with the
bus down is read on resume; a screen change that arrives with the bus down is applied on resume.
The `wake_gesture` attribute is on the input device, see `TOUCH_WAKE.md`.

## Sysfs and proc [D]

- On the I2C client device: `reg`, `fwupdate`, `argchecksum`, `powerstate`, `apkdebug`,
  `kernelversion`, `irqstatus`, `chipinfo` (`dev_attr_*`; attribute group created in probe+0x3d4..0x440).
- `/sys/board_properties/virtualkeys.Betterlife_ts` (`virtual_keys_show`): three entries
  `0x01:158:x:y:w:h` (KEY_BACK) built from the DT `virtualkeys` words (120,300,60,45 on every board).
  Android would turn touches inside that area into a BACK key. Not reproduced.
- `/proc/Betterlife_ts` (apk interface, `bl_apk_fops`) and a `Betterlife_ts` factory-test file path.
None of these is implemented.

## Shared GPIOs in the in-tree driver

The reset and irq GPIOs are taken with `devm_gpiod_get_index`. A devm action puts the irq GPIO
back to input (the ID read drives it) before the GPIOs are released, so a failed probe leaves the
lines usable for `cst0xx_ts` / `hyn_ts`. If another driver already holds a GPIO the probe fails
with -EBUSY before any register access. The ID read pulses the shared reset line, which the
other candidates also do in their own probes.

## Open items

- Never run on a unit with this chip. Reset/ID timing is copied from the stock disassembly.
- A blank IC (no firmware) would be flashed by stock; the in-tree driver will report the chip
  but touch will not work.
- Probe order against the other candidates is untested: a chip that answers at 0x38 or 0x5a
  gets one extra reset pulse and an irq-pin wiggle from this probe if it runs first.
- The 500 ms sleep after gesture 0x17 is not reproduced.
- The sleep and wake sequences are copied from the stock disassembly and not run on a unit with this chip.
  The wake timing (reset pulse of 50 ms steps) is the stock one; whether the chip needs the dummy read is unknown.
- `A5 03` is called deep sleep here by inference from the name of the function; no datasheet.
- No gesture mode is known. Finding one needs the vendor firmware documentation or a unit to probe.
