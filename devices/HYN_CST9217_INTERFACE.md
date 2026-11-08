# Hynitron CST9217 (`hyn_ts`): stock kernel interface

Applies to the k18 and dm82 DTs (node `hynitron@5a`, `compatible = "hynitron,hyn_ts"`, i2c-3, address
0x5a). In-tree driver: `drivers/input/touchscreen/hynitron_cst9217.c` (`CONFIG_TOUCHSCREEN_CST9217`).

Evidence tags:

- **[D]** verified from disassembly of the stock K18 kernel Image (`hyn_ts` driver, built in), with the
  kallsyms table of the Image. Offsets are `function+offset`.
- **[F]** from the stock K18 system and vendor images (read only).
- **[I]** inferred, not confirmed. No device was accessed.

## Chip variant [D]

`hyn_ts_data_init` defaults to IC type 0x9217, I2C address 0x5a, and looks the IC type up in the
`hynitron_chip_type_grp` table (34 entries, 16 bytes each, the IC type at +8, the chip class at +12).
0x9217 (also 0x916e and 0x9220) is class 1: 16 bit register addresses, "mutcap". Class 2 (selfcap) is
used by 0x10/0x1a/0x24 and 0x2cc..0x344 and is not relevant here.

## Report [D] (`cst3xx_touch_report`)

Read 7 bytes at register 0xd000 (write `d0 00`, then read). `buf[6] != 0xab` or `buf[0] == 0xab`: invalid,
acknowledged. Acknowledge is `d0 00 ab` (3 byte write) on every path. `buf[4]` is logged as
"gestrue_data". `buf[4] == 0x80` is a key frame (key 62, only while a flag is 1). `buf[4] == 0xf1` is the
gesture report. Otherwise `buf[5] & 0x7f` is the point count (max 5). This matches the touch parser of the
in-tree driver.

## Gesture / DT2W [D]

- **There is no enable command and no switch.** The gesture is not configured by the host.
- Gesture ID: `buf[4] == 0xf1`, handled in `cst3xx_touch_report+0x138` only while the screen-off flags
  (`ts_data` byte at `.data+0xf8` of the driver data and `hyn_gesture_flag`) are both 0, i.e. after
  `hyn_suspend` and before `hyn_resume` (suspend clears both, resume sets both to 1).
- On the gesture the driver sets `hyn_gesture_flag = 1` (debounce until resume), calls `pm_stay_awake`
  on the device, reports `input_event(EV_KEY, 143, 1)`, `EV_SYN`, `(EV_KEY, 143, 0)`, `EV_SYN`, then queues
  a work item (`hyn_wakeup_work_func`, which is `pm_relax`). Key 143 is KEY_WAKEUP. The log string says
  "report KEY_POWER", but the code value is 143 (KEY_POWER is 116).
- The key capability is set in `hyn_input_dev_int`: `set_bit(143, keybit)`, and `device_init_wakeup(dev, 1)`.
  Input device name `hyn_ts`, 2 MT slots (the in-tree driver uses 5, from the DT), BTN_TOUCH.
- The stock driver does not call `enable_irq_wake` / `disable_irq` / `disable_irq_nosync` anywhere in the
  driver. The irq (`hyn_irq_init`) is `request_threaded_irq` with flags `trigger(1, rising) | IRQF_ONESHOT`
  and stays armed in screen-off.
- Whether the firmware emits 0xf1 for a double tap (as opposed to another gesture) cannot be seen from the
  host code: **[I]** unverified without a device.

## Sleep and wake [D]

- Sleep (`hyn_enter_deep_sleep`, chip class 1, logs "Set TP to sleep mode mutcap"): write the two bytes
  `d1 05` with `cst3xx_i2c_write` (`i2c_master_send`, repeated once if it returns <= 0). Class 2 writes
  register 0xa5 = 0x03 instead.
- No power-off, no reset low, no regulator change in sleep.
- Wake (`hyn_resume`): reset pulse low for 21 x 1 ms, high, wait 40 ms (`hyn_reset_proc(40)`); set the
  flags to 1; release all MT slots (`ABS_MT_SLOT`, tracking id -1, `input_mt_report_slot_state(false)`),
  `EV_SYN`; then `hynitron_touch_setmode(client, 0, class == 1)` which sends `d1 09` with `i2c_master_send`.
  The in-tree driver already used 0xd109 as its start command.
- `hynitron_touch_setmode` modes 1, 2 and 3 (`d1 19`, `d1 0a`, `d1 0d` for class 1) are used only by the
  factory test / debug proc files; they are not gesture modes.

## Blank handling [D]

Both a framebuffer notifier (`hyn_fb_notifier_callback`: event 0x10 with blank 0 resumes through a work
item, blank 4 suspends; event 0x11 queues the resume) and an ADF notifier (`ts_adf_event_handler`:
`ADF_EVENT_BLANK` (0x10), `*data == 0` (DPMS_ON) calls `hyn_resume`, `== 3` (DPMS_OFF) calls
`hyn_suspend`, other values ignored) are registered in `hyn_probe`. The in-tree driver uses the ADF one,
like `sc7a22.c`.

## sysfs / proc [D][F]

The stock sysfs attributes (mode 0644) are `hyntpfwver`, `hynfwupdate`, `hyntprwreg`, `hynfwupgradeapp`,
`hyntpfactorytest`, `hyncheckbuf`, and there is the proc file `hynitron_debug` (factory/firmware tool).
None of them is a gesture switch. In the stock K18 system and vendor images no init script, property, app
or HAL (`power.default.so`, `power.sprd.so`, the power 2.0 impl) names a hynitron/gesture path. The sensors
HAL carries `android.sensor.wake_gesture` but no path to this driver.

## Key layout [F]

There is no `hyn_ts.kl` in `/system/usr/keylayout`, so the device uses `Generic.kl`:
`key 143 WAKEUP` (no `WAKE` flag). The files that give `WAKE` are `gpio-keys.kl` (116 POWER, 62 F4) and are
not used for `hyn_ts`. Android 8.1 `PhoneWindowManager` treats `KEYCODE_WAKEUP` itself as a wake key while
the screen is off: **[I]** from the AOSP source, not checked in the image framework.

## DM82 [D]

The stock DM82 kernel has no `hyn_enter_deep_sleep`/"gestrue_data" code: it carries only the `cst0xx`
driver (input name `hyn_ts`, see the CST0xx notes). The DM82 DT node `hynitron@5a` is a candidate for the
in-tree driver; the evidence above is from the K18 kernel.

## In-tree driver

`wake_gesture` (0/1, default 1, mode 0644) on the input device. Screen on: normal touch. Screen off with
`wake_gesture = 1`: `d1 05`, irq stays enabled and is a wake source, a 0xf1 report gives a KEY_WAKEUP
press. Screen off with `wake_gesture = 0`: `d1 05`, `disable_irq`, wake source dropped. Screen on: reset
(21 ms, 40 ms), `d1 09`, fingers released, irq re-enabled. Because the stock driver has a single sleep
command, the controller state is the same in both screen-off modes: only the host side differs.
