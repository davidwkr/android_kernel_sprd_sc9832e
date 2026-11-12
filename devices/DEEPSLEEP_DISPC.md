# SC9832E deep-sleep check and the dispc AHB enable

Kernel evidence only (4.4.302 `devices/k18-mali-candidate`, 4.14 `kernel-4.14`,
stock DM63 4.4.83 boot.img strings). No device measurement.

## What the check is

`drivers/soc/sprd/pm-sharkle.c`: `sprd_deep_condition_check()` (called from
`sprd_deep_sleep_status()` on `CPU_CLUSTER_PM_ENTER`, after
`sprd_deep_disable_module()`). `BITS_CHECK()` only does `pr_info("<bit> not 0")`
when the bit is set; it gates nothing. The stock DM63 boot.img contains the
same strings ("#####Enter Deepsleep  Condition Check!######", "BIT_AP_AHB_DISPC_EB
not 0", the INTC/UART1/SIM0 lines), so the print is stock behaviour.

Checked bits (sharkle `ap_ahb.h` / `ap_apb.h` / `aon_apb.h`): AP AHB: DSI,
DISPC, VSP, GSP, DMA_PUB, OTG, CE_PUB, SDIO0/1, NANDC, EMMC. AP APB: SIM0, IIS0,
SPI0/2, I2C0-4, UART1, INTC0-3. AON APB: GPU, MM, CA53_DAP, AP_DAP.

`REG_AP_AHB_AHB_EB` = 0x82 is bit 1 `DISPC_EB` and bit 7 `CKG_EB` (clock
generator, always on). INTC0-3, UART1, SIM0 (AP APB) and CA53_DAP (AON) are
set on a running system and are listed by the check unconditionally; they are
not owned by anything the display path touches. DISPC_EB is the one bit that
should be clear with the panel off and runtime-suspended.

## Root cause

`drivers/video/adf/sprd/drv/sharkle/global_dispc.c`: `dispc_glb_enable()` does
`clk_prepare_enable(clk_dispc_ahb_eb)` (the AP AHB bit 1 gate, DT
`clk_dispc_ahb_eb`), `dispc_glb_disable()` does `clk_disable_unprepare()` on it.
It is reference counted.

`drivers/video/adf/sprd/drv/sprd_dispc.c`, `sprd_dispc_resume()`:

- step 1 calls `dispc->glb->enable(ctx)` (the original, stock-style call);
- step 3, after `glb->reset()`, called `dispc->glb->enable(ctx)` a second time
  ("Re-enable GLB after reset" / "Final Fix V2").

`sprd_dispc_suspend()` -> `sprd_dispc_uninit()` calls `glb->disable()` once.
Every runtime resume therefore leaks one reference on the gate: after the first
suspend/resume cycle the count is 2 at resume and 1 after suspend, so
`DISPC_EB` stays set for good while the display is off.

The second enable was unnecessary: the reset writes `REG_AP_AHB_AHB_RST`
(`BIT_AP_AHB_DISPC_SOFT_RST`), a different register from `AHB_EB`. The source
cannot prove that the silicon reset has no side effect on `AHB_EB`, but the
second enable could not have helped anyway: the clock framework only counts a
second `clk_prepare_enable()` on an enabled gate and does not write the
hardware again. Both messages (`Powering up...`, `Re-enable GLB`,
`Final Fix V2`) are absent from the stock image strings, which only has
"dispc suspend OK" / "dispc resume OK". The modified resume is present since
the squashed import commit (`git log -S` shows only `df2e35f95603`).

## 4.14

`kernel-4.14` has the identical resume (`sprd_dispc.c` calls `glb->enable` at
the "Powering up" step and again at "Re-enable GLB after reset") and the same
`sharkle/global_dispc.c`: same leak.

## Fix

Drop the second `glb->enable()` in `sprd_dispc_resume()`; on the successful
paths `enable`/`disable` are balanced again and runtime suspend releases
`DISPC_EB`. To confirm on a watch: with the screen off on battery the check no
longer prints `DISPC_EB not 0`, and `AP_DEEP_SLEEP_CNT` rises (if it stays 0,
another source still blocks deep sleep).

Open (pre-existing, both trees): when a clock enable fails, the error paths of
`dispc_glb_enable()` (`sharkle/global_dispc.c`), the core/DPI clock enable and
`global_dsi.c` disable clocks they never acquired, and init/resume ignore the
failure, so the next suspend disables again. Only reachable on a clock failure.
