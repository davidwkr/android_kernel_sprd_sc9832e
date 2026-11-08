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
| Reg 0x01 | HX3918 [D]: 0 = on (`hx3918_ppg_on` 0x4d4d8c writes 0, then 5 ms), 1 = standby (`hx3918_ppg_off` 0x4d4d4c writes regs 0x2a, 0x2b, 0x16 = 0, then reg 0x01 = 1). HRS3300: ENABLE, 0 = disabled |
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

## Implemented by `drivers/input/misc/hrs3xxx_core.c`

Probe (power GPIO, ID check for HRS3918 `0x27` and HRS3300 `0x21`, each behind its Kconfig bool `HEARTRATE_HRS3918` / `HEARTRATE_HRS3300`; `chipinfo` reports `hrs3918` or `hrs3300`; power off), input device and capabilities, `enable` and
`chipinfo` attributes with the formats above, suspend/resume (without the blob: sensor powered off while
suspended, mode kept; with it: see below). Without `CONFIG_HRS3XXX_TYHX_BLOB` the chip measurement and the heart-rate/SpO2
algorithms are not implemented (see the next sections for the option); while enabled the driver reports "no valid sample" (0 for bpm/SpO2/wear,
-1 for REL_HWHEEL/REL_MISC) once per second.

HRS3300 (unverified, no unit): only the ID (reg 0x00 = 0x21) is checked and ENABLE (reg 0x01) is written 0 for standby, which is the register and disabled state in the public hrs3300 register description (ENABLE 0x01, PDRIVER 0x0C, data 0x08/0x09/0x0A/0x0D/0x0E/0x0F). The power-up value (typ. 0x68), LED drive and sample readout are not programmed, so the HRS3300 does not measure yet; the stock path (HAL name `hrs3300`) is untested on a unit.

Note: the input core in this tree drops EV_REL events whose value is 0
(`input_get_disposition`), so an all-zero frame reaches userspace as
REL_HWHEEL = -1, REL_MISC = -1, SYN. The stock driver goes through the same core;
the HAL emits its sensor event on SYN, so behaviour is identical.

## Stock glue semantics [D]

[D] = disassembly of the DM63 stock kernel (offsets are from `_text`, `0x`
hex). DW100U's stock hx3918 layer differs and is not covered here.

| Item | Evidence |
|---|---|
| `hrs3xxx_i2c_read(client, reg, buf, len)` | [D] `0x4d420c`: x0 client, w1 reg, x2 buf, w3 len. `i2c_master_send(client, buf, 1)` must return 1, then `i2c_master_recv(client, buf, len)` must return `len` (both compared as `uxtb`). Returns `len` (w0) on success, `0xff` on any mismatch. |
| `hrs3xxx_read_reg(client, reg)` | [D] `0x4d42b0`: calls i2c_read with len 1 into a stack byte primed with `reg`, returns that byte (so a failed read yields the register number). |
| `hrs3xxx_i2c_write` / `hrs3xxx_write_reg(client, reg, val)` | [D] `0x4d4698`, `0x4d476c`: one `i2c_master_send` of `{reg, data...}` (len <= 19), `0` on success, `0xff` on error. |
| Blob-side callers | [D] `hx3918_write_reg` `0x4d47d0`, `hx3918_read_reg` `0x4d4808`, `hx3918_brust_read_reg` `0x4d4838`: load the global `hrs3xxx_t*`, use `*(struct i2c_client **)hrs3xxx_t` (offset +0x00) as the client, return `0xff` / `0` / `1` when the pointer is NULL. |
| `hx3918_ppg_timer_cfg(on)` | [D] `0x4d4870`: `hrtimer_cancel(hrs3xxx_t+0x68)`; if `on`, `hrtimer_start_range_ns(+0x68, 0x2625a00 ns = 40 ms, 0, HRTIMER_MODE_REL)`. |
| `hx3918_agc_timer_cfg(on)` | [D] `0x4d48dc`: same on `hrs3xxx_t+0x10`. |
| Timer callbacks | [D] `hrs3xxx_hearterate_timer_func` `0x4d4164` (timer +0x10): `queue_work_on(WORK_CPU_UNBOUND, system_wq, work +0xc0)`; if the byte at +0x100 is set, restart the timer for 40 ms, return `HRTIMER_NORESTART`. `hrs3xxx_heartrate1_timer_func` `0x4d41b8` (timer +0x68): same for work +0xe0. |
| Work functions | [D] `hrs3xxx_heartrate_work_func` `0x4d4124` (work +0xc0): lock mutex +0x108; if enabled (+0x100) call `agc_timeout_handler(hrs3xxx_t)`. `hrs3xxx_heartrate1_work_func` `0x4d4024` (work +0xe0): same lock; if enabled call `heart_rate_meas_timeout_handler(hrs3xxx_t)` then report. |
| Reporting | [D] `0x4d4024`: `input_event(input_dev at +0x08, EV_REL, code, value)` in this order: REL_X=+0x130, REL_Y=+0x148, REL_Z=+0x14c, REL_RX=+0x134, REL_RY=+0x138, REL_RZ=+0x13c, REL_MISC=+0x144, REL_HWHEEL=+0x140, then `EV_SYN`. The vendor code writes only `+0x130..+0x148` (stores in `hx3918_hrs/spo2_ppg_Int_handle` and `hx3918_ppg_Int_handle`); it never reads them. |
| Enable path | [D] `hrs3xxx_enable_store` `0x4d3f88` -> `hrs3xxx_init_heartrate(obj, val)` `0x4d3e2c`: clear +0x130..+0x14c except +0x140/+0x144, enabled=0, cancel both hrtimers, lock +0x108. `val == 0`: `msleep(320)`, `hx3918_exit`, power GPIO low, `msleep(10)`. `val > 0`: power GPIO high, `msleep(100)`, `hx3918_init(obj, m)` with m = 1 if val == 2, 6 if val == 3, else 0; enabled=1. `val < 0`: ignored. |
| `hx3918_init(obj, mode)` | [D] `0x4d4f14`: stores `obj` in the global pointer, `work_mode_flag = mode`, chip on, mode jump table (modes 0, 1, 6 used). Return value is not uniform (0 = success for modes 0/1, non-zero for mode 6) and stock ignores it. `hx3918_exit` `0x4d4ee0` stops the hrtimers through `hx3918_data_reset`. |
| Stack protector | [D] the vendor code reads `__stack_chk_guard` by value and calls `__stack_chk_fail`. The tree provides both with `CONFIG_CC_STACKPROTECTOR` (dm63: `_STRONG`); the shim defines `__stack_chk_fail` only without it. |

