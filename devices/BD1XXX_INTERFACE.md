# BD1xxx (Rohm BD1820) PPG heart-rate sensor: stock kernel/HAL interface

Applies to the k18 watch: DT node `bd1xxx_hr@38` on i2c-2, `compatible = "heartrate,bd1xxx"`,
`reg = <0x38>`, `vdd_name = "vddcammot"`, `power-gpio = <&gpio 55 0>` (0x37). Fitted chip: BD1820.

Evidence tags used below:

- **[D]** verified from disassembly of the stock 4.4.83 kernel Image (driver `bd1xxx`, built in,
  `CONFIG_INPUT_SENSORS_BD1XXX=y`, `CONFIG_HEARTRATE_BD1820=y`; `BD1662`/`BD1688` variants are
  not enabled in stock). Symbols were recovered from the kallsyms tables in the Image.
- **[H]** from the vendor HAL `sensors.sp9832e.so` (32-bit ARM, class `HeartrateSensor`): strings
  and disassembly.
- **[I]** inferred, not directly confirmed.

No device was accessed: every fact comes from the two files above and the board DT.

## Overview

Unlike HRS3XXX, the userspace interface is a **misc character device plus an input device**.
The HAL does the HR/SpO2 maths (`BD_health_pro`, `BD_Spo2_math`, `BD_HRV_math`) on raw ADC samples
it reads from the misc device [H]. The kernel driver also contains vendor measurement code
(`BD_Sensor_Init/Start/Close/Data_Read`, `BD_finger_math`, `Wear_judge*`, `gensor_*_lpf`, which uses
the accelerometer) [D]; that code is not reimplemented.

## Probe sequence [D] (`bd1xxx_i2c_probe`)

1. `i2c_check_functionality` style check on the adapter (the code calls the adapter's functionality
   op and returns -ENODEV if bit 0 (I2C_FUNC_I2C) is missing).
2. Allocate driver data (968 bytes). Parse DT (`of_node` of the client):
   - `vdd_name` read as string; if present: `regulator_get(&client->dev, <vdd_name>)` is
     used (log `User %s power supply`), `regulator_set_voltage(2800000, 2800000)` (0x2ab980 uV),
     `regulator_enable`. If the property is absent: log `Default power supply`, no regulator.
     If `regulator_get` returns an error the stock code executes a `brk` (bug); a replacement
     should simply continue without a regulator.
   - `power-gpio` via `of_get_named_gpio_flags`; if valid (< 512): `gpio_request(.., "bd1xxx-power-gpio")`,
     `gpiod_direction_output_raw(0)`.
   - DT parse failure: log `bd1xxx_parse_dt is fail`, free, return -1.
3. Power on: set the power GPIO high (raw value 1), `msleep(10)`.
4. Chip check (`BD_I2C_*` helpers, all single byte registers):
   - write reg 0x03 = 0x0A, `msleep(10)`, read back reg 0x03 (must equal 0x0A, "check");
   - read reg 0xBD ("chipid") and reg 0xBC.
   - logs `check:0x%x chipid:0x%x chipid:0x%x`.
5. Power off the power GPIO (raw 0).
6. Decision: reg 0x03 readback != 0x0A -> `The chpi ID is failed`, probe fails (-1).
   Otherwise chip model = 0x1820 if (reg 0xBD == 0x70 and reg 0xBC == 0x11), else 0x1662
   (stored at driver data +956; any other combination also selects 0x1662).
7. `input_allocate_device`, then register the input device and misc device and the sysfs group
   (see below); on failure unwinds (disable regulator, free gpio).
8. Initialises an hrtimer (CLOCK_MONOTONIC, REL mode) with callback `bd1xxx_hearterate_timer_func`
   and a work item (`bd1xxx_heartrate_work_func`). After probe the regulator (if any) is disabled
   again; the sensor starts unpowered and idle.

Remove [D]: removes sysfs group, `misc_deregister`, `input_unregister_device`, `gpio_free`,
`cancel_work_sync`, `hrtimer_cancel`, `kfree`.

I2C adapter/client: the driver id table name is `bd1xxx`, of_match `heartrate,bd1xxx` [D].

