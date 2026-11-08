# SC7A22 / SC7A20 stock interface

What the stock vendor kernel's `sc7a22` driver exposes, and how `drivers/input/misc/sc7a22.c`
reproduces it. Nothing here comes from source: the stock driver is built into the stock `Image`.

Evidence tags:

- **[D]** disassembly of the stock kernel, as `function+offset`. The reference is the K18 Image
  (4.4.83; symbols recovered from its kallsyms, function addresses `0xffffff8008571e88` to
  `0xffffff8008573b3c`). Section "Firmware revisions" says where DM63 / MT107 differ.
- **[H]** the sensors HAL `sensors.sp9832e.so` (ARM32, K18 copy; the copy from the DM63-class
  system dump has the same Acc strings and the same scale constant).
- **[J]** stock JNI libraries in the system dump (`libjni_step.so`, `libjni_sensor.so`,
  `libeqc_algorithm_jni.so`), and the companion app notes, which are the real users of the step
  nodes.

## Which stock image is which

| Image | Driver revision | Notes |
|---|---|---|
| K18 (`kernel.Image`) | B | fully disassembled |
| DW100U (`dw100u/boot.img`) | B | same step constants (0x4f/0x33/0x9d/0x24/0x04/0x8c) and `0x08 = 0x21` as K18; other functions not diffed |
| DM63 (`aod/dm63_kernel.Image`, `dm63/boot.img`) | A | disassembled where it differs |
| MT107 (`aod/mt107_kernel.Image`) | A | same constants as DM63 (checked only the two anchor sequences) |

No image was available for MT108, DM82 or DW99.

## HAL ABI match

The sensors HAL does **not** open `step_value`, `step_clean`, `hand_start`, `motion_state`,
`layout`, `/sys/qmax981_step/clear_step` or any key device. [H] It uses only:

| HAL use | Where | Driver |
|---|---|---|
| input device with `name` = `accelerometer`, found through `/sys/class/input/input%d/name` | `AccSensor::AccSensor` / `SensorBase` | match |
| EV_ABS codes 0, 1, 2 (X, Y, Z), EV_SYN; 32-entry circular reader | `AccSensor::readEvents` 0x9224, `cmp #2` at 0x92f2 | match |
| enable: write to `/sys/class/xr-gsensor/device/gsensor` | `AccSensor::setEnable` 0x8eb0 (`write_sys_attribute`) | match |
| delay in ms (ns divided with `__aeabi_ldivmod`) to `.../device/delay_acc` | `AccSensor::setDelay` 0x907c | match |
| scale `9.80665 / 1024` per count (float 0x3c1ce80a at 0x94a4) | `readEvents` `vmul` at 0x9304 | **mismatch, see open items** |

`chipinfo` appears in the HAL strings only for the Pls, Encoder and Heartrate classes;
`SensorBase::getChipInfo` (0x7d64) is an empty function and `AccSensor` does not override it.

The step, motion, clear and hand_start nodes are used by the stock launcher JNI [J]:

- `libjni_step.so`: finds the input device named `accelerometer` (`/sys/class/input/input%d/name`),
  then opens `<path>/step_value` and `<path>/motion_state` (thread `read_date` at 0xbb0:
  `__read_chk(fd, buf, 8, 8)`, then `atoi`) and writes `<path>/step_clean`.
- `libjni_sensor.so`: same lookup, writes `<path>/hand_start`.
- Companion app `HardwarePedometer` / `RaiseToWake`: `/sys/devices/virtual/input/input*/step_value`
  and `hand_start`, found by input name.
- `libeqc_algorithm_jni.so` opens `/dev/qmax981` and uses its ioctls. That is the QMA6981 driver's
  misc device, not this one.

Consequences for the driver: the nodes must be attributes of the `accelerometer` input device, and
that device has no parent (it lives under `/sys/devices/virtual/input`). The values are read
with at most 8 bytes and `atoi`, so the show formats below are exactly what they parse.

## Sysfs

All eight attributes are created with `sysfs_create_group` on the `accelerometer` input device
(`sc7a22_i2c_probe+0x7e0`; table `sc7a22_attributes` at `0xffffff800914ae58`) [D], all with mode
0644 even when the show or store method is NULL. `gsensor` and `delay_acc` are also files of the
`xr-gsensor` class device `device` (`probe+0x840`) [D], which is what the HAL opens.

