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

Each measurement-path item below also carries a confidence (high / medium / low). The stock addresses
are kernel virtual addresses with the `0xffffff80` prefix dropped (`Init+0x2dc` = `0x56d5d4`, symbol base
`0x56d2f8`). Variable names in quotes (`green_current`, `red_setflag`, `data_count`, ...) are the vendor
symbol names from the kallsyms table of the Image [D].

No device was accessed: every fact comes from the files above and the board DT.

## Overview

Unlike HRS3XXX, the userspace interface is a **misc character device plus an input device**.
The HAL does the HR/SpO2 maths (`BD_health_pro`, `BD_Spo2_math`, `BD_HRV_math`) on raw ADC samples
it reads from the misc device [H]. The kernel driver also contains vendor measurement code
(`BD_Sensor_Init/Start/Close/Data_Read`) [D], which `drivers/input/misc/bd1xxx.c` now implements, and
the accelerometer-driven parts (`Wear_judge*`, `gensor_*_lpf`, `BD_finger_math`), which it does not.

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
- Write: one message of 2 bytes `[reg, value]` (flags 0) [D, high].
- Data registers are 16 bit **little endian**: the low byte is at the lower register (0x1a low, 0x1b
  high), read as two separate single-register transfers, low byte first [D, high].
- Vendor code passes address 0x70 (= 0x38 << 1) to its helpers but the helpers ignore it and use the
  client address.

## Register facts (BD1820)

| Reg | Use | Source / confidence |
|---|---|---|
| 0x00 | control: 0x4b written last by Init (run); 0x00, 0x80, 0x00 written by Close | [D] high |
| 0x03 | probe: write 0x0a, read back; Init: 0x14 (modes 2, others) or 0x02 (modes 3, 4) | [D] high; meaning unknown |
| 0x04 | green LED current (`green_current`): 0x00 in mode 2, else 0x51 / 0x59 (below) | [D] value and symbol, high; "LED" is [I] |
| 0x0e, 0x0f | red / IR LED current (`red_current`, `ir_current`) | [D] values high; meaning [I] |
| 0x18/0x19 | 16-bit "ambient / reflect" word (`reflect_abssum1`), stored in trailer word 27 of channel 0 | [D] high; meaning [I] |
| 0x1a/0x1b, 0x1c/0x1d, 0x1e/0x1f | 16-bit samples: channel 0, 1, 2 (green, red, IR by the vendor symbol names) | [D] high |
| 0x20..0x25 | HRV modes: FIFO sample words (0x20/21 ch0, 0x22/23 ch1, 0x24/25 ch2) | [D] high |
| 0x30 | HRV modes: FIFO level; drain while > 2 | [D] high, meaning of the value [I] |
| 0x32, 0x33, 0x34 | DC cancel values for green, red, IR (`green_dc_value1` ...), written by the DC loop | [D] high; meaning [I] |
| 0xbc..0xbf | ID: 0xbc = 0x11, 0xbd = 0x70 for BD1820; 0xbe, 0xbf select the parameter variant | [D] high |
| 0x01, 0x05..0x0b, 0x16, 0x26, 0x27, 0x2a, 0x2b, 0x2f, 0x38..0x3a | configuration, values below, meaning unknown | [D] high for values |

## BD_Sensor_Init / BD_Sensor_Start [D]

`bd1xxx_init_heartrate.part.8` (`0x56b9e0`) after power on, `msleep(10)` and clearing the 400-byte buffer
(`+0x34..+0x48`):

| Mode (data +952) | `BD_Sensor_Init(mode, a, b)` a, b | `BD_Sensor_Start(mode, c, d)` c, d | Timer |
|---|---|---|---|
| 2 | `{30720,104,0}`, `{24576,88,0}` | `{32,30000,6000}`, same | 50 ms |
| 15 | `{89,16,0}`, `{89,56,0}` | `{110,2000,5}`, `{110,15000,5}` | 200 ms |
| all others (0, 1, 3..14, > 15) | as 15 | as 15 | 50 ms |

