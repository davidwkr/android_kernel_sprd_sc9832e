# HRS3300 stock measurement code (MT108): what a shim must provide

Evidence: disassembly of the MT108 stock boot image kernel (kallsyms symbols
recovered by `tools/blobcarve`, profile `hrs3300`). Nothing was run on a unit.
The carved object is generated from the device's own image and never committed
(`tools/blobcarve/README.md`). Companion of `HRS3XXX_INTERFACE.md` (HX3918) and
of the HX3918 shim (`drivers/input/misc/hrs3xxx_blob.c`, branch
`drivers/hrs3918-shim`), which this extends.

## What it is

The same stock `hrs3xxx` glue as on DM63/DW100U (probe, `enable`/`chipinfo`
sysfs, input device `heartrate`, hrtimer plus work item) with a different
hardware layer and algorithm: `Hrs3300_*` (chip ID 0x21 at register 0x00, I2C
0x44). The probe in this image dispatches on the ID (0x21, 0x22, 0x25, 0x27)
but only the HRS3300 and BD1xxx code is linked in; there is no `hx3918_*` or
`tyhx_*` code in this image (the image also holds a separate `bd1xxx` driver
with its own functions, not carved).

`rand` (a stub returning 0) and `hr_sqrt` are carved with it and exported
under those names. `hr_sqrt` is also exported by the HX3918 blob: never link
both blobs into one kernel without localising the symbol.

## Carved layout

25 functions in one contiguous range, no gaps, no excluded functions:

| Part | Functions |
|---|---|
| register access | `Hrs3300_write_reg`, `Hrs3300_read_reg` |
| sample readout | `Hrs3300_read_hrs`, `Hrs3300_read_als` |
| chip control | `Hrs3300_chip_init`, `_chip_enable`, `_chip_disable`, `_set_static_mode` |
| per-sample entry | `hrs3300_Int_handle` |
| power/AGC | `Hrs3300_agc`, `_low_power`, `_hlow_power`, `_normal_power` (called only from `_agc` and `_alg_open`) |
| algorithm | `Hrs3300_alg_open`, `_alg_send_data` (18 KB), `_alg_close`, `_alg_get_results`, `_alg_get_hr_internal`, `_alg_get_hrm_iir_data`, `_alg_get_agc_status`, `Hrs3300_set_exinf`, `Hrs3300_bp_age_fn`, `Hrs3300_alg_get_bp_results` |
| helpers | `rand`, `hr_sqrt` |

Data: a 10-entry pointer table, a read-only block (init register table, tuning
bytes, a sine table, the jump table of `Hrs3300_agc`), a block with the printk
format strings, `alg_status`/`agc_status`, and one bss block holding the
`hrs3xxx_t` pointer, the algorithm RAM (4280 bytes) and the result buffers. No SIMD/FP, no atomics, no indirect calls, no stack protector.

## Entry points as recovered (AArch64 ABI)

`struct hrs3xxx_t` is the vendor struct (fields below). The only calls the
stock kernel makes into the blob, from outside it, are the five marked
"glue".