| Node | Methods | Format / semantics [D] |
|---|---|---|
| `gsensor` | `sc7a22_enable_show` / `_store` | store: `simple_strtoul` base 10; `1` enables the poll work, anything else disables. show: `"%d\n"` |
| `delay_acc` | `sc7a22_delay_show` / `_store` | store accepts 1..199 ms. show: `"%d\n"` |
| `layout` | `sc7a22_layout_show` / `_store` | store (0..7) only changes the software axis map (`sc7a22_set_layout`). show: `"%d\n"` of the DT value |
| `chipinfo` | `show_chipinfo_value` 0x8572804 | `"sc7a22c\n"` for ID 0x13, `"sc7a22h\n"` for 0x18, else `"unknow!\n"`; returns 8 |
| `hand_start` | `sc7a22_hand_start_store` 0x8572758 | store-only. `1`: hand_start = 1, msleep(10), `device_init_wakeup(dev, 1)`. `0`: `device_init_wakeup(dev, 0)`, hand_start = 0, msleep(10). Other values ignored |
| `step_value` | `sc7a22_step_value_show` 0x8572d70 | show-only, `"%d\n"` (below) |
| `motion_state` | `sc7a22_motion_state_show` 0x8572334 | show-only, `"%x\n"` of (reg & 0x30); reg 0x14 (0x13) or 0x3b (0x18), no bank switch |
| `step_clean` | `sc7a22_step_clean_store` 0x8572138 | store-only, `simple_strtoul` as u8; acts on `1` only |

The driver follows these, except `delay_acc` (kept at the existing 1..1000 ms clamp: the stock
199 ms limit would silently drop the HAL's 200 ms NORMAL rate) and `gsensor` show (the stock one
prints a field that is never written, so it always reads 0; ours prints the real state).
`layout` store/show keep the stock quirk (store does not change what show prints).

## Step counter

The counter is in the chip on both variants; there is no software pedometer in the kernel.

**0x13 part**

- Configure (`sc7a22_set_step_mode+0x1c`): 0x10 = 0x05, 0x11 = 0x20, 0x18 = 0xbc, 0x19 = 0x1b,
  0x1a = 0x52. [D]
- Read (`step_value_show+0x4c`): block read of 3 bytes at 0x95 (register 0x15 with auto-increment),
  count = b0 | b1 << 8 | b2 << 16. [D]
- Clear (`step_clean_store+0x58`): read 0x10, write 0x04 to 0x10, write the original value back.
  [D]

**0x18 part**

- Configure (`set_step_mode+0x60`): bank 0x90 (`0x7f`), then 0x3a = 0x05, 0x46 = 0x4f,
  0x47 = 0x33, 0x48 = 0x9d, 0x49 = 0x24, 0x4a = 0x04, 0x4b = 0x8c, then bank 0. [D]
- Read (`step_value_show+0x70`): bank 0x90, block read of 2 bytes at 0xbc (0x3c), bank 0, read
  register 0x01 as an ID check; count = b0 | b1 << 8. [D]
- If that ID is not 0x18 the chip has lost its configuration: the running total becomes the
  offset (`+0x120`), then `sc7a22h_initialize`, `set_hand_up_mode` and `set_step_mode` run again.
  The call returns the last total. [D]
- Clear (`step_clean_store+0x8c`): bank 0x90, 0x3a = 0x04, 0x3a = 0x05, bank 0. [D]

**Both**: displayed value = chip count + offset. It is stored as the last value only when it is
at most 0xffff (`step_value_show+0x194`), so above 65535 steps the stock node stops moving. The
driver keeps this. `step_clean` also zeroes the stored value and the offset. [D]

## Input devices