Each argument is a pointer to three u16 in kernel data (`0xffffff800914ac30..0xac68`); high confidence.
The first pointer is used when the chip revision is not 0x0101, the second when it is ("Id_flag").

**Start writes no register.** It sets `hardware_flag2` (running), `red_setflag`, `ir_setflag` (DC loops
armed), clears the counters, sets the wear state `Product_old_status1` = `Product_test_status1` = 2,
copies three u16 of c or d (d iff Id_flag) into `wear_ref1..3` (`+0x5c..+0xd4`), and re-reads 0xbc/0xbd
for the finger-math flag. High confidence.

**Init**, in order: read 0xbc, 0xbd, 0xbe, 0xbf (`+0x3c..`; `Proudct_Id_Data` = BC<<24 | BD<<16 | BE<<8 | BF).
If BC != 0x11 or BD != 0x70 return before any write (`+0x19c`). Revision 0x0101 (BE == 1 and BF == 1) sets
`Id_flag` = 1, `green_current` = 0x51, red and IR DC start values (`red_dc_value1`, `ir_dc_value1`) = 3;
otherwise 0, 0x59, 0x0a. `red_current` / `ir_current` = byte 1 / byte 2 of the first (Id_flag 0) or second
(Id_flag 1) u16 triple passed as a, b: mode 2: 0x78 / 0x68 or 0x60 / 0x58; others: 0x00 / 0x10 or 0x00 / 0x38.
Then, exactly (reg=value), no delays, write errors ignored by stock:

- Mode 2 (`+0x2dc`): 01=08 03=14 04=00 05=A0 06=21 07=78 08=08 09=29 0A=B1 0B=00 26=8F 27=81 2A=20 2B=00
  0E=red_current 0F=ir_current 16=63 2F=33 32=00 33=dc_red 34=dc_ir (3 or 0x0a) T T T 00=4B
- Modes 3, 4 (`+0x990`): 01=01 03=02 04=green_current 05=A4 06=21 07=78 08=08 09=29 0A=B1 0B=00 26=8F
  27=81 2A=20 2B=00 0E=00 0F=ir_current 16=A0 2F=33 32=00 33=00 34=00 T T T 00=4B
- All other modes incl. 1, 15 (`+0x102c`): as modes 3, 4 with 03=14.
- T T T = reg 0x38, 0x39, 0x3a each 0x00 when Id_flag = 1, else 0x03 (written right after 0x34).

Note the stock quirk: the globals `red_dc_value1` / `ir_dc_value1` are set to 3 / 0x0a in every mode while
regs 0x33 / 0x34 are written 0 outside mode 2; the DC loop and the trailer use the globals. Confidence high
(straight-line code, every value read from `mov` immediates and re-checked against the call order).

## BD_Sensor_Data_Read state machine [D]

Called by the work function with `(mode, arr0 = buf+0, arr1 = buf+70, arr2 = buf+140)` (`buf` = data +0x98,
zeroed at start). Returns 1 when a frame was completed, 2 on an I2C failure, 0 otherwise. Does nothing
(returns 0) unless `hardware_flag2` is set. Three paths: mode 2 (`+0x64`), modes 3, 4 (`+0xb8c`), the rest
(`+0x1704`). High confidence unless noted.

Per call, every path first reads, in this order: 0x1a, 0x1b, 0x18, 0x19, 0x1c, 0x1d, 0x1e, 0x1f (little endian
words g, amb, r, ir). `amb` is stored at once.

1. **DC loop**, only while its pending flag is set (initially set by Start):
   - value > 0xb3b0 and DC != 0xff: DC += 2 if value > 0xffdc and DC <= 0xfc, else 1; write DC to the
     register; flag stays set.
   - otherwise: if value <= 0x2327 and DC != 0: DC -= 2 if value <= 9 and DC > 2, else 1, and write it;
     then clear the flag.
   - Mode 2 (`+0x360`): red flag with r -> reg 0x33, then IR flag with ir -> reg 0x34. Modes 3, 4 (`+0xe84`)
     and others (`+0x1a00`): green (`red_setflag`) flag with g -> reg 0x32.