| Signature | Notes |
|---|---|
| `void Hrs3300_chip_init(struct hrs3xxx_t *h)` (glue) | prints `%s start`, **stores `h` in the global `hrs3xxx_t`**, writes the init register table (18 register/value byte pairs) with `hrs3xxx_write_reg`, then reads registers 0x7f..0x82 into globals |
| `void Hrs3300_chip_enable(void)` (glue) | writes registers 0x16, 0x01, 0x0c; no-op if the global is NULL |
| `void Hrs3300_chip_disable(void)` (glue) | writes 0x01, 0x02, 0x0c, 0x16 and a repeated 0x01/0x0c sequence; no-op if the global is NULL |
| `void Hrs3300_alg_open(void)` (glue) | resets the algorithm: points `alg_data` at its RAM, sets `alg_status` to 6, derives parameters from the tuning bytes, calls `_normal_power` |
| `u8 hrs3300_Int_handle(struct hrs3xxx_t *h)` (glue) | one 40 ms tick, see below; returns 1 when new results were written to `h`, 0 otherwise |
| `u16 Hrs3300_read_hrs(struct hrs3xxx_t *h)` | 16 bit PPG sample from registers 0x09, 0x0a, 0x0f via `hrs3xxx_read_reg(h->client, reg)` |
| `u16 Hrs3300_read_als(struct hrs3xxx_t *h)` | 16 bit ambient sample from 0x08, 0x0d, 0x0e, saturating to 0x7fff |
| `u8 Hrs3300_write_reg(u8 reg, u8 val)` | through the global; returns 0 (also on I2C error), 1 if the global is NULL |
| `u8 Hrs3300_read_reg(u8 reg)` | through the global; 0xff if the global is NULL |
| `void Hrs3300_alg_send_data(u16 hrs, u16 als, u16 x, u16 y, u16 z, u16 t)` | six 16 bit arguments; the stock tick passes the two samples and four zeros; calls `Hrs3300_agc` internally; 240 byte stack frame |
| `struct {...} Hrs3300_alg_get_results(void)` | 12 byte struct in x0/x1: u32 `alg_status`, u32 (algorithm RAM +12), then bytes from algorithm RAM +0, +6 and +103 |
| `struct {...} Hrs3300_alg_get_bp_results(void)` | 16 byte struct in x0/x1: bytes at +4 and +5 are algorithm RAM +0x1080 and +0x1081, u32 at +8 is RAM +12, byte at +12 is RAM +0 |
| `void Hrs3300_alg_close(void)`, `u8 Hrs3300_alg_get_hr_internal(void)`, `u32 Hrs3300_alg_get_hrm_iir_data(void)`, `u8 Hrs3300_alg_get_agc_status(void)`, `void Hrs3300_set_exinf(u8 a, u8 b, u8 c, u8 d, u8 e, u8 f)`, `void Hrs3300_set_static_mode(u8)` | never called by the stock kernel glue; `set_exinf` fills a 6 byte external-information buffer that the stock kernel never sets (it stays zero) |

