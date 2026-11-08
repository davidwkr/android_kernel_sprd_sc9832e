# Touch controller screen-off behaviour

One section per touch driver. Common ABI of the touch drivers:

- `wake_gesture` (0/1, read/write) on the touch input device
  (`/sys/class/input/inputN/wake_gesture`): double-tap-to-wake (DT2W).
- Wake key: `KEY_WAKEUP` (143; `Generic.kl` maps it to `WAKEUP`, and Android treats it as a wake key).
- Screen state: UNISOC ADF blank notifier (`ADF_EVENT_BLANK`; `DRM_MODE_DPMS_ON` / `DRM_MODE_DPMS_OFF`).
- Screen on: normal touch. Screen off with DT2W enabled: gesture mode, irq armed as a wake source.
  Screen off with DT2W disabled: deep sleep, irq disabled, not a wake source.
- Tags: **[D]** verified from disassembly of the stock 4.4.83 kernel Images (DM63 `boot.img`, DW100U
  `boot.bin`, K18 `kernel.Image`); **[I]** inferred.

## cst0xx_ts

Driver: `drivers/input/touchscreen/cst0xx_ts.c` (Hynitron CST0xx, I2C 0x38).

| Item | Value |
|------|-------|
| Switch | `wake_gesture` on the `cst0xx_ts` input device |
| Default | 0 (stock has no switch and no gesture mode) |
| Write 1 | fails with `-EOPNOTSUPP` (no gesture mode is evidence-backed); write 0 succeeds; other values `-EINVAL` |
| Key | `KEY_WAKEUP` is advertised for the ABI but never reported: no gesture can be armed |
| Screen on | normal touch; controller reset on unblank, irq enabled, all fingers released |
| Screen off, DT2W off (the only state) | sleep command `E5 03`, `disable_irq`, no wake source |
| Screen off, DT2W on | not available |

### Evidence [D]

Identical in the three stock Images (the cst0xx functions compare equal after address normalisation).
Functions: `adf_notifier_callback`, `touch_event_handler`, `cst0xx_reset`, `cst0xx_interrupt`,
`cst0xx_probe`, `cst0xx_i2c_Write`.

- **Blank handling** (`adf_notifier_callback`): acts only on event `0x10` (`ADF_EVENT_BLANK`) with
  non-null data; the state is the `int` behind `data`.
  - State 3 (off): builds the two bytes `E5 03` and writes them in one `i2c_transfer` (flags 0, len 2) with
    `cst0xx_i2c_Write`, then `disable_irq(irq)`. This is the stock sleep command: register 0xE5 = 0x03.
    (Not 0xA5 as in `btl_ts`.)
  - State 0 (on): `cst0xx_reset`, then 200 x `udelay(1000)` (about 200 ms), `enable_irq(irq)`.
  - Other states (1, 2): ignored.
  - No fingers are released by stock on unblank; the driver does it (task requirement).
- **Reset** (`cst0xx_reset`): reset line high, 1 ms, low, 51 x 1 ms (about 50 ms), high.
- **ADF client priority**: 1000 (`probe`, stores 1000 in the notifier block).
- **IRQ request**: flags `0x6002` = `IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_NO_SUSPEND`, hard handler
  `cst0xx_interrupt` (sets a flag and wakes a kthread). No `enable_irq_wake`, `irq_set_irq_wake` or
  `device_init_wakeup` call in the cst0xx code.
- **Gesture report only, no enable**: `touch_event_handler` reads 33 bytes from register 0. If byte 1 is
  `0xAA` and `(byte 3 >> 4)` is 0 or 4, it sends `KEY_F4` (code 62) press and release plus `SYN`, then
  `msleep(100)`. Probe sets keybits `KEY_F4`, `KEY_MENU`, `KEY_BACK`, `KEY_HOMEPAGE` and `BTN_TOUCH`. The
  only I2C writes in the cst0xx code are the sleep command above and the raw `cst0xx_rw_iic_drv` ioctl
  (user space). `cst0xx_read_id` only reads. Nothing writes a gesture enable register (0xD0, 0xE5 with another
  value, etc.), so how the controller would emit the 0xAA frame is unknown.
  The in-tree driver drops frames with byte 1 = 0xAA as before.