2. **Store**: modes 2 and the rest: arr0[n] = g, arr1[n] = r, arr2[n] = ir, `data_count` n += 1 if n < 30
   (`+0x758`, `+0x1c04`). Modes 3, 4 (`+0x13e4`): loop { read reg 0x30; stop if <= 2; read words 0x20, 0x22,
   0x24 (low byte first); store those as arr0/1/2[n] } (stock has no bound; the driver stops at 32 iterations).
   The vendor loop has a fixed iteration count of one for the sum of the three words (no averaging).
3. **Frame end** when `data_count` >= 20 (`+0x8e4`, `+0x1500`, `+0x1d90`): with n = `data_count`
   - arr0[34] = n; `DC_refless_data` f08 / f0c: mode 2 arr1[n-1] / arr0[n-1]; others arr2[n-1] / arr1[n-1];
   - re-arm the pending flags: mode 2: arr1[n-1] outside 0x1b58..0xc350 re-arms the red flag, arr2[n-1] the IR
     flag; others: arr0[n-1] re-arms the green flag;
   - arr[0..2][n..29] = 0; trailer arr0: [29] green DC, [30] f08, [31] f0c, [32] red DC, [33] IR DC;
     `data_count` = 0, return 1 (2 instead if any I2C transfer of this call failed; no copy, no event).
4. **Tail** (return 1 only, `+0x20a8`): average of arr0 and a call to `Wear_SPO2_judge` (mode 2),
   `Wear_Hrv_judge` (3, 4) or `Wear_judge` (others); then trailer arr0: [28] = wear state (`Product_old_status1`),
   [27] = amb; arr1: [29] = chip id low 16, [30] = chip id high 16, [31..33] = `wear_ref1..3`, [34] = n;
   arr2: [31] green_current, [32] red_current, [33] ir_current, [34] = n.
5. **Wear state** (`Wear_judge*`, accelerometer driven, no I2C) toggles `Product_old_status1` between 2 and 1.
   When it differs from `Product_test_status1` the read path writes reg 0x04 = 1 (state 1) or `green_current`
   (modes other than 2, `+0x7f0`/`+0x1ca0` area), or reg 0x0e = 0 / `red_current` (mode 2). Meaning of state 1
   [I] (probably "not worn": LEDs dimmed). The driver never leaves state 2, so these writes never happen.

Samples are raw 16-bit ADC counts (unit: counts, no scaling) [D]. One frame is 20 consecutive samples per
channel, polled every 50 ms, so a frame (and the input event) arrives about once per second (modes 3, 4:
whenever 20 or more FIFO samples have accumulated, up to 30).

### Frame / buffer layout [D][H]

400-byte buffer = three arrays of 35 little-endian u16 at byte offsets 0, 70, 140; 210 bytes used, the rest
zero. Index 0..29: samples (only 0..n-1 valid), 27..34: trailer as above. The work function copies all 400
bytes to the read buffer at a completed frame; `read(fd, buf, 200)` returns the first 200 bytes, i.e. arrays 0
and 1 whole and samples 0..29 of array 2 (its trailer words 30..34 are cut off) [D].

HAL side [H, high]: `readEvents` zeroes a 400-byte local, `__read_chk(fd, buf, 200, 400)`, then calls
`BD_health_pro(mode, accel_count, accel_buf, buf, buf + 70, buf + 140, out)`; it logs 35 words per array
(`i`, `i + 35`, `i + 70`). The 200-byte accelerometer buffer is built by the HAL's own `setAccel`
(`|x| + |y| + |z|`), unrelated to the kernel buffer. `BD_health_pro` reads the trailer words of array 0 at
byte offsets 0x36, 0x38, 0x3a, 0x3c, 0x40, 0x42 (amb, wear state, green DC, f08, red DC, IR DC) and the
count at 0x44, and selects SpO2 (mode 2), HRV (3, 4) or HR (others) like the kernel [H, high].

