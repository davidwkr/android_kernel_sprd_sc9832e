# AF6837 stock interface

Evidence tags: **[D]** disassembly of the stock K18 kernel Image (`kernel.Image`, full kallsyms
absent a usable decoder — see Method below; addresses given are raw file offsets into the Image,
which equal PC-relative disassembly addresses since the image is an unrelocated, uncompressed
arm64 `Image`); **[DT]** stock device tree (`devices/*/stock.dts`); **[HAL]** stock MT107
`vendor.bin`/`system.bin` userspace; **[H]** hypothesis/uncertain, not directly backed by
disassembly. No unit was available; nothing here is hardware-tested.

## Method

K18's `kernel.Image` and the separately supplied MT107 `mt107_kernel.Image` both contain the
compiled `af6837` driver (and an adjacent `msp891` pressure driver, linked next to it — out of
scope here) even though only MT107 and DM63 instantiate it in their device trees; K18's own DT has
no `af6837` node. A full kallsyms symbol table was not decoded (no ELF, no exported System.map);
instead, the driver's own debug/error format strings (e.g. `"af6837 chip id is 0x%x\n"`,
`"compass"`, `"&af6837->lock"`) were located in `.rodata` by `strings`, and a small script decoded
`adrp`+`add`/`ldr` pairs across the image to find every instruction that computes one of those
string addresses (PC-relative addressing is base-independent, so this works without knowing the
kernel's real virtual load address). That pins `af6837_i2c_probe` at file offset `0x4f41b4`–
`0x4f45c4`, with two small helpers at `0x4f3c48` (`af6837_i2c_rxdata`) and `0x4f3de4`
(`af6837_i2c_txdata`), and a third function at `0x4f3e88` (`af6837_read_xyz`) identified by being
the only call whose failure path prints `"af6837_read_xyz is failed\n"`. Function+offset below
refer to these file offsets. MT107's image has the same strings and the same code shape (not
re-verified instruction-by-instruction); only K18 was disassembled.

## DT properties [DT]

compatible `"compass,af6837"`, `reg = <0x0c>` (i2c address 0x0c), `status = "okay"`. **No other
properties** — no `irq-gpio`, no `rst-gpio`, no axis/threshold properties, nothing the driver
would need to parse via `of_property_read_*`. Present only on MT107 and DM63 (`af6837_mag@0C` in
both `stock.dts`); absent from K18, MT108, DM82, DW100U. Per the task brief, the DM63 node exists
but the chip does not end up bound on the running stock DM63 (not independently re-verified here;
consistent with the i2c-functionality/chip-id check below returning `-ENODEV` if the device simply
doesn't ACK on that board's bus).

[D] confirms the DT reading matches the code: `af6837_i2c_probe` never calls any `of_*` property
accessor, `gpio_request*`, or `request_irq`/`request_threaded_irq` — the whole probe only ever
touches the `i2c_client` it's given and reads/writes the chip over I2C. **The driver is pure
I2C, no GPIO, no interrupt line.**

## Probe sequence [D] (`af6837_i2c_probe`, 0x4f41b4–0x4f45c4)

1. `client->adapter->algo->functionality(client->adapter)` (the `i2c_check_functionality()`
   expansion); bit 0 (`I2C_FUNC_I2C`) must be set or probe fails with `-ENODEV` (`-19`) and logs
   `"af6837 i2c functionality check failed."`.
