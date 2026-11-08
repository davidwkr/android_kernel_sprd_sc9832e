# Touch wake (double tap to wake) per driver

One section per touch driver: switch path, default, key, screen-off states and an evidence summary.
Screen state comes from the ADF blank notifier in every driver.

## hynitron_cst9217

- Driver: `drivers/input/touchscreen/hynitron_cst9217.c` (input device `hyn_ts`, DT `hynitron,hyn_ts`).
- Switch: `wake_gesture` on the input device, `/sys/class/input/input<N>/wake_gesture`, `0` or `1`, rw.
  Stock has no switch (see `HYN_CST9217_INTERFACE.md`), so this name is new.
- Default: 1 (the stock driver always handles the gesture report in screen-off).
- Key: `KEY_WAKEUP` (143), the stock key. `Generic.kl` maps it to `WAKEUP`, without `WAKE`.
- Screen on: normal touch.
- Screen off, switch 1: sleep command `d1 05`, irq enabled and `enable_irq_wake`, a report with 0xf1 in
  byte 4 gives a KEY_WAKEUP press and a wakeup event.
- Screen off, switch 0: sleep command `d1 05`, `disable_irq`, no wake source.
- Screen on again: reset (low 21 ms, high, 40 ms), `d1 09`, MT slots released, irq enabled.
- System suspend: no I2C while the bus is suspended; a screen change or a gesture report that arrives
  then is applied on resume.
- Evidence: [D] stock K18 kernel (`cst3xx_touch_report`, `hyn_enter_deep_sleep`, `hyn_resume`,
  `ts_adf_event_handler`, `hynitron_touch_setmode`). Open: the gesture type (double tap) that makes the
  firmware emit 0xf1, and the sleep current, cannot be checked without a device.