| Device | Capabilities [D] | Events |
|---|---|---|
| `accelerometer` (BUS_I2C) | ABS_X/Y/Z -8192..8192; ABS_THROTTLE 0..3 | poll work at 0x85728d8 reports X, Y, Z (s16, layout applied), then ABS 6 = 3, then SYN (`+0x3c`..`+0x88`) |
| `sc7a22_input_key` (BUS_HOST) | EV_KEY and EV_ABS bits; keys 64 (KEY_F6), 68 (KEY_F10), 116 (KEY_POWER) (`probe+0x5d8`) | KEY_F10 only, below |
| `sc7a22_input_step` (BUS_HOST) | EV_ABS ABS_X 0..0xffff, EV_REL REL_X (`probe+0x680`) | none: the only references to this device are the store at probe and the unregister |

Key event, `sc7a22_irq_work_fun+0x10c` [D]: when flag bit 1 (0x02) is set, `hand_start` is 1 and
the screen is off, it sends KEY_F10 value 1, KEY_F10 value 0, SYN_REPORT. Flag bit 2 (0x04)
with `hand_start` set and the screen on only prints `KEY_F6`; revision B sends no event
(revision A sent KEY_F6 press and release).

The screen state is `adf_notifier_callback` (0x8572b30): `ADF_EVENT_BLANK` with `DRM_MODE_DPMS_ON`
sets "screen on", `DPMS_OFF` sets "screen off". Initial values: screen on, `hand_start` 0
(`probe+0x7ac`).

## Interrupt and wake

- DT `irq-gpio` is read with `of_get_named_gpio_flags`, requested as `sc7a22-hand-up` and set as
  input (`probe+0xf0`..`+0x1a0`). The line is converted with `gpiod_to_irq` and requested with
  `request_threaded_irq(irq, sc7a22_handle_fun, NULL, flags = 4 (IRQF_TRIGGER_HIGH),
  "sc7a22-hand-up-irq", NULL)` (`probe+0x598`). [D]
- `sc7a22_handle_fun` 0x857285c: `disable_irq_nosync`, `pm_stay_awake` if the device may wake,
  queue the irq work, return IRQ_HANDLED. [D]
- `sc7a22_irq_work_fun` 0x857299c: take the irq mutex, `pm_relax`, read the flag register (0x06
  for 0x13; bank 0x90 register 0x3e for 0x18), print it, send the key if applicable,
  `enable_irq`. [D]
- Probe calls `device_init_wakeup(dev, 1)` (`probe+0x66c`), so the line is a wake source from
  boot even while `hand_start` is 0. `sc7a22_suspend` 0x8571f2c / `sc7a22_resume` 0x8571e9c call
  `irq_set_irq_wake(irq, 1 / 0)` when the device may wake, and stop / restart the poll work if it
  is enabled. [D]

## Hand-raise registers and `angle`

`sc7a22_set_hand_up_mode` 0x8572ba0 [D]:

| Chip | Writes |
|---|---|
| 0x13 (`+0x20`) | 0x08 = 0x21 (revision B; 0x11 in revision A), 0x09 = layout \| 0x70, **0x0a = angle**, 0x0b = 0xc8 |
| 0x18 (`+0x64`) | bank 0x90; 0x3e = 0x10, 0x51 = layout \| 0x70, **0x52 = angle**, 0x53 = 0xc8; bank 0; 0x06 = 0x15 |

`angle` is the DT property `angle` (`probe+0x150`), default 10 (0x0a) when absent; 0x75 on K18.
The old driver wrote 0 there.

`sc7a22h_initialize` 0x8572258 (0x18 only): bank 0, 0x4a = 0xa5, about 50 ms, bank 0,
0x7d = 0x04, about 30 ms, then 0x40 = 0x26, 0x41 = 0x00, 0x05 = 0x50, 0x08 = 0x05.
0x13 base configuration (`probe+0x338`): 0x20 = 0x4f, 0x23 = 0x88, 0x1f = 0x08, 0x57 = 0x08.

## Firmware revisions

DM63 and MT107 run revision A, K18 and DW100U revision B. The ABI above is the same; internals
differ:

| Item | Revision A (DM63, MT107) | Revision B (K18, DW100U) | Driver |
|---|---|---|---|
| 0x13 hand-up register 0x08 | 0x11 | 0x21 | DT `hand-up-ctrl`, default 0x11, 0x21 set in K18 and DW100U DTs |
| 0x18 step parameters | 0x47 = 0x23, 0x48 = 0xcf, 0x49 = 0x1c; 0x3a = 5 written in bank 0 after the bank 0x90 block | 0x33, 0x9d, 0x24; 0x3a = 5 inside bank 0x90 | revision B |
| 0x18 step read / clear | bank 0 | bank 0x90 | revision B |
| Step total | the irq work adds the chip count to a software total and clears the chip; show = total + chip | no accumulation; offset kept only across a chip reset | revision B |
| IRQ trigger | chosen from the pin level, flipped between level high and low after each event | fixed level high | level high |
| KEY_F6 | sent | log only | log only |
| `step_clean` | clears total and chip | also clears the offset | revision B |

## Not in this driver

`/sys/qmax981_step/clear_step` is not part of the sc7a22 driver. It is a kobject created in the
init function of the stock **qmaX981** driver (`qmaX981_i2c_init+0x28`, `kobject_init_and_add`,
parent NULL), and its store writes register 0x13 of the QMA chip through that driver's own client
(`clear_step_enable`: 0x13 = 0x80 three times with about 11 ms between, then 0x80, 0x7f). It
exists whenever that driver is built in, whether or not the chip is present (on a board without
the chip, stock dereferences a NULL client). It is therefore not implemented here. It belongs in
`qma6981.c` if it is wanted.

The stock misc device is named `sc7a22` with an ioctl that returns 0 (`sc7a22_unlocked_ioctl`
0x8571e88) [D]; this driver still registers `qmax981` with the old ioctls.

## Open items

1. Accelerometer scale: now reported undivided like stock [D `read_accel_xyz+0x274`]. With
   `0x23 = 0x88` (+-2 g, high resolution) that is ~1024 counts per g, which is what the HAL's
   9.80665 / 1024 [H] expects. The earlier driver divided by 2 (0x13) or 4 (0x18), i.e. half or a
   quarter of gravity in the HAL. Confirm on a unit: a watch lying flat should read ~9.8 m/s2 on Z.
2. Misc name collision. On K18, MT107, MT108 and DM82 both `qma6981.c` and this driver register
   a misc device named `qmax981`, and both create the `xr-gsensor` class. Stock names this one
   `sc7a22`. Not changed.
3. The old driver wrote 0x06 = 0x15 and 0x08 = 0x11 on the 0x13 part. Neither revision writes
   0x06 on that part, so it was dropped. This is untested on hardware.
4. No known 0x18 unit; those paths follow revision B and were never exercised.
5. Wake is armed at probe as in stock; if the battery cost is unwanted, drop the
   `device_init_wakeup(dev, true)` call in probe and rely on `hand_start`.
6. Boards without a stock image (MT108, DM82, DW99) keep `hand-up-ctrl` 0x11.
7. IRQ level high with no rearm logic: if the chip keeps INT high after the flag read, the work
   re-runs continuously (same as stock revision B). Not testable offline.
8. Nothing here was run on a device; the build check compiles `sc7a22.o` only.

## 4.14 port notes (`kwt-414k`)

- The stock ADF blank notifier (`<video/adf_notifier.h>`) does not exist on 4.14: the sprd display
  is DRM/KMS (`drivers/gpu/drm/sprd`), which has no screen state notifier, `sprd_drm` sets up no
  fbdev emulation and the sprd backlight is not mapped on the watch boards. The driver registers
  an fb blank notifier (`fb_register_client`, `FB_EVENT_BLANK`; `FB_BLANK_UNBLANK` is screen on,
  `FB_BLANK_POWERDOWN` screen off) when `CONFIG_FB_NOTIFY` is set. Nothing in the 4.14 sprd stack
  raises that event, so the screen stays "on" (its initial value) and the raise-to-wake key
  (KEY_F10, only sent with the screen off) is never sent. A display driver hook that calls
  `fb_notifier_call_chain(FB_EVENT_BLANK)` from `sprd_panel_enable/disable` would enable it.
- Because of that the DTS maps `hand-up-ctrl` (K18, DW100U) but not `irq-gpio` and `angle`: the
  interrupt would only cause wakeups without a key. Add them together with the display hook.
- Step counter, sysfs ABI and `/dev/qmax981` do not depend on the screen state.

