# Vibrator (timed_output) on 4.14

**Compile-only. Untested on hardware.**

The stock 8.1 vibrator HAL (`vibrator.default.so`, `android.hardware.vibrator@1.0-impl`) drives
`/sys/class/timed_output/vibrator/enable`. 4.14 has neither the Android timed_output class nor the
SPRD vibrator driver; it only has the input force-feedback `sc27xx-vibra`
(`CONFIG_INPUT_SC27XX_VIBRA`), which the HAL does not use. Ported from the 4.4 product tree
(`security-backports`):

- `drivers/staging/android/timed_output.c`, `include/linux/timed_output.h`
  (`CONFIG_ANDROID_TIMED_OUTPUT`): class with `dev_groups`, drvdata passed to `device_create()`
  (no window where the node exists without drvdata), class creation serialised so a driver that
  probes before the initcall still works. `timed_output_dev_unregister()` no longer calls
  `enable(0)`; the driver powers the LDO down itself after the node is gone.
- `drivers/soc/sprd/sprd_vibrator.c` (`CONFIG_SPRD_VIBRATOR`, depends on
  `ANDROID_TIMED_OUTPUT && MFD_SC27XX_PMIC && !INPUT_SC27XX_VIBRA`).

`timed_gpio` (`CONFIG_ANDROID_TIMED_GPIO=y` on stock) is not ported: it only takes platform data
from board files, has no DT binding, and no watch DT uses it.

## Sysfs ABI

`/sys/class/timed_output/vibrator/enable`

- write: duration in ms (`kstrtoint`, base auto). `<= 0` stops, values above 15000 are clamped to
  15000 (as 4.4). A new write while running restarts the timer. Garbage gives `-EINVAL`.
- read: milliseconds remaining, 0 when idle (4.4 returned nanoseconds).

`enable()` is safe in atomic context: it only arms an hrtimer and queues a work item; the PMIC
(SPI regmap) access happens in the work item.

## DT binding

Unchanged: `vibrator@eb4` in `sc2721.dtsi`, `compatible = "sprd,sc27xx-vibrator",
"sprd,sc2721-vibrator"`, `reg = <0xeb4>` (`ANA_VIBRATOR_CTRL0`), child of the SC27xx PMIC, whose
regmap is used. Both `sc27xx-vibra` and `sprd_vibrator` match `sprd,sc2721-vibrator`, so
`watch_414_stock81.fragment` selects this one and sets `# CONFIG_INPUT_SC27XX_VIBRA is not set`
(Kconfig enforces the exclusion). The driver also matches `sprd,sc2731-vibrator`,
`sprd,sc2723t-vibrator` and `sprd,sc2720-vibrator` (4.4 compatibles); the SC2730 register layout
differs and is not supported here.

Hardware sequence as 4.4: LDO trim in `ANA_VIBRATOR_CTRL0[7:0]` (0xb4; 0x2 on SC2720), then
`LDO_VIBR_PD` (bit 8) and `SLP_LDOVIBR_PD_EN` (bit 9) cleared to run, set to stop.

## Teardown and suspend

Remove: unregister the sysfs node (drains in-flight writes), `hrtimer_cancel()`,
`cancel_work_sync()`, then power the LDO down. Suspend does the same cancel and power-down, so a
vibration is cut off on suspend.
