# MSP891 stock interface (pressure + temperature)

Evidence tags: **[D]** disassembly of the stock K18 kernel Image (function name from kallsyms,
file offset from the function start, both built-in — not a module); **[DT]** stock device tree
(`devices/mt107/stock.dts`); **[HAL]** stock vendor partitions (mt107 `vendor.bin`/`system.bin`,
read-only via `debugfs -R`); **[DS]** public datasheet listing; **[H]** hypothesis, not directly
backed by disassembly. No unit was available; nothing here is hardware-tested.

Images used: the K18 kernel Image (`kernel.Image`, full kallsyms) contains the same built-in
`msp891` driver as the MT107 kernel Image (identical strings/offsets in both; K18's device tree
has no `msp891` node, so the driver is simply dead code there). All addresses below are from the
K18 Image; the symbol table was cross-checked against a known `.text` function (`msp891_i2c_probe`
disassembly matches between the symbolic and raw dumps) and against `t_msp891`'s own `nm` address.
The driver is compiled into the kernel, not a loadable `.ko`; pointer-valued static data
(`i2c_driver`, `miscdevice`, `dev_pm_ops`, `file_operations`, `of_device_id.data`, attribute
tables) reads as zero in the flat Image — this kernel is `CONFIG_RELOCATABLE` and those fields are
patched by the self-relocation table at boot, not present in a flat dump. Only plain data
(strings, the one `i2c_device_id` name, the `of_device_id.compatible` string) and `.text` are
readable directly; the few struct layouts below (field offsets inside `struct msp891_data`) were
reconstructed from the code that dereferences them, not from a data dump.

## Identity and DT [D][DT]

- DT: `msp891_gas@20`, `compatible = "pressure,msp891"`, `reg = <0x20>`, `status = "okay"` — no
  other properties (no `irq-gpio`, no `rst-gpio`, no calibration/threshold properties). This is
  the *only* stock device tree (of mt107/mt108/dm82/dm63/dw100u-silver/k18) that has an `msp891`
  node; the other five have none.
- `i2c_device_id msp891_id[]`: one entry, name `"MSP891"`, driver_data 0.
- `of_device_id msp891_acc_of_match[]`: one entry, `compatible = "pressure,msp891"`, matching the
  DT string exactly; `type` and `name` fields empty.
- I2C slave address: 0x20 (from `reg`), used dynamically from `client->addr` (offset +2 of
  `struct i2c_client`) in every transfer — not hardcoded in the driver.
- No interrupt, no GPIO of any kind. The driver is pure software polling (`msp891_work_func`
  re-arms itself via `queue_delayed_work_on`/`schedule_delayed_work`-style requeue). There is
  nothing resembling the OCH1970 IRQ/wake machinery.

## I2C transfer primitives [D]

- `msp891_i2c_write(reg:u8, val:u8)`: builds one `i2c_msg` (`addr=client->addr, flags=0, len=2,
  buf={reg,val}`), calls `i2c_transfer(client->adapter, &msg, 1)`. On failure (`i2c_transfer`
  returns negative), logs `"transfer failed."` and returns `-EFAULT` (0xfffffffb); on success
  returns 0.
- `msp891_i2c_rxdata(client, buf, len)`: the caller pre-loads `buf[0]` with the register address.
  Builds two `i2c_msg`s against the same `buf`: msg0 = `{addr, flags=0, len=1, buf}` (write the
  register byte), msg1 = `{addr, flags=I2C_M_RD(1), len, buf}` (read `len` bytes back into the
  same buffer, overwriting the register byte and beyond). `i2c_transfer(adapter, msgs, 2)`. Same
  error path/return convention as write.
- Both check `i2c_check_functionality(adapter, I2C_FUNC_I2C)` is implied by the probe's own check
  (below); the driver always uses raw `i2c_transfer`, never SMBus calls.

## Probe sequence (`msp891_i2c_probe`) [D]

1. `client->adapter->algo->functionality(adapter)` bit 0 (`I2C_FUNC_I2C`) must be set, else log
   `"i2c function not support!"` and return `-ENODEV`.
2. `kzalloc(200)` for `struct msp891_data` (zero-initialized — all fields default 0/NULL unless
   set explicitly below). `data->client = client` (struct offset 0). Global `t_msp891 = data`
   (a single static pointer — the driver supports exactly one instance).
3. **Chip ID check**, two single-byte reads via `msp891_i2c_rxdata(client, buf, 1)`:
   - read register **0x00** → `chip_id0`; must have **bit 5 (0x20) set**.
   - read register **0x01** → `chip_id1`; must equal exactly **0x03**.
   - Both logged: `"chip_id0: 0x%x chip_id1: 0x%x"`.
   - **If either check fails**: logs `"check chip id failed"`, `kfree(data)`, **and returns 0**
     (not an error) — i2c core will treat the probe as having *succeeded* with nothing set up.
     `t_msp891` is left pointing at freed memory. This is a stock bug, reproduced only as a
     documented hazard, not meant to be copied into the new driver.