## I2C transfers [D]

- Read one register: single `i2c_transfer` with two messages: write 1 byte (register), then read
  N bytes (flag I2C_M_RD); both at `client->addr` (0x38). Repeated start, not STOP + start.
- Write: one message of 2 bytes `[reg, value]`.
- Vendor code passes address 0x70 (= 0x38 << 1) to its helpers but the helpers ignore it and use the
  client address.

## Register facts (BD1820)

| Reg | Use | Source |
|---|---|---|
| 0x00 | control: 0x4B written last in each init block (start), 0x00 / 0x80 / 0x00 written on close (soft reset sequence) | [D] call sites (grouping [I]) |
| 0x03 | probe: write 0x0A then read back; also configuration (values 0x14 / 0x02 in init blocks) | [D] |
| 0xBC, 0xBD | chip ID bytes: 0xBD == 0x70 and 0xBC == 0x11 for BD1820; init also reads 0xBE, 0xBF | [D] |
| 0x18..0x1F | 4 x 16-bit result registers read per sample (pairs 0x1A/0x1B, 0x18/0x19, 0x1C/0x1D, 0x1E/0x1F; byte order not decoded) | [D] call sites |
| 0x20..0x25, 0x30 | further per-sample status / data reads | [D] call sites, meaning [I] |
| 0x32, 0x33, 0x34, 0x0E | written during the data read (LED / gain / mode control per phase) with computed values | [D] call sites, values not decoded |
| 0x04 | written during read (0x01 seen) | [D] |

Init (`BD_Sensor_Init(mode, ...)`, 7 KB function): reads 0xBC..0xBF, selects on chip id and on
mode (`mode == 2` or `2..4` take the configuration path; other values go to another branch
that was not analysed), then writes three straight-line configuration blocks (one of which is
chosen by mode/chip variant; the mapping of block to mode was **not** resolved). Writes seen, in order
(reg=value), block A: 01=08 03=14 04=00 05=A0 06=21 07=78 08=08 09=29 0A=B1 0B=00 26=8F 27=81 2A=20
2B=00 0E=00 0F=00 16=63 2F=33 32=00 33=00 34=00 38=00 39=00 3A=00 38=03 39=03 3A=03 00=4B.
Block B: 01=01 03=02 04=02 05=A4 06..0F,26,27,2A,2B,2F,32..3A as in A, 16=A0, then 00=4B. Block C: like B with
03=14, 04=14, 16=A0, 00=4B. This was extracted by scanning `mov` immediates before each write
call (linear, branch-blind), so treat the per-register values as [D] for existence and [I] for
which mode uses which block. The correct programming order and the unresolved branches are
the main open item for a working measurement.

## Input device [D][H]

- Name `heartrate` (string at +0), `id.bustype = 0x18` (BUS_HOST), `evbit = 6` (EV_KEY, EV_REL),
  `keybit` bit 63 (KEY code 63), `relbit = 7` (REL_X, REL_Y, REL_Z). No parent device;
  registers under `/devices/virtual/input/inputN`.
- Events from the work function [D]:
  - modes other than 15: REL_X = u16 sample 0 of channel 1, REL_Y = return value of
    `BD_Sensor_Data_Read` (always 1 when reported), REL_Z = u16 sample 0 of channel 3
    (channel arrays at byte offsets 0, 70, 140 of a 400-byte buffer), then EV_SYN. Reported only
    when `BD_Sensor_Data_Read` returned 1.
  - mode 15 (finger/`BD_finger_math`): if result == 2: EV_KEY code 63 value 1, SYN, EV_KEY 63 value 0, SYN.
- The HAL [H] only uses the SYN: `readEvents` ignores EV_REL (type 2), treats SYN (type 0) as "new
  data" (stamps the time, reads 200 bytes from the misc device and calls `BD_health_pro`) and logs
  any other type (EV_KEY) as unknown. So exact REL values do not matter to the HAL.
- The HAL finds the node by scanning `/sys/class/input/input*/name` for `heartrate`, opens
  `/dev/input/eventN`, sets `EVIOCSCLOCKID` to CLOCK_BOOTTIME.