## BD_finger_math (mode 15) [D]

In mode 15 the work function calls `BD_finger_math(0, local, g_data, g_data + 200, g_data + 400, ...)`
instead of `BD_Sensor_Data_Read`. It does **no I2C**: `g_data` is the SC7A22 accelerometer driver data
(three arrays of 100 u16 at +0, +200, +400 and a new-sample count byte at +600, cleared by the work
function), run through `gensor_{x,y,z}_lpf` and a rolling max/delta analysis. It returns 2 on a detected
motion pattern (medium confidence for the exact condition; it also requires the ID flag set by Start). The
work function then sends EV_KEY 63 = 1, SYN, EV_KEY 63 = 0, SYN. The PPG chip is still programmed
(Init block of the "other" modes with the mode 15 parameters, 200 ms timer) but never read in this mode.
Not implemented: it needs the accelerometer buffer.

## Input device [D][H]

- Name `heartrate` (string at +0), `id.bustype = 0x18` (BUS_HOST), `evbit = 6` (EV_KEY, EV_REL),
  `keybit` bit 63 (KEY code 63), `relbit = 7` (REL_X, REL_Y, REL_Z). No parent device;
  registers under `/devices/virtual/input/inputN`.
- Events from the work function [D, high] (`0x56b0f0..0x56b148`), only when `BD_Sensor_Data_Read` returned 1:
  EV_REL REL_X = arr0[0] (first sample of channel 0), REL_Y (code 1 = the return value) = arr1[0],
  REL_Z = arr2[0], then SYN. (An earlier version of this document said REL_Y carried the return value;
  it is the event code.) Mode 15: see above.
- The HAL [H] only uses the SYN: `readEvents` ignores EV_REL (type 2), treats SYN (type 0) as "new
  data" (stamps the time, reads 200 bytes from the misc device and calls `BD_health_pro`) and logs
  any other type (EV_KEY) as unknown. So exact REL values do not matter to the HAL. Note that the input
  core drops a SYN-only packet: three zero REL values mean no wake-up for the HAL.
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
- `read(fd, buf, 200)` [H, D]: copies the first `count` bytes (returns `count`, -EFAULT on failure) of the
  400-byte read buffer saved by the work function at `data + 0x228` (200 bytes = 100 u16 little endian,
  layout: see "Frame / buffer layout"; the HAL passes them to `BD_health_pro` as arrays 0, 1 and part of 2).
  Kernel also `printk`s the first 40 u16 words. The HAL calls read once per SYN event.

## sysfs attributes (on the input device) [D]

| Attribute | Mode | Format |
|---|---|---|
| `enable` | 0664 (0x1a4) | store: decimal `kstrtol`-style (`kstrtol`-like helper, base 10); 0 stops, nonzero starts using the stored mode. show: `%d\n` of a state field that the stock driver never updates (always 0) |
| `chipinfo` | 0664 (0x1a4, show only so not writable in practice) | `bd1820` (or `bd1662`), 6 bytes, **no newline**; selected by the chip model decided at probe |

The HAL reads `chipinfo` from `/sys/class/input/inputN/chipinfo` and picks its sensor name from it [H];
it does **not** use the sysfs `enable` (HAL start/stop goes through the misc device write) [H].
There is no delay/period attribute.

## Start / stop and timing [D]

- Start (`bd1xxx_init_heartrate(1)` / write enable, `0x56b9e0`): enable regulator (if any), power GPIO high,
  `msleep(10)`, zero the 400-byte working buffer, `BD_Sensor_Init` + `BD_Sensor_Start` (argument sets above),
  `hrtimer_start_range_ns(period, 0, REL)`: mode 15 -> 200 ms, others -> 50 ms. High confidence.