4. On success, calls the register init helper (`msp891_init`, a different, file-local function of
   the same source name — not the module `__init`):
   - write reg **0x06 = 0x80**, `msleep(10)`
   - write reg **0x7f = 0x67**
   - write reg **0x56 = 0x00**
   - write reg **0x07 = 0x08**
   - write reg **0x08 = 0x00**
   - write reg **0x09 = 0x00**
   - write reg **0x06 = 0x13**
   (register meanings are not known beyond what the datasheet listing below implies; reproduced
   as a literal sequence — [H] 0x06 looks like a reset/mode register given the two different
   values written to it, bracketing the rest of the sequence).
5. Allocates **two** separate `input_dev`s via `input_allocate_device()`:
   - name **"pressure"**
   - name **"ambient_temperature"**
   Both: `bus = BUS_I2C` (0x18, at the `id.bustype` field offset), `evbit = EV_REL` only (no
   EV_KEY, no EV_ABS), `relbit = REL_MISC` (bit 9, 0x200) only — no REL_X/Y/Z. Both registered
   with `input_register_device()`; `data` pointer stashed as driver data on each (offset +744,
   not otherwise used by the ported driver). On failure of either `input_allocate_device` or
   `input_register_device`, unwinds and logs `"can't allocate device!"` / `"can't register
   device!"`, returns `-ENOMEM`/the register error.
   - `data->pres_idev` stored at struct offset **+8**, `data->temp_idev` at offset **+16**
     (confirmed by `msp891_i2c_remove` and sysfs group add/remove using the same offsets).
6. `data->delay` (offset **+192**) defaults to **15** (ms, decimal 0xf). `data->enable` (offset
   **+196**) defaults to 0 (polling off) — these are the backing fields of the `delay`/`enable`
   sysfs attributes (below). A `delayed_work` is initialized with `msp891_work_func` as its
   function (not queued yet — nothing starts polling until `enable` is written).
7. `misc_register(&msp891_device)` — minor `MISC_DYNAMIC_MINOR` (0xff); name/fops are pointer
   fields (relocated, unreadable from the flat image) but the of/id tables and a bare string
   literal `"msp891"` found in `.rodata` next to the driver's `"MSP891"` log tag are the only
   plausible source for the node name: **[H]** `/dev/msp891`, not directly confirmed. On failure,
   logs `"create register fail!"` but — like the gen-3 chip-id-fail path — the probe still
   continues to run the rest of its setup (sysfs groups) rather than unwinding.
8. `sysfs_create_group()` on `data->pres_idev->dev.kobj` with `msp891_pres_attribute_grout`, and
   on `data->temp_idev->dev.kobj` with `msp891_temp_attribute_grout` (logged `"pres_input create
   group fail!"` / `"temp_input create group fail!"` on error, which unwinds the misc device and
   both input devices, then returns the sysfs error).
9. Logs `"successful"` and returns 0.

**No stock HAL usage found** [HAL]: `sensors.sp9832e.so`, extracted read-only from the mt107
vendor partition (`/vendor/lib64/hw/sensors.sp9832e.so` via `debugfs -R`), has no string reference
to `msp891`, `pressure`, `ambient_temperature`, or `barometer` anywhere — same for the K18 variant
of the same library. No pressure/barometer HAL module is listed in `/vendor/lib64/hw` either. The
stock Android sensor framework does not appear to consume this device at all; nothing in the given
inputs (no ramdisk/ueventd.rc for mt107 was provided, only its kernel Image) shows what, if
anything, opens `/dev/msp891` or the two input nodes. **Open question**, flagged for the driver
author.

## Measurement (`msp891_work_func`, self-requeuing delayed work) [D]

