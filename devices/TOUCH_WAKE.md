# Touch wake behaviour per driver

One section per touch driver: switch path, default, key, screen-off states, evidence summary.

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