- Timer callback (`0x56b170`): `queue_work_on(WORK_CPU_UNBOUND, system_wq)`, re-arms itself relative to now
  (200 ms for mode 15, else 50 ms), returns HRTIMER_NORESTART. High confidence.
- Work function (`0x56afd4`): mode 15 -> `BD_finger_math`; else `BD_Sensor_Data_Read(mode, ...)`, and on
  return 1 copies the 400-byte working buffer to the read buffer and reports the input events (above).
- Stop (`0x56bb64`): `hrtimer_cancel`, `msleep(2)`, `BD_Sensor_Close` (running flag = 0, then reg 0x00 = 0x00,
  0x80, 0x00 in this order, `0x56d238..0x56d2c0`), clear the working buffer, power GPIO low, regulator disable.
  Stock does not cancel the work item and the mode is re-read from data +952 on every poll. High confidence.
- The mode written by the HAL is stored in driver data (+952) on every write, also on stop, and used by the
  next start.
- Interrupts: **none** (no IRQ in DT, no request_irq); data is polled by the timer.

## Suspend / resume [D]

PM ops only log `bd1xxx_i2c_suspend` / `bd1xxx_i2c_resume` and return 0: the sensor is not touched and the
timer keeps running.

## Not recovered / open items

- `BD_finger_math` (mode 15) and the wear-detection state machine (`Wear_judge`, `Wear_Hrv_judge`,
  `Wear_SPO2_judge`, `gensor_*_lpf`) are not reimplemented: they consume the SC7A22 sample buffer
  (`g_data`), which the BD1xxx driver cannot see. Consequences: mode 15 produces no KEY 63 events and no
  samples; the wear state stays 2, so the reg 0x04 / 0x0e switch to the "state 1" values never happens.
  The exact decision logic of the three judge functions, and what state 1 means electrically, were not decoded.
- Meaning of regs 0x01, 0x03, 0x05..0x0b, 0x16, 0x26, 0x27, 0x2a, 0x2b, 0x2f, 0x38..0x3a and of the FIFO
  level in reg 0x30 is unknown; only the stock values and access order are known. Whether reading 0x1a..0x25
  has side effects on the chip (latching, FIFO pop) is unknown, so the driver keeps the stock read order.
- No delay exists between the Init writes in stock; whether the chip needs one is unknown (not measured).
- Behaviour on a real unit is untested: nothing here was run against hardware.

## Implemented by `drivers/input/misc/bd1xxx.c`

Probe (regulator `vdd_name` at 2.8 V, power GPIO, read-only ID check reg 0xBD == 0x70 and reg 0xBC ==
0x11, then power off; the stock write/readback of reg 0x03 is skipped because its meaning is unknown),
input device `heartrate` (EV_KEY 63, EV_REL X/Y/Z), `enable` and `chipinfo` (`bd1820`, no newline),
misc device `/dev/bd1662rate` (write: byte 0 mode, byte 1 enable, up to 20 bytes; read: up to 400 bytes, the
last complete frame), power on/off on enable.

Measurement: Init block per mode and Close exactly as above; hrtimer (50 ms, 200 ms in mode 15) and a work
item that runs the Data_Read state machine of the three paths (DC loops, 20-sample frames, trailer words),
copies the frame to the read buffer and reports REL_X/Y/Z + SYN. A `ctl` mutex serialises enable, disable,
suspend, resume and remove and is held across `cancel_work_sync`; `lock` protects the sampling state, and the
timer and work are cancelled (timer first) before the Close sequence and the power-off in disable, suspend
and remove.

Differences from stock: write returns the byte count, `enable` show always prints 0; suspend stops sampling,
closes and powers the chip off and resume restarts it; the poller uses the mode captured at start; a
failed Init write aborts the start (stock carries on); a failed read drops the sample before touching state
(stock stores garbage and drops the frame); the HRV FIFO drain is bounded to 32 reads; the read buffer is
cleared at start; the wear state is fixed at 2; mode 15 never reports KEY 63; the working buffer is not exposed.