Each cycle:
1. `msleep(15)`.
2. `msp891_i2c_rxdata(client, buf, 3)` with `buf[0]` pre-set to register **0x17** → reads
   registers 0x17/0x18/0x19 (auto-incrementing burst read), little-endian: `raw = buf[0] |
   (buf[1]<<8) | (buf[2]<<16)` (24-bit). Sign-extended as a 24-bit two's-complement value (if
   `raw >= 0x800000`, subtract 0x1000000), then **`pressure = signed_raw >> 6`** (arithmetic
   shift, i.e. divide by 64 rounding toward −∞). On I2C error the read is simply skipped (no
   retry), and a wrong substitute value (0xfffffffb, the error code, shifted along with everything
   else because the code doesn't check before using it) ends up emitted. [H]: 0x17 is presumably
   a pressure-ADC LSB register; not confirmed against a datasheet (see below).
3. `msleep(3)`.
4. `msp891_i2c_rxdata(client, buf, 2)` with `buf[0]` pre-set to register **0x1a** → reads
   registers 0x1a/0x1b, little-endian 16-bit: `traw = buf[0] | (buf[1]<<8)`, then
   **`temperature = traw >> 7`** (arithmetic shift on a 32-bit register that was only ever
   zero-extended from the 16-bit load — **a negative `traw` will not sign-extend correctly before
   the shift**; reproduced as a literal stock quirk, worth fixing rather than copying in the new
   driver).
5. Emits, in order: `input_event(pres_idev, EV_REL, REL_MISC, pressure)`,
   `input_event(pres_idev, EV_SYN, 0, 0)`, `input_event(temp_idev, EV_REL, REL_MISC, temperature)`,
   `input_event(temp_idev, EV_SYN, 0, 0)`. **Both values go out as `REL_MISC` (event code 9)** —
   there is no `ABS_PRESSURE`/`ABS_TEMPERATURE` reporting and no distinguishing event code between
   the two input devices (only the device name distinguishes them).
6. Re-arms itself: `queue_delayed_work_on(cpu=0x40, system_wq, &data->work,
   msecs_to_jiffies(data->delay))`. The literal CPU argument is **0x40 (64)** — larger than any
   plausible CPU count on this SoC; reproduced as observed, likely safe to replace with
   `schedule_delayed_work()` (no CPU pinning) in the new driver. [H] on the intent, not the value.

## sysfs attributes [D]

Two identical attribute pairs, one group per input device (`msp891_pres_attribute_grout` on the
`pressure` device, `msp891_temp_attribute_grout` on `ambient_temperature`; both back the *same*
`data->delay`/`data->enable` fields — there is only one set of state for the whole chip, exposed
twice):

- **`delay`** (store/show, `%d` format via `sprintf`/`kstrtoul`-equivalent `simple_strtoul`):
  - show: prints `data->delay` (ms).
  - store: parses an unsigned int; the printk tag for this path is literally
    `"xgl----#######delay=%d"` (left in verbatim — looks like a leftover vendor debug tag, not a
    documented value). **Only accepted if `(value - 1) <= 0xc6`, i.e. value in [1, 199]**;
    otherwise the store is silently ignored (old value kept). Written straight into
    `data->delay` at offset +192 — does not itself touch the running delayed work (the new
    interval only takes effect from the next self-requeue in step 6 above).
- **`enable`** (store/show, same `%d`/`simple_strtoul` convention, printk tag
  `"xgl----#######enable=%d"`):
  - show: prints `data->enable` (just echoes the last stored value — not read from hardware, and
    not actually used to gate the measurement loop's own logic beyond scheduling, see store).
  - store: parses an unsigned int. **Nonzero** → `queue_delayed_work_on(cpu=0x40, system_wq,
    &data->work, msecs_to_jiffies(data->delay))` (starts/kicks off polling at the current
    `delay`). **Zero** → `cancel_delayed_work_sync(&data->work)` (stops polling). The parsed value
    itself is not stored anywhere as `data->enable` in the disassembly path actually followed —
    **[H]**: a store of `data->enable` may exist on a code path this trace didn't need to resolve;
    treat `enable`'s show value as unreliable/not authoritative.

No `chipinfo`, no threshold, no axis, no calibration-file attributes — unlike OCH1970's/the other
hall-sensor family's sysfs surface, MSP891's is just `delay` + `enable` on each of two input
devices.

## Misc device `/dev/msp891` (hypothesised name, minor dynamic) [D]

- `open`: `nonseekable_open()`, then stashes `i2c_get_clientdata(client)` equivalent (loaded via
  the global `t_msp891 → client → client's embedded `struct device` driver-data, offset +192 of
  `i2c_client`) into `file->private_data`. Logs `"open fail!"` on `nonseekable_open` failure.
- `release`: clears `file->private_data`, returns 0. No reference counting, no fasync.
- `unlocked_ioctl`: **always returns 0** — a complete no-op regardless of the `cmd`/`arg` given.
  No ioctl numbers are defined or checked.
- No `read`/`write`/`poll` fop was found referenced from the probe's `file_operations` table
  (only open/release/ioctl are implemented as named functions); **[H]** the struct may still have
  `.owner` set, unreadable from the flat image as noted above.

## Power management [D]

- `msp891_suspend(dev)` and `msp891_resume(dev)` are **both single-instruction stubs that just
  `return 0`** — no register writes, no `device_init_wakeup`, no `irq_set_irq_wake` (there is no
  IRQ to begin with). If `enable` was left on across a suspend, the polling `delayed_work` simply
  keeps firing on its own schedule; nothing in the driver stops or restarts it around suspend.

## Remove (`msp891_i2c_remove`) [D]

Removes both sysfs groups, `misc_deregister()`s the misc device, `input_unregister_device()` +
`input_free_device()` on both input devices, then `kfree(data)`. **No `cancel_delayed_work_sync()`
is called before the free** — if polling was enabled, the delayed work can still be in flight (or
get requeued one more time by its own self-arm) after the backing `input_dev`s and `data` have
been freed. Reproduced as a documented stock bug; the new driver should cancel the work in
`remove()` before freeing anything.

## Not corroborated / open questions

- **Misc device name**: only indirect evidence (`"msp891"` string in `.rodata`, no direct struct
  read possible). Confirm against a live `/dev` listing or the mt107 ramdisk's `ueventd*.rc`
  (not among the given inputs) before committing to `/dev/msp891` in the new driver.
- **No stock userspace consumer found** in the mt107 vendor/system partitions given (HAL, rc
  files for device-node permissions). It's possible this sensor is dead/reserved hardware on this
  particular watch model, or is read by something not covered by the provided images (e.g. a
  factory-test binary, or `/productinfo`-gated app, neither of which was searched here per the
  restricted-NV/device_backups boundary).
- **Register map semantics** (0x06/0x7f/0x56/0x07/0x08/0x09 meanings, and why 0x17 for pressure
  vs 0x1a for temperature) are reproduced only as literal reg/value pairs from disassembly; a
  public "MSP890/MSP891 Digital Output Pressure Sensor" datasheet listing exists (sekorm.com,
  2021-03-02 entry) [DS] but its register-map page could not be fetched (HTTP error) in this
  session — worth a second attempt before finalizing the register table in the real driver.
- **`enable` store not confirmed to persist `data->enable`** — the disassembly path traced here
  only shows the delayed-work start/stop side effect; treat the show value as possibly stale
  until re-checked.
- **CPU pin `0x40` in `queue_delayed_work_on`**: almost certainly not meaningful on a 4-core SoC;
  recommend `schedule_delayed_work()` in the port rather than reproducing the literal value.

## In-tree driver

`drivers/input/misc/msp891.c` (`CONFIG_SENSORS_MSP891`). DT: `msp891_gas@20` added to
`arch/arm64/boot/dts/sprd/sp9832e-1h10-mt107.dts` on `&i2c2` (stock `i2c@70700000`), with no
properties beyond `compatible`/`reg`, matching the stock node exactly; enabled in
`arch/arm64/configs/watch_414.fragment`.

Userspace-visible interface matches stock exactly: two input devices, `"pressure"` and
`"ambient_temperature"`, both `BUS_I2C`, `EV_REL`/`REL_MISC` only, reporting the same `raw >> 6`
(pressure, 24-bit sign-extended) and `traw >> 7` (temperature) values from the same registers
(0x17/0x18/0x19 and 0x1a/0x1b), and the same `enable`/`delay` sysfs pair (`delay` in ms) on each
input device's sysfs directory. `delay` is clamped to the documented stock range (1..199) instead
of silently ignoring an out-of-range store; functionally equivalent for any real writer.

Deviations (all from documented stock bugs, not from the wire protocol):

- A failed chip id check (reg 0x00 bit 5, reg 0x01 == 0x03) returns `-ENODEV` and leaves nothing
  registered, instead of the stock path that frees its state and returns success with a dangling
  global pointer.
- A failed I2C read inside the poll cycle skips reporting that axis instead of emitting the
  (unchecked) error code as if it were a sample.
- The temperature raw 16-bit value is cast to `s16` (sign-extended) before the arithmetic shift;
  stock shifts the zero-extended load, which is wrong for negative readings.
- The poll work re-arms with `schedule_delayed_work()` instead of `queue_delayed_work_on()` with a
  literal, out-of-range CPU argument (0x40).
- `remove()` calls `cancel_delayed_work_sync()` before tearing down the input devices; stock frees
  everything without cancelling the (possibly still scheduled) work first.
- Suspend stops polling and resume restores the `enable` state that was active before suspend;
  stock's suspend/resume are no-op stubs and let the work keep firing across a suspend.

Not implemented, by design:

- No misc device (`/dev/msp891`): the stock one is `open`/`release`/an always-returns-0 `ioctl`
  with no defined commands, and no HAL, rc file, or other consumer of it was found anywhere in the
  given stock images (see "Not corroborated / open questions" above). It is dead ABI surface with
  nothing to stay compatible with, so it is not reproduced.