2. `devm_kzalloc(&client->dev, 0x378 /* 888 */, gfp)` for the private struct ("`af6837`" is the
   driver's own local variable name for it — see below); failure returns `-ENOMEM` (`-12`).
3. `i2c_set_clientdata(client, data)`, `data->client = client` (private-struct offset `0x2b8`/696),
   and a module-global pointer is also set to `data` (a static singleton, dereferenced later by the
   read/poll path instead of using per-instance context — i.e. the stock driver assumes a single
   instance).
4. `mutex_init(&af6837->lock)` — the debug name string literally embedded by the `mutex_init()`
   macro is `"&af6837->lock"`, confirming the private struct has a `lock` mutex and that the
   driver's author named the pointer `af6837`.
5. **Chip ID check**: `af6837_i2c_rxdata(reg=0x00, buf, len=1)`; on I2C failure logs
   `"read reg id failed.(%d)\n"` and fails. On success, logs `"af6837 chip id is 0x%x\n"` with the
   byte read, then compares it to **`0x6c`** [D, `cmp w0, #0x6c` at +0x100 of the probe]; any other
   value → `-ENODEV`.
6. **Init register writes**, each via the 2-byte block write helper (reg,value):
   - write `0x11 = 0x81`, then a software delay of roughly 11 ms (a loop of 11 iterations of what
     disassembles as the compiler's inlined `udelay(1000)` constant-multiply sequence — the same
     literal constant is reused everywhere in this kernel for that idiom, so the "1 ms" reading is
     an inference from the constant, not a directly-read decimal argument).
   - write `0x0c = 0x04`.
   - `af6837_i2c_rxdata(reg=0x01, buf, len=1)` — single byte read whose result is **discarded**
     (not checked, not stored); looks like a clear-on-read status/flag register touched purely for
     its side effect.
   - write `0x0a = 0x09`, then a similar ~36 ms software delay (36 iterations of the same
     ~1 ms unit). Failure of this specific write (or the sequence as a whole — the driver doesn't
     distinguish) is reported as `"af6837 chip init failed\n"`.
7. Self-test: calls `af6837_read_xyz(data)` once (described below); its failure is reported as
   `"af6837_read_xyz is failed\n"`. At this point `data`'s input-device pointer is still NULL, so
   `af6837_read_xyz`'s own input-report branch is skipped — the self-test only exercises the I2C
   read and the all-zero check.
8. `input_allocate_device()` (confirmed by the allocation size, `0x538`/1336 bytes, matching
   `struct input_dev`, and by field offsets lining up with the real struct: offset 0 = `name`,
   offset 24 = `id.bustype`, consistent with `name`/`phys`/`uniq` (3 pointers) then `struct
   input_id`). Sets:
   - `name = "compass"` [D, exact string, stored at offset 0].
   - `id.bustype = 0x18` = `BUS_I2C` [D, stored at offset 24].
   - `evbit |= (1 << EV_ABS)` [D, bit 3 set at offset 40] — **no `EV_SYN`/`EV_KEY`/`EV_REL` bit is
     explicitly set** (`EV_SYN` is implied/auto-set by the input core on registration, as usual).
   - `input_set_abs_params(dev, ABS_RX /* 3 */, -32768, 32767, 0, 0)`
   - `input_set_abs_params(dev, ABS_RY /* 4 */, -32768, 32767, 0, 0)`
   - `input_set_abs_params(dev, ABS_RZ /* 5 */, -32768, 32767, 0, 0)`
   - `input_set_abs_params(dev, ABS_MISC /* 40 */, INT_MIN, INT_MAX, 0, 0)` — a 4th axis, full
     32-bit range; purpose not identified (not written by `af6837_read_xyz`; written from
     somewhere else not traced here) [H].
   - three more calls to a function taking `(dev, 2, 0)`-shaped arguments right after, not
     resolved — possibly `input_set_capability()`/property bits; not pinned down [H].
   - on failure, logs `"error registering input device\n"`.
9. Stores a function pointer to a small wrapper (file offset `0x4f4020`) into the private struct;
   that wrapper calls `af6837_read_xyz` and logs a differently-worded failure
   (`"af6837_read_xyz failed\n"`, no "is") plus what looks like a rate-limited/throttled counter
   read-and-log afterwards (not resolved in detail). Initializes some kind of per-struct object at
   offset `0x2e8` via a helper that has the shape of kernel lockdep/debug-object instrumentation
   (seen at the top of many `INIT_WORK`/`timer_setup`-style helpers) — **the exact dispatch
   mechanism (timer vs. workqueue vs. delayed-work) was not conclusively identified**; given there
   is no IRQ anywhere in this driver, **some software-polling mechanism driving the wrapper/
   `af6837_read_xyz` periodically is almost certainly present**, this is the open item to resolve
   before writing the 4.14 driver (e.g. by testing both a kernel timer and a delayed workqueue
   against the exact probe code, or just designing fresh since no DT period is given anyway).
10. `sysfs_create_group()` is called against what resolves to `input_dev->dev.kobj` (offset
    `0x258` into the input_dev, matching `struct device` embedded inside `struct input_dev`, same
    kobj-on-input-device pattern as the `och1970` driver — see `OCH1970_INTERFACE.md`). The
    attribute names/count were not identified (the group pointer resolves into a small `.rodata`
    struct with no readable strings directly at that address; the actual `struct attribute *`
    array wasn't walked). Failure logs `"Unable to creat sysfs group\n"` (stock typo, kept
    verbatim as a fingerprint of this exact source, not meant to be reproduced).
11. On full success, a per-struct byte field is set to decimal `20` and logs
    `"af6837 successfully probed\n"`.

A second uppercase literal, `"AF6837"`, sits in `.rodata` right next to the MSP891 driver's own
strings; no code reference to it was found in the first ~11 MB of the image it would plausibly be
called from. By analogy with `och1970`'s `chipinfo` sysfs attribute (which prints the lowercase
driver name), this could be a `chipinfo`-style sysfs value for `af6837`, but this is **not
confirmed** [H].