## Using the stock code (`CONFIG_HRS3XXX_TYHX_BLOB`)

The stock hx3918 layer and the tyhx algorithm are stock bytes: they are never
committed. They are carved from the device's own `boot.img` with
`tools/blobcarve` into a relocatable assembly file that the kernel build links
from an absolute path outside the tree.

Generate (DM63; offline):

    python3 -I tools/blobcarve/blobcarve.py --boot-img /path/to/stock/boot.img \
        --out /path/outside/tree/hrsblob-dm63 --cross aarch64-linux-gnu-

Then enable `CONFIG_HRS3XXX_TYHX_BLOB=y` and either set
`CONFIG_HRS3XXX_TYHX_BLOB_S="/path/outside/tree/hrsblob-dm63/hrsblob.S"` or pass
`HRS3XXX_TYHX_BLOB_S=/path/.../hrsblob.S` to make. If the option is set and the
file is missing the build stops with an error. `scripts/watch_build.sh dm63`
does all of this when `HRS_STOCK_BOOT_IMG=/path/to/boot.img` is set.

Shim (`hrs3xxx_blob.c`): provides the three register accessors, the two timer
switches (own hrtimers and works; the vendor code never sees their layout),
and a vendor-layout struct with only the used fields at the offsets above
(`BUILD_BUG_ON` checked). One mutex serialises every call into the blob (its
state is global). `enable` 1..3 maps as stock (2 -> mode 1, 3 -> mode 6, else
0); the 40 ms work calls `heart_rate_meas_timeout_handler`, the second timer's
work `agc_timeout_handler`, and all eight REL codes are reported after each
measurement work as stock does. Teardown order on disable and remove:
timers, then works, then `hx3918_exit`, then the power switch. Suspend and resume
keep the algorithm state (next section). The context starts "not live" and is
made live at the end of probe.

### Suspend/resume and standby (stock) [D]

- Stock `dev_pm_ops` [D]: the `i2c_driver` `.driver.pm` (0x10ca9b8 in the data) points at an
  ops table at 0x87db00 whose suspend/resume/freeze/thaw/poweroff/restore slots all hold
  `hrs3xxx_i2c_suspend` (0x4d3cd0) and `hrs3xxx_i2c_resume` (0x4d3ca8). Both only `printk` and
  return 0. The i2c_driver has no legacy suspend/resume, and the glue calls no early-suspend,
  PM-notifier or fb-notifier registration. So stock keeps the chip powered, the algorithm
  state and the timers' configuration across suspend; the monotonic hrtimers simply do not
  expire while the system sleeps and continue afterwards.
- With the blob, suspend therefore only stops the timers and works (`hrsb_suspend`: run
  flags cleared, both hrtimers cancelled, both works cancelled; works that were already
  queued see "suspended" and do nothing), remembering which timers were on. Resume
  (`hrsb_resume`) re-arms exactly those with the 40 ms period. No `hx3918_exit/init`, no
  standby write and no GPIO change, so HR/SpO2 do not re-converge on wake. If a later device
  fails to suspend, the PM core resumes this one and the same call re-arms; both calls are
  idempotent. There is no -EBUSY path: userspace is frozen before device suspend, so no
  `enable` write can be in progress, and the shim's mutex covers the rest.
- Standby on disable [D]: `hx3918_exit` (0x4d4ee0) -> `hx3918_hrs_disable` (0x4d67dc) ->
  `hx3918_hrs_set_mode(1)` (0x4d64b4, jump-table entry 1 = 0x4d650c) -> `hx3918_ppg_off`
  (0x4d4d4c): the stock exit itself puts the chip in standby (reg 0x01 = 1). The stock
  glue writes nothing else on disable except dropping the power GPIO when there is one.
  `hx3918_init` (0x4d4f14) calls `hx3918_ppg_on` (0x4d4d8c: reg 0x01 = 0, 5 ms) first, so it
  wakes the chip by itself: on DW100U (no power GPIO) nothing else is needed.
- Fix: the driver's `power_off` wrote reg 0x01 = 0, which on the HX3918 is the *on* value.
  It now writes 1 for the HX3918 (chip ID 0x27) and keeps 0 for the HRS3300. This applies
  with and without the blob; on boards with a power GPIO the supply is cut right after.

### One stock image per device

The hx3918 hardware layers and some logic differ between devices (blobcarve verifies
DM63, DW100U and MT107 and derives the function set per image), although the shim
interface is identical. Each device must be built from the object carved from its own stock
image: DM63 from the DM63 `boot.img`, DW100U silver and v13m from the DW100U one,
MT107 from the MT107 image. `scripts/watch_build.sh` accepts `HRS_STOCK_BOOT_IMG` for
`dm63`, `dw100u-silver`, `dw100u-v13m` and `mt107` and refuses other devices. The DW100U
and MT107 offsets above were not re-derived; only DM63 is documented here.
