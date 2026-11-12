# SC7A22 hand-raise interrupt storm (DM63 bench boot)

Evidence tags: **[B]** read-only bench captures of the DM63 boot (`/proc/interrupts` taken 2 s
apart, dmesg, logcat, monitor log); **[D]** disassembly of the stock images' `Image`
(`aarch64-linux-gnu-objdump -D -b binary --adjust-vma=0xffffff8008080000`); **[T]** stock DT
(`dtc -I dtb`); **[S]** our source. Nothing here was run on a device by us.

## Symptom [B]

Over 2 s `sc7a22-hand-up-irq` (irq 54, `irq-ap-gpio`, "Level") fired 3759 times (~1900/s), the
I2C controller `70700000.i2c` 7518 times (two transfers per interrupt) and IPI0 15120 times.
CPU 75 C, load ~10, all cores at 1.4 GHz. Earlier boots: busy `irq/7-70700000` thread.

## Cause

Ours requested the line as a fixed `IRQF_TRIGGER_HIGH` and re-enabled it after reading the flag
register (`irq_work`: `disable_irq_nosync` in the handler, `enable_irq` at the end of the work).
When the chip keeps INT high after that read, the line is still asserted at `enable_irq`, the
level interrupt fires again at once, and the loop runs forever (each pass: bank/flag reads over
I2C). Open item 7 of `SC7A22_INTERFACE.md` predicted this.

### Stock DM63 (revision A) [D]

- Request (`sc7a22_i2c_probe`, call at `0xffffff800856cf78`):
  `request_threaded_irq(irq, handler 0x856c430, NULL, flags, "sc7a22-hand-up-irq", NULL)`, with
  `flags = gpio_get_value(irq_gpio) ? 8 (IRQF_TRIGGER_LOW) : 4 (IRQF_TRIGGER_HIGH)` (`csel` at
  `0x856cf5c`). No `IRQF_ONESHOT`: the handler is not threaded.
- Handler `0x856c430`: `disable_irq_nosync`, `pm_stay_awake` if the device may wake, queue the
  work, `IRQ_HANDLED`. No I2C access.
- Work `0x856c570`: read the flag register (0x06 on the 0x13 part, bank 0x90 register 0x3e on
  the 0x18 part), accumulate the step count, log, send the key, and then **re-arm on the
  opposite level**: `irq_set_irq_type(irq, gpio_get_value(irq_gpio) ? LEVEL_LOW : LEVEL_HIGH)`
  (`0x856c7cc`..`0x856c7f0`), then `enable_irq`. So the line only fires again after the pin
  changes.
- No write to an interrupt latch or source register in the IRQ path or at init: the only
  registers touched are the flag read, the step block and the hand-up block already listed in
  `SC7A22_INTERFACE.md`. The chip is not LIS3DH register-compatible here (no INT1_SRC 0x31,
  CTRL_REG5 or INT1_CFG writes), so there is no latch to clear; the opposite-level re-arm is how
  stock copes with a pin that stays asserted.
- The interrupt is requested and enabled at probe and `device_init_wakeup(dev, 1)` is called at
  probe; stock does not hold it disabled until `hand_start`. `hand_start` only gates the key
  event.

### Stock DT [T]

DM63 and DW100U: `irq-gpio = <&gpio 0x5a|0x5d 0>` (flags 0). The flags are not used by the
driver; the trigger comes from the pin level as above. The controller (`gpio-ap.c`) accepts only
level types for these lines (`-EINVAL` for edge) and has no `IRQCHIP_SET_TYPE_MASKED`.

### DW100U (revision B) [D]

Same sensor, same I2C address 0x18, same driver, GPIO 0x5d. The request (`0xffffff8008573328`)
uses a fixed `flags = 4`, as documented. Revision B reads the flag register too and does not
flip. The opposite-level re-arm is a superset of that behaviour (it cannot storm on a stuck-high
pin), so the same fix applies to DW100U, K18 and the other boards; they were not tested.

## Fix [S]

1. The trigger follows the pin: requested and re-armed as low when the pin is high and as high
   when it is low (as stock A).
2. The interrupt is threaded with `IRQF_ONESHOT`: the hard handler only does `pm_stay_awake` and
   returns `IRQ_WAKE_THREAD`; the thread reads the flags, re-arms the type, and returns
   `IRQ_NONE` when neither hand-up (0x02) nor hand-down (0x04) is set. The kernel's spurious
   interrupt detector then disables a line that is stuck. With the pin high and no source bit a
   rate-limited warning is logged, so a storm shows in dmesg. A low pin without a flag is the
   release interrupt and is not warned about.
3. The pin is sampled before the flags are read and again after servicing. If it moved
   meanwhile (a new assertion may have latched after the read), up to 2 extra passes run before
   arming for release, so a new assertion is not left unserviced until the release. The trigger
   is always the opposite of the last sampled level; a fixed level is never used.
4. I2C read and bank-selection errors are propagated, not turned into "no flags". On an error the
   opposite level is still armed (no storm) and `flag_work` retries the flag read up to 3 times
   at 50 ms, independent of pin transitions. The error path returns `IRQ_HANDLED` (a retry is
   pending), not `IRQ_NONE`. The retry is cancelled in suspend (re-queued on resume if tries
   remain) and in teardown after `free_irq()`.

The interrupt stays enabled from probe, as in stock.

## Open

- The pin behaviour is inferred from the stock re-arm and the bench counters; whether INT is
  asserted by a latched condition or by data-ready was not observed on a scope.
- Untested on hardware. After flashing, `/proc/interrupts` for irq 54 should stay near zero when
  the watch is still; "interrupt without a source" in dmesg means the chip holds INT high with no
  flag.