## `af6837_read_xyz` (0x4f3e88) [D]

- `af6837_i2c_rxdata(reg=0x03, buf, len=6)` — a single 6-byte block read starting at **register
  0x03** (i.e. registers 0x03–0x08).
- The 6 bytes are unpacked as **three little-endian 16-bit words**, `buf[0] | (buf[1]<<8)`,
  `buf[2] | (buf[3]<<8)`, `buf[4] | (buf[5]<<8)`, in that order (assumed X, Y, Z — nothing in the
  driver names them; order is purely positional). Each word is then explicitly sign-extended in
  software (`if (v > 0x7fff) v -= 0x10000`), i.e. the registers hold plain two's-complement s16
  values and the "manual sign-extend" is just how this compiler emitted it, not evidence of an
  unusual encoding.
- If all three samples are exactly zero, the function treats this as an error and returns `-1`
  (no message logged for this case — it's a silent `return -1`, not a `dev_err`).
- Otherwise, if `data->input_dev` is non-NULL (i.e. not during the probe self-test), it reports:
  `input_event(dev, EV_ABS, ABS_RX, x)`, `input_event(dev, EV_ABS, ABS_RY, y)`,
  `input_event(dev, EV_ABS, ABS_RZ, z)`, then `input_event(dev, EV_SYN, SYN_REPORT, 0)` — four
  calls to what resolves to the standard `input_event()` signature `(dev, type, code, value)`.
  **No raw/calibrated distinction, no timestamp field, no `ABS_MISC` sample here** despite
  `ABS_MISC` being configured as a capability in probe.
- After the read (success or failure of the report step aside), it writes **register 0x0a = 0x01**
  via `af6837_i2c_txdata`; this looks like a per-sample re-arm/trigger write (the same register
  0x0a was written `0x09` once during init). Failure of this specific write returns an I2C error
  code and is logged via a second, separate format string (not fully resolved, a sibling of
  `"af6837_read_xyz failed\n"`/`"af6837_read_xyz is failed\n"`).

## Register map summary [D]

| Reg | Access | Value / meaning |
|---|---|---|
| 0x00 | read 1 byte | chip ID; must equal `0x6c` |
| 0x01 | read 1 byte | read once during init, result discarded (status/clear?) [H] |
| 0x03–0x08 | read 6 bytes (block) | X/Y/Z samples, little-endian s16 each, in that register order |
| 0x0a | write 1 byte | `0x09` during init; `0x01` after every successful sample read (re-arm/trigger) [H on exact semantics] |
| 0x0c | write 1 byte | `0x04` during init |
| 0x11 | write 1 byte | `0x81` during init |

All reads/writes go through two tiny i2c_transfer-based helpers: a register-address-then-read
transaction for `af6837_i2c_rxdata(reg, buf, len)` (write 1 byte = reg, then read `len` bytes into
the same buffer), and a combined `{reg, value}` 2-byte write for `af6837_i2c_txdata(reg, value)`
(single 1-byte value only — there is no multi-byte register write path in this driver at all,
unlike `och1970`'s 4-byte threshold writes).

## What's not in this driver, compared to `och1970`

No GPIO, no IRQ, no wake source/wakelock *source* (there is a mutex, not a wakelock, despite the
`wake_lock`-style naming convention of the debug string), no DT-tunable axis/threshold/key
properties, no misc device (`/dev/af6837` was not found referenced anywhere), and — as far as the
code goes — no self-identified "generations" the way `och1970` has three: this looks like a single,
simple, always-the-same-sequence driver. Whatever samples at ~regular intervals must therefore be
a timer or workqueue the stock driver sets up itself (step 9 above), not something configured from
the device tree — there is nothing in the DT to configure it with.

## Userspace [HAL]

Checked read-only, MT107 `vendor.bin`/`system.bin`: `sensors.sp9832e.so` (the sensors HAL) has no
`af6837`, `compass`, or `magnetic` string anywhere — it implements only an accelerometer,
heart-rate, light, and encoder sensor; **no magnetic-field Android sensor is exposed on this
build**. No `/usr/keylayout/*.kl` or `/usr/idc/*.idc` file matches `af6837`/`compass` (expected,
since the device is pure `EV_ABS`, not `EV_KEY`, so no `.kl` is needed — but also no `.idc` to
calibrate the abs axes, meaning whatever opens `/dev/input/eventN` for this device, if anything,
uses the kernel defaults with no userspace calibration). No `ueventd.rc`/`init*.rc` entry names
`af6837` or `compass`. As with `och1970`, this strongly suggests the chip is present on the
hardware but has no active Android consumer in this stock image — consistent with the task's note
that it's unbound on the running stock DM63.

## Open items for the driver-writing phase

- The periodic-sampling mechanism (timer vs. delayed work, and its actual period) was not pinned
  down; since there's no DT property to drive it anyway, the 4.14 driver is free to pick a
  reasonable interval (e.g. a `delayed_work` on `/sys/class/input/inputN/enable`-less always-on
  polling, or gate it behind standard input `open()`/`close()`), rather than reproduce the exact
  stock timing.
- Exact sysfs attribute names/count under the input device's `kobj` were not recovered (group
  pointer found, contents not walked).
- Chip ID register formatting prints a single byte (`0x%x` of `buf[0]`, matching `len=1`), but the
  datasheet/register-map name for 0x00, and whether any other chip-ID-adjacent register (e.g. a
  WHO_AM_I-style vendor code at a neighboring address) exists, was not checked against a public
  AF6837 datasheet — none was consulted for this document; everything above comes only from the
  stock binary and device trees.
- Registers 0x0a/0x0c/0x11 are given only as raw numbers with no datasheet-backed names (e.g.
  "mode", "control") — purely from the init-sequence disassembly.
- The `ABS_MISC` capability is configured but never written in the only code path traced
  (`af6837_read_xyz`); if it matters, there is probably another function (self-test/calibration
  ioctl-equivalent via sysfs?) that uses it, not found in this pass.

## In-tree driver

`drivers/input/misc/af6837.c` (`CONFIG_SENSORS_COMPASS_AF6837`).

Backported from the 4.14 tree unchanged. DT: the stock `af6837_mag@0C` nodes of the 4.4 MT107 and
DM63 DTs (`compass,af6837`, `reg = <0x0c>`, `&i2c2`) bind as they are; no other 4.4 DT has one.
Enabled through `arch/arm64/configs/watch_sensors_misc.fragment`.

Reproduces: chip id check (reg `0x00` == `0x6c`), the init sequence (`0x11=0x81` + ~11 ms,
`0x0c=0x04`, discarded read of `0x01`, `0x0a=0x09` + ~36 ms, using `msleep()` for the two
delays), the probe-time self-test read, the `0x03`-based 6-byte little-endian XYZ read and the
input device identity: name `"compass"`, `BUS_I2C`, `EV_ABS` `ABS_RX`/`ABS_RY`/`ABS_RZ`
(`-32768`..`32767`) and `ABS_MISC` (`INT_MIN`..`INT_MAX`, configured but never written, same as
stock) — so existing userspace that reads this device sees the same shape.

Differs from stock, and why:

- **Sampling mechanism**: stock's timer/workqueue and its period were not recoverable from the
  disassembly (see "Open items" above), and there is no DT property to configure one either way.
  This port uses a `delayed_work`, gated by a sysfs `enable` (0/1) attribute on the input device
  (polling is off until enabled — a deliberate behaviour change from whatever stock does at
  probe, since stock's self-arming, if any, was never pinned down) and a sysfs `delay`
  (milliseconds, clamped to 20–2000 ms) attribute for the period. Both are plain driver-local
  state, not reconstructions of a stock mechanism.
- **Register `0x0a` re-arm on an all-zero sample**: the stock read function appears to return
  before writing `0x0a=0x01` when all three axes are exactly zero (it treats that as an error
  and returns early), which would leave the chip never re-armed until some other event retries
  it — a plausible stock bug. This driver always issues the `0x0a=0x01` write after a successful
  transfer, zero sample or not, and only skips the input report for that sample.
- **No GPIO/IRQ/wakeup code**: matches stock, which has none of these either (see "What's not in
  this driver" above) — not a deviation, just confirmed by the port having nothing to add here.
- Sysfs attribute names/count under stock's `kobj` group were not recovered from the binary (see
  "Open items"); `enable`/`delay` are therefore this port's own choice, following the shape
  already used by `och1970`/`sc7a22` in this tree, not a reproduction of stock's actual names.