## Misc device `/dev/bd1662rate` [D][H]

- `misc_register` with `MISC_DYNAMIC_MINOR`, name **`bd1662rate`** (the stock name regardless of the chip),
  fops: `open` (nonseekable_open), `read`, `write`, `release`. The HAL opens it `O_RDWR` on the first enable.
- `write(fd, buf, 20)` [H, D]: HAL sends a 20-byte buffer, byte 0 = test mode, byte 1 = enable (0/1).
  Kernel copies up to the user count into a stack buffer, stores `buf[0]` as the mode and then starts
  (`buf[1] != 0`) or stops (`buf[1] == 0`) the sensor, logging `test_mode:0x%x enable:%s`.
  The stock write handler returns the copy_from_user remainder (0 on success), not the byte count
  [D]; the HAL only logs the value. If `access_ok` fails the buffer is zeroed (mode 0, disable).
  HAL mode comes from `persist.sys.heartrate_test_mode` (default string `0`): values 1..12 map to
  themselves, 15 -> 15 (finger), anything else (0, 13, 14, >15) logs "not support, default user BD_SBPM
  mode" and uses 1 [H].
- `read(fd, buf, 200)` [H, D]: copies the first `count` bytes (returns `count`, -EFAULT on failure) of the 400-byte raw buffer saved by the work function
  (200 bytes = 100 u16 little endian samples, which the HAL passes to `BD_health_pro`). Kernel
  also `printk`s the first 40 u16 words. The HAL calls read once per SYN event.

## sysfs attributes (on the input device) [D]

| Attribute | Mode | Format |
|---|---|---|
| `enable` | 0664 (0x1a4) | store: decimal `kstrtol`-style (`kstrtol`-like helper, base 10); 0 stops, nonzero starts using the stored mode. show: `%d\n` of a state field that the stock driver never updates (always 0) |
| `chipinfo` | 0664 (0x1a4, show only so not writable in practice) | `bd1820` (or `bd1662`), 6 bytes, **no newline**; selected by the chip model decided at probe |

The HAL reads `chipinfo` from `/sys/class/input/inputN/chipinfo` and picks its sensor name from it [H];
it does **not** use the sysfs `enable` (HAL start/stop goes through the misc device write) [H].
There is no delay/period attribute.

## Start / stop and timing [D]

- Start (`bd1xxx_init_heartrate(1)` / write enable): enable regulator (if any), power GPIO high,
  `msleep(10)`, zero the 400-byte raw buffer, `BD_Sensor_Init` + `BD_Sensor_Start` (modes 2, 15 and others
  differ only in the argument sets passed), start the hrtimer: mode 15 -> 200 ms, others -> 50 ms.
- Timer callback: re-arms itself (forward by 200 ms for mode 15, else 50 ms) and schedules the work item.
- Work function: calls `BD_Sensor_Data_Read` (or `BD_finger_math` for mode 15), copies the 400-byte buffer
  to the read buffer and reports input events (above).
- Stop: cancel timer, `msleep(2)`, `BD_Sensor_Close` (writes reg 0x00 = 0x00, 0x80, 0x00), clear buffer,
  power GPIO low, regulator disable.
- The mode written by the HAL is stored in driver data (+952) and used by the next start.
- Interrupts: **none** (no IRQ in DT, no request_irq); data is polled by the 50 ms timer.

## Suspend / resume [D]

PM ops only log `bd1xxx_i2c_suspend` / `bd1xxx_i2c_resume` and return 0: the sensor is not touched.

## Not recovered / open items

- Mapping of `BD_Sensor_Init` blocks to modes, the exact register order and the `BD_Sensor_Data_Read`
  state machine (alternating LED phases on reg 0x32..0x34 / 0x0E / 0x04), byte order of regs 0x18..0x1F.
- Layout of the 3 x 35 sample arrays versus the 100-sample read buffer (the HAL may expect
  an interleaved layout; only the first 200 bytes of the buffer are read).
- Accelerometer coupling (`gensor_*_lpf`, `Wear_judge`) is in the kernel path; the HAL gets its
  accelerometer through a separate sensor (`heartrate_acc_enable`, `setAccel`).