`hrs3300_Int_handle` does, in order: `Hrs3300_read_hrs`, `Hrs3300_read_als`,
`Hrs3300_alg_send_data(hrs, als, 0, 0, 0, 0)`, `Hrs3300_alg_get_results`,
and if its status is 2 or 3 stores 0 in `h->rel_x` and returns 0. Otherwise it
calls `Hrs3300_alg_get_bp_results`, stores three bytes into the struct, prints
`hr_result:%d hr_result:%d sbp:%d dbp:%d` and returns 1. The log text names the
three values: heart rate, systolic and diastolic blood pressure (the blood
pressure values are the algorithm's estimates; their quality is not known).

## Undefined symbols

`printk`, `hrs3xxx_read_reg`, `hrs3xxx_write_reg`. Nothing else: no
`__stack_chk_guard`/`__stack_chk_fail`, no `system_wq`, no delays, no memset,
no timer switches, no `hrs3xxx_i2c_read`.

## Vendor struct fields used

Offsets are stock offsets; the layout is identical to the one the HX3918 shim
already builds (`struct hrsb_vendor`).

| Offset | Use by the blob |
|---|---|
| +0x000 | `client` (pointer): load, passed to the register accessors |
| +0x130 | store: heart rate (REL_X) |
| +0x148 | store: systolic estimate (REL_Y) |
| +0x14c | store: diastolic estimate (REL_Z) |

Everything else in the struct (input device, timers, work, mutex, `enabled`
at +0x100, the other result slots) belongs to the glue. Stock `init_heartrate`
clears +0x130, +0x134, +0x138, +0x13c, +0x148 and +0x14c before starting.

## What the kernel shim must provide

1. `u8 hrs3xxx_read_reg(struct i2c_client *, u8 reg)` and
   `u8 hrs3xxx_write_reg(struct i2c_client *, u8 reg, u8 val)`: identical to
   the HX3918 shim's functions (stock semantics, see there).
2. `printk` (the kernel).
3. The vendor struct with `client` at offset 0 and 32 bit result slots at
   +0x130/+0x148/+0x14c. The HX3918 `struct hrsb_vendor` already satisfies it.
4. Serialise every call into the blob with the existing `hrsb->lock`: global
   state, and the global `hrs3xxx_t` pointer (`hrsblob_hrs3xxx_t`).
5. Sequence, as stock `hrs3xxx_init_heartrate(h, enable)` does it:
   - start (enable > 0; stock treats every positive value alike, the mode is
     ignored): power GPIO high, 100 ms, clear the result slots, then
     `Hrs3300_chip_init(&v)`, `Hrs3300_chip_enable()`, `Hrs3300_alg_open()`,
     set `enabled`, arm the 40 ms timer;
   - tick (work item, `enabled` checked under the lock):
     `hrs3300_Int_handle(&v)`; only if it returns non-zero report;
   - stop: clear `enabled`, cancel timer and work, then under the lock
     (stock waits 320 ms first) `Hrs3300_chip_disable()`, power GPIO low,
     10 ms;
   - on remove set `hrsblob_hrs3xxx_t` to NULL after the last call
     (`chip_disable`/`chip_enable`/register access are then no-ops).
6. Report only `REL_X`, `REL_Y`, `REL_Z`, then `input_sync()`, per tick that
   returned non-zero (stock does exactly this on this path). `REL_RX`,
   `REL_RY`, `REL_RZ`, `REL_HWHEEL` and `REL_MISC` are never sent by the
   stock HRS3300 path (the capability bitmap is the same as for HX3918,
   0x27f).
7. Link the object generated from the MT108 image
   (`--profile hrs3300`); it is image specific.

## Differences from the HX3918 shim

| | HX3918 | HRS3300 |
|---|---|---|
| Entry points | `hx3918_init(h, mode)`, `hx3918_exit(h)`, `heart_rate_meas_timeout_handler(h)`, `agc_timeout_handler(h)` | `Hrs3300_chip_init(h)`, `_chip_enable()`, `_alg_open()`, `hrs3300_Int_handle(h)`, `_chip_disable()` |
| Mode | `enable` 1..3 mapped to init modes 0/1/6 | none: any positive value is the same |
| Timers | two: PPG and AGC, both 40 ms, switched by the blob through `hx3918_ppg_timer_cfg`/`hx3918_agc_timer_cfg` | one 40 ms timer owned entirely by the shim; AGC runs inside `Hrs3300_alg_send_data`, so no `*_timer_cfg` callbacks and no second work item |
| Callbacks into the shim | `hrs3xxx_i2c_read`, `_read_reg`, `_write_reg`, `hx3918_ppg_timer_cfg`, `hx3918_agc_timer_cfg`, `__stack_chk_fail` | `hrs3xxx_read_reg`, `hrs3xxx_write_reg` only |
| Kernel imports | `printk`, `__const_udelay`, `__memset`, `__stack_chk_guard`, `__stack_chk_fail` | `printk` |
| Global struct pointer | shim must set `hrsblob_hrs3xxx_t` before every call | set by `Hrs3300_chip_init`; the shim only clears it at teardown |
| Struct fields used | +0x00, +0x130, +0x134, +0x13c, +0x140, +0x144, +0x148, +0x14c | +0x00, +0x130, +0x148, +0x14c |
| Results | bpm, SpO2, wear state and more, all eight REL codes | heart rate, systolic, diastolic estimate on X, Y, Z; sent only when `Int_handle` returns non-zero |
| Return convention | tick function reports always | tick reports only on a non-zero return |
| Exported blob symbols | `hrsblob_hrs3xxx_t`, `hrsblob_alg_ram`, `hrsblob_spo2_alg_data` | `hrsblob_hrs3xxx_t` |
| Blob size | 110 functions | 25 functions, one 18 KB algorithm function |

To extend the HX3918 shim: put the five calls behind an ops table selected by
the chip ID (0x27 or 0x21), keep `hrsb_vendor`, the lock, the live flag and
the register accessors unchanged, drop the second timer/work for the HRS3300
ops, change `hrsb_report` to the three-code form for it, and add the
`HEARTRATE_HRS3300` Kconfig path pointing at the HRS3300 `hrsblob.S`. A kernel
with both chips would also need the duplicated `hr_sqrt` (and `rand` if
exported) localised in one of the two objects, and it would have two global
vendor pointers (one per blob), so the "one instance only" rule of the shim
stays.

## Open points (no unit)

- Which heart-rate part is populated on a unit (HRS3300 or the `bd1xxx`
  alternative) is unconfirmed.
- Whether the blood pressure estimates are meaningful without the external
  information (`Hrs3300_set_exinf`, never set by the stock kernel) is unknown.
- The stock HAL handling of a tick without REL_RX/RY/RZ events on this path was
  not examined (the MT108 HAL differs from DM63/DW100U according to the SPEC).
