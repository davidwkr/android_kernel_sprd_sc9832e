# HRS3XXX heart-rate sensor: stock kernel/HAL interface

Applies to the DW100U (i2c-2) and DM63 (i2c-1) watches. Both use DT node
`hrs3xxx@44`, compatible `tyhx,hrs3xxx`, I2C address 0x44. Sources of evidence:

- **[W]** read-only inspection of the running watches (`/proc/bus/input/devices`,
  `getevent -p`, `ls -l`/`cat` on sysfs).
- **[H]** disassembly of the vendor sensors HAL `/vendor/lib64/hw/sensors.sp9832e.so`
  (class `HeartrateSensor`).
- **[K]** `strings` and disassembly of the stock `boot.img` kernel (driver `hrs3xxx`).

## Chip

The "hrs3918" is not a register-compatible HRS3300. The stock driver bundles
vendor code for the HX3918 (and HX3690L) PPG front end: FIFO readout, AGC,
wear detection, and heart-rate and SpO2 algorithms (`hx3918_*`, `SPO2_*`
symbols/strings) **inside the kernel** [K]. Those algorithms are closed vendor
code and are not reimplemented by this driver.

Register facts seen in the stock driver [K], used by this driver:

| Item | Value |
|---|---|
| Transfer style | write 1-byte register address (STOP), then read (two transfers) |
| Reg 0x00 | chip ID: 0x21 HRS3300, 0x22 HRS3602, 0x25 HRS3605, 0x27 HRS3918 |
| Reg 0x01 | written 0 (then 5 ms) to put the chip in standby |
| Probe delay | 10 ms after power-up before reading the ID |
| Enable delay | 100 ms after raising the power GPIO |

Other registers seen (0x40 FIFO level, 0x41 FIFO data, 0x16 to 0x6D, 0xF0 to 0xFF
configuration) belong to the measurement back end and are not used.

## Device tree [K][stock.dts]

- `compatible = "tyhx,hrs3xxx"`, `reg = <0x44>`.
- `power-gpio = <&gpio 0x82 0>` (DM63 only; read with `of_get_named_gpio`, requested
  as "hrs3xxx-power-gpio", driven high to power, low to power off).
- DW100U has no `power-gpio`: supply is always on.
- No IRQ, regulator or other vendor property is read by the stock driver.

## Input device [W][K]

- Name `heartrate`, bus `0x18` (BUS_HOST), no parent: sysfs path
  `/devices/virtual/input/inputN`. Input number varies (input0 on DW100U, input2 on DM63).
- Capabilities: `EV=5` (EV_SYN, EV_REL), REL bitmap `0x27f`: REL_X, REL_Y, REL_Z,
  REL_RX, REL_RY, REL_RZ, REL_HWHEEL (6), REL_MISC (9) [W: `getevent -p`, `capabilities/rel`].
- Events (stock work function, [K]): all eight REL codes then EV_SYN, per update.
- HAL consumption [H]: `HeartrateSensor::readEvents` collects REL values and emits one
  sensor event on EV_SYN, `data[]` = X, RX, Y, Z, RY, HWHEEL, MISC, RZ. Its log line
  is `bpm: <REL_X>  spo2: <REL_RX>  wear: <REL_RZ>`. For REL_HWHEEL and REL_MISC the
  value -1 is mapped to 0. Other event types/codes are logged as unknown.
- HAL requests `EVIOCSCLOCKID` = CLOCK_BOOTTIME (7) on the event fd [H]; no driver work needed.
- The HAL finds the node by scanning `/sys/class/input/input*/name` for `heartrate`.

## sysfs attributes (on the input device) [W][K][H]

Located at `/sys/class/input/inputN/`:

| Attribute | Mode on stock [W] | Format |
|---|---|---|
| `enable` | `rw-rw----` (owner/group set by Android init: system:input) | write decimal text `0`..`3` |
| `chipinfo` | `r--r--r--` | text `hrs3918`, **no trailing newline** [W: `cat` output] |

- `enable` write [H `setEnable`]: the HAL writes `"%d\n"`. Disable writes `0`. Enable writes 1
  unless system property `heartrate.spo2` is `true`, in which case the value comes from
  `persist.sys.heartrate_test_mode` (valid 1..3, otherwise 1).
- `enable` handling [K]: value parsed with base 10; 0 = disable and power off; 1 = default
  heart-rate mode, 2 and 3 select other stock measurement modes; stock accepts any positive
  value (unknown values behave like 1). Re-enabling restarts the sensor. Stock `show` prints a
  state variable that reads 0 (observed 0 while disabled [W]).
- `chipinfo` [H]: the HAL reads this file and `strcmp`s it against `hrs3918`, `hrs3605`,
  `hrs3602`, `hrs3300` to build the sensor name (`hrs3918 HeartrateSensor`, vendor
  `TYHX Sensortec`). A trailing newline would probably break the match (not confirmed), so none is emitted.
- `setDelay` in the HAL is a no-op (logs only) [H]: there is **no delay/period attribute**.
- No misc device, ioctl or `/dev` node with `hrs`/`heart` in the name exists [W]; the HAL
  uses only `/dev/input/eventN` and the two sysfs files.

## Stock driver log messages [K]

Prefix `[HRS3XXX-heartrate]`, e.g. `The chpi ID is 0x%x`, `hrs3xxx check chip is fail`,
`enable:%d`, `hr_result:%d spo2_result:%d`. Stock reports to the input device from a work item
driven by a 40 ms timer plus the chip FIFO.

## Implemented by `drivers/input/misc/hrs3xxx.c`

Probe (power GPIO, ID check, power off), input device and capabilities, `enable` and
`chipinfo` attributes with the formats above, suspend/resume (sensor powered off while
suspended, mode kept). Not implemented: chip measurement and the heart-rate/SpO2
algorithms; while enabled the driver reports "no valid sample" (0 for bpm/SpO2/wear,
-1 for REL_HWHEEL/REL_MISC) once per second.

Note: the input core in this tree drops EV_REL events whose value is 0
(`input_get_disposition`), so an all-zero frame reaches userspace as
REL_HWHEEL = -1, REL_MISC = -1, SYN. The stock driver goes through the same core;
the HAL emits its sensor event on SYN, so behaviour is identical.