- **Switch**: no sysfs/proc DT2W switch in the cst0xx code. Its sysfs objects are `board_properties`
  (`virtualkeys.cst0xx_ts`) and `chipinfo`; the misc node `cst0xx_rw_iic_drv` is a raw register ioctl.
- **User space** (read only): K18 `system.bin` and DW100U `system.bin` hold no `cst0xx_ts.kl`, so the device
  uses `Generic.kl`: `key 62 F4` and `key 143 WAKEUP`, neither with the `WAKE` flag. Only `gpio-keys.kl` maps
  `F4` to `WAKE`, and it applies to the `gpio-keys` device only. No string for a gesture sysfs/proc path
  (`wake_gesture`, `gesture`, `dt2w`, `double_tap`) was found in `system.bin` or `vendor.bin` (K18) or
  in the DW100U system image. The DM63 `system.img` cannot be read with `debugfs` (short read); only
  `strings` was run on it (nothing found).

### Open items

- A real DT2W needs the controller's gesture enable command (datasheet or firmware tracing on a device).
  When known: add the command to `cst0xx_sync_state`, `enable_irq_wake` on blank, `disable_irq_wake` on
  unblank, report `KEY_WAKEUP` for the gesture frame, and accept 1 in `wake_gesture_store`.
- Whether the AOD path (DM63 `aod_idle`) sends `DRM_MODE_DPMS_OFF`: if it does, touch is asleep in AOD (stock
  behaves the same). Not checked on hardware.
- The previous driver kept the controller awake as a wake source (late PM, `IRQF_NO_SUSPEND`). Now touch does
  not wake the system. Not tested on hardware (offline work).
- The notifier takes about 250 ms on unblank (reset and settle) in the display path, as in stock.

## btl_ts (Betterlife BL6133/BL6138; K18, MT107, MT108, DM82)

Driver: `drivers/input/touchscreen/btl_ts.c`. Details: [BTL_TS_INTERFACE.md](BTL_TS_INTERFACE.md).

| | |
|---|---|
| Switch | `wake_gesture` on the input device `Betterlife_ts`: `/sys/class/input/input<N>/wake_gesture` (0/1, rw). Stock has no switch of its own. |
| Values | read: always `0`. Write `0`: accepted. Write `1`: `-EOPNOTSUPP` (no evidence-backed gesture mode). Other values: `-EINVAL`. |
| Default | 0 (stock has no gesture enable at all). |
| Key | `KEY_F4` (0x3e) for gesture ID 0x17 in the normal mode, as stock. Stock has no `Betterlife_ts.kl`; `Generic.kl` has `key 62 F4` without `WAKE`, so it does not wake Android 8.1. `KEY_WAKEUP` is not reported: nothing in the chip's known behaviour produces it. |
| Screen on | normal touch. |
| Screen off | controller in sleep mode (`A5 03`, as stock), irq disabled, no wake source. Touch does not wake the system. |
| Screen on again | dummy read of 0xA5, reset pulse (50 ms steps), irq enabled, fingers released. |
| Screen source | ADF blank notifier (`ADF_EVENT_BLANK`, `DRM_MODE_DPMS_ON/OFF`). |
| System suspend | late PM ops. No I2C while the bus is down; a screen change that arrives then is applied on resume. With the screen on, touch stays a wake source. |
| Evidence | Stock K18 kernel [D]: blank/unblank sequence in `adf_notifier_callback`, `A5 <mode>` in `bl_chip_switch_mode` (only mode 3 used), no gesture enable, no gesture sysfs/proc node, only gesture 0x17 handled. Stock system/vendor images [D]: no Betterlife `.kl`, no gesture path. |
| Open | A double-tap wake needs a gesture mode of the chip that no source here documents. Nothing was run on a unit. |
