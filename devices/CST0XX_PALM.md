# cst0xx_ts palm cover

Palm rejection ("cover the screen to turn it off", vendor property `palm.press.screen=true`) works on
the stock DM63 kernel and did not work with `drivers/input/touchscreen/cst0xx_ts.c`: the driver dropped
every report whose gesture byte (register 1) is `0xAA`, and the input device had no key to report.

## Stock behaviour [D]

Source: kernel Image of the DM63 `boot.img` (4.4.83, arm64, relocatable, `_text` =
`0xffffff8008080000`, file offset = address - `_text`). The functions were located by their strings and
disassembled; a plain kallsyms parse labels the cst0xx functions one entry off (the address table is
relocated through RELA), so the labels below are by content, not by kallsyms.

| Function | Address | File offset |
|----------|---------|-------------|
| `touch_event_handler` (report kthread) | `0xffffff800854742c` | `0x4c742c` |
| `cst0xx_interrupt` (hard irq handler) | `0xffffff8008547748` | `0x4c7748` |
| `cst0xx_reset` | `0xffffff8008547840` | `0x4c7840` |
| `adf_notifier_callback` | `0xffffff80085478bc` | `0x4c78bc` |
| `cst0xx_probe` | `0xffffff80085479b0` | `0x4c79b0` |

`touch_event_handler` loop:

1. Reads 33 bytes from register 0 into `buf` (`0xffffff800854751c`, I2C read helper).
2. Palm check (`0xffffff8008547540`..`0xffffff80085475b4`): `buf[1] == 0xaa` (`cmp w0, #0xaa`) and
   `((buf[3] >> 4) & ~4) == 0` (`and w0, w0, #0xfffffffb`), i.e. the first point byte has event flag 0
   (down) or 1 (up) and bits [5:4] clear. Then, with `input_event(dev, type, code, value)`:
   - `(EV_KEY, 62, 1)`, `(EV_KEY, 62, 0)`: `KEY_F4` press and release, no `SYN` in between,
   - `(EV_SYN, SYN_MT_REPORT, 0)`, `(EV_SYN, SYN_REPORT, 0)`,
   - `msleep(100)` (`mov w0, #0x64` before the sleep call), then back to waiting for the next irq.
   No point data is parsed for such a frame.
3. Any other frame (including `0xaa` with another flag nibble) is a normal report: `buf[2] & 7` is the
   count, only the first point is used (event flag `(buf[3] & 0xc0) == 0xc0` or bit 6 set: not
   reported), `ABS_MT_TRACKING_ID` = `buf[5] >> 4`, X = `(buf[3] & 0xf) << 8 | buf[4]`,
   Y = `(buf[5] & 0xf) << 8 | buf[6]`, `ABS_MT_PRESSURE` = `buf[7]` (127 if not positive),
   `ABS_MT_TOUCH_MAJOR` = `buf[8] >> 4` (9 if 0), `BTN_TOUCH`, `SYN_MT_REPORT`, `SYN_REPORT`
   (protocol A, no slots). No palm or width value is derived from the area field.

The irq is disabled while the screen is blanked (`adf_notifier_callback`: state 3 writes `E5 03` then
`disable_irq`, state 0 resets and `enable_irq`; see `devices/TOUCH_WAKE.md`), so the palm key is only
reported with the screen on.

Input capabilities registered by `cst0xx_probe` (`0xffffff8008547d64`..`0xffffff8008547e7c`):

- `EV_KEY` keybits: `KEY_F4` (62), `KEY_MENU` (139), `KEY_BACK` (158), `KEY_HOMEPAGE` (172),
  `BTN_TOUCH` (330). `KEY_WAKEUP` is not set.
- `EV_ABS`: `ABS_MT_POSITION_X`/`Y` 0..max, `ABS_MT_TOUCH_MAJOR` and `ABS_MT_WIDTH_MAJOR` 0..15,
  `ABS_MT_PRESSURE` 0..127, `ABS_MT_TRACKING_ID` 0..255. No `ABS_MT_SLOT`.
- Input name `cst0xx_ts`. No `BTN_TOOL_*`, no sysfs attribute and no uevent for palm.

`BIG_PALM_ENABLE` and `BIG_PALM_FLAG` exist as kallsyms data symbols in the image (common Hynitron
code); no code of the cst0xx path references them. User space: `Generic.kl` maps `key 62 F4`; whether the
vendor framework acts on it (`palm.press.screen`) was not examined.

## Driver change

- The `0xaa` frame with the stock flag condition reports `KEY_F4` press, release and `SYN`, then the
  irq thread sleeps 100 ms after releasing the lock, as stock.
- `KEY_F4` is advertised. `KEY_MENU`, `KEY_BACK`, `KEY_HOMEPAGE` and the other stock capabilities are
  not (virtual keys are not part of this driver).
- As in stock, an `0xaa` frame that does not meet the flag condition is parsed as a normal report; the
  previous driver dropped it.
- Not reported while the controller is asleep (blanked screen), as in stock.
