# DM63 display panel on the 4.14 DRM stack

Scope: how the 4.14 tree describes and selects a MIPI DSI panel, how the three stock CO5300 AMOLED variants
map onto it, and what is still open. Compile only, nothing was run on a watch. Sources: `drivers/gpu/drm/sprd/`
of this tree, the 4.4 ADF driver `drivers/video/adf/sprd/drv/` and the stock DT `devices/dm63/stock.dts` (both
of the 4.4 tree, read only). Files: `arch/arm64/boot/dts/sprd/lcd/lcd_co5300_oled{1952,206,213}_mipi.dtsi`,
board part in `arch/arm64/boot/dts/sprd/sp9832e-1h10-dm63.dts`.

## 4.14 binding

Chain: `dpu` (`sprd,display-processor`, `sprd,ip = "dpu-lite-r1p0"`) -> `dsi` (`sprd,dsi-host`) -> `dphy`
(`sprd,dsi-phy`) -> panel. The DT graph ends in the child node of `&dsi` (`compatible =
"sprd,generic-mipi-panel"`, `reg = <0>`, `port` with `panel_in`). `sharkle.dtsi` already wires
`dpu_out/dsi_in/dsi_out/dphy_in/dphy_out`.

Two nodes carry panel data, read by `sprd_panel.c`:

* The DSI child node `panel` (board property): `power-supply` (required, see below), `avdd-gpio`, `avee-gpio`,
  `reset-gpio` (read with `gpiod_get_optional`, ASIS, so the driver does not touch them before the first
  prepare), optional `sprd,backlight` phandle (a PWM backlight, not the OLED one), `sprd,force-attached`
  (string), `sprd,surface-width/height` (extra builtin mode, unused).
* The `/lcds/<name>` node (panel property, one dtsi per panel):
  `sprd,dsi-work-mode` (0 command, 1 video burst, 2 sync pulse, 3 sync event), `sprd,dsi-non-continuous-clock`,
  `sprd,dsi-lane-number` (default 4), `sprd,dsi-color-format` (`rgb888` default, `rgb666`, `rgb666_packed`,
  `rgb565`, `dsc`), `sprd,width-mm` / `sprd,height-mm` (68 / 121 default), `sprd,phy-bit-clock` (kbps, read by
  `sprd_dsi.c`, default 500000), `sprd,phy-escape-clock`, `sprd,esd-check-enable/-mode/-period/-register/-value`
  (mode 0 register read, 1 TE; defaults 1, 1000 ms, 0x0a, 0x9c), `sprd,use-dcs-write`,
  `sprd,reset-on-sequence` / `sprd,reset-off-sequence` (pairs `<level delay-ms>`, both mandatory),
  `sprd,initial-command`, `sprd,sleep-in-command`, `sprd,sleep-out-command` (byte strings),
  `display-timings` (first `timingN` is the native mode), child `oled-backlight`.
* `oled-backlight` child: `brightness-levels` (command bytes), `sprd,reg-lock`, `sprd,reg-unlock`,
  `default-brightness-level` (initial 0..255 backlight value, default 25), `sprd,max-level` (default 255).
  It registers a backlight class device named `sprd_backlight`.

Sequence in the driver: `prepare` = `regulator_enable(power)`, avdd gpio high + 5 ms, avee gpio high + 5 ms, walk the
reset-on sequence on the reset gpio. `enable` = send the initial command stream, then update the PWM backlight
if a `sprd,backlight` exists. `disable` = send sleep-in. `unprepare` = avee low + 5 ms, avdd low + 5 ms, reset-off
sequence, `regulator_disable`. `sprd,sleep-out-command` is parsed but never sent; `enable` re-sends the whole
initial command stream after a resume instead (the 4.4 driver also re-ran `init-data` on resume).

Command writes: `sprd_panel_send_cmds` walks `type wait len-hi len-lo payload`; it ignores `type`. Without
`sprd,use-dcs-write` every command goes out as a generic write, whose packet type the DRM core picks from the
payload length (1 byte 0x13, 2 bytes 0x23, 3 or more 0x29); with it, as DCS writes (0x05, 0x15, 0x39). `wait`
is an `msleep` after the command.

DSI mode: `sprd_dsi_host_attach` takes lanes, format and mode from the panel; `sprd-dpu` decides the DPU
interface in `sprd_crtc_mode_set_nofb`: edpi (command mode with TE) only if `hdisplay == htotal` or
`vdisplay == vtotal`, otherwise dpi (video). A command mode panel therefore needs timings with no horizontal
(or vertical) blanking, whatever `sprd,dsi-work-mode` says. The DPU also sets the DPI clock to
`display-timings` `clock-frequency` (`dpu_clk_init`, source 96 / 128 / 153.6 MHz, the first that divides it).

Power supply: `sprd_panel_probe` calls `devm_regulator_get(dev, "power")`. With no `power-supply` the
regulator core returns -ENODEV (nothing calls `regulator_has_full_constraints()` on this board, so no dummy
supply) and the probe fails. The board DTS gives the panel a `regulator-fixed` without gpio.

## Panel selection, 4.14 against stock

Stock (4.4 ADF): the bootloader puts `lcd_id=ID<hex>` on the command line (DM63: `ID5300213`). `panel_info_attach`
searches `sprd-panel-if/panel-drivers` (14 phandles) for the node named `lcd<hex>` (`lcd5300213`);
`lcd_id=0` means calibration mode, a `force-id` property overrides.

4.14: there is no `lcd_id` parsing. `sprd_panel.c` has `__setup("lcd_name=")` (the node name under `/lcds`,
for example `lcd_name=lcd_co5300_oled213_mipi`), and the panel node property `sprd,force-attached` overrides it.
If neither is present the path becomes `/lcds/(null)` and the probe returns -ENODEV (blank, no crash).
`sprd_dsi_find_panel` then asks every `/lcds` child for a registered panel, so several dtsi files can be
compiled in at once; only the selected one registers. `lcd_id=ID5300213` on the command line is ignored.

DM63 DTS: all three CO5300 dtsi are included, `sprd,force-attached = "lcd_co5300_oled213_mipi"` (the stock
DM63 reports ID5300213). Other panels of the stock `lcds` (GC9B71 x2, ST77916 x3, ST7797, ST7801, CH13613,
SH8601 x2, dummy) are not converted here. To run a 1.952 inch (ID53001952) or 2.06 inch (ID5300206) unit change
that string, or drop the property and pass `lcd_name=` from the boot image command line.

## Stock CO5300 nodes

Three variants, all `work-mode 0`, `bpp 24`, `lane-number 1`, 412x502, `fps 60`, 68x121 mm, `need-check-esd 0`
(`esd-check-reg 0x0a`, `esd-return-code 0x9c`, `esd-timeout 2000`), same power sequences and same sleep-in /
sleep-out.

| | lcd53001952 (ID53001952) | lcd5300206 (ID5300206) | lcd5300213 (ID5300213) |
|---|---|---|---|
| clock-frequency (stock, DSI kbps) | 409000 | 409000 | 460000 |
| hback / hfront / hsync | 24 / 16 / 4 | 24 / 16 / 4 | 18 / 30 / 2 |
| vback / vfront / vsync | 8 / 9 / 1 | 8 / 9 / 1 | 7 / 6 / 1 |
| init-data | 112 bytes, 18 commands | 112 bytes, same but window | 64 bytes, 10 commands |
| column window (0x2a) | 0x002b..0x01c6 | 0x0016..0x01b1 | 0x0015..0x01b0 |
| default-brightness | 120 | 120 | 25 |

The 1952 and 206 init streams start with the same eight vendor-page writes (`fe 20`, `f4 5a`, `f5 59`, `fe d0`,
`2a 80`, `2b 02`, `fe 90`, `4e 1f`) that the 213 stream does not have; all three then run `fe 00`, `35 00` (TE on),
`3a 77` (24 bit), `53 20`, `51 00`, `63 ff`, the 2a/2b window (rows 0..0x1f5 in all), `11` (wait 120 ms), `29`
(wait 20 ms). The `fe XX` writes are CO5300 command page selects; their meaning was not checked against a
datasheet, the bytes are copied.

### Mappings

| Stock | 4.14 | Note |
|---|---|---|
| `work-mode 0` | `sprd,dsi-work-mode = <0>` | command mode |
| `bpp 24` | `sprd,dsi-color-format = "rgb888"` | |
| `lane-number 1` | `sprd,dsi-lane-number = <1>` | |
| `display-timings clock-frequency` (409000 / 460000) | `sprd,phy-bit-clock` (same number, kbps) | the 4.4 driver assigns this value to `phy_freq`, the DSI bit clock; it is not a pixel clock |
| `hactive/vactive` 412x502 | same | |
| porches, sync | vertical values stock, horizontal 0 | needed for the edpi selection above; the 4.14 edpi path (`dpu_dpi_init`) does not program them |
| (no counterpart) pixel clock | `clock-frequency = <128000000>` | stock dispc `clock-src` = 0x16e36000 (384 MHz), 0x7a12000 (128 MHz); assumed that the 4.4 edpi path ran from the second one, unverified |
| `need-check-esd 0`, `esd-*` | `sprd,esd-check-enable = <0>`, register 0x0a, value 0x9c, period 2000, mode 0 | off as in stock |
| `width-mm`, `height-mm` | `sprd,width-mm = <68>`, `sprd,height-mm = <121>` | equal to the 4.14 defaults |
| `power-on-sequence` `33 1 20, 50 1 20, 50 0 20, 50 1 50` | board `avdd-gpio = <&ap_gpio 33>`, `reset-gpio = <&ap_gpio 50>`; `sprd,reset-on-sequence = <1 20>, <0 20>, <1 50>` | gpio 33 high with 20 ms wait becomes avdd high with the driver's fixed 5 ms |
| `power-off-sequence` `50 0 20, 33 0 20` | `sprd,reset-off-sequence = <0 20>`, avdd low in the driver | order differs, see risks |
| `init-data`, `sleep-in`, `sleep-out` | `sprd,initial-command`, `sprd,sleep-in-command`, `sprd,sleep-out-command` | byte encoding below |
| `oled-backlight` `default-brightness` | `default-brightness-level` (120 / 120 / 25) | |
| `oled-backlight` `max-level 255` | `sprd,max-level = <255>` | |
| `oled-backlight` `brightness-levels` | one command `23 0a 00 02 51 00` | below |
| `fps 60`, `panel-name` | none | fps comes from the timings |
| `lcd_id`, `panel-drivers` | `sprd,force-attached` / `lcd_name=` | |

### Init command encoding

Both drivers use the same stream layout, a repetition of `type wait len-hi len-lo payload[len]`: `type` is the
DSI data type, `wait` a delay in ms after the command, `len` a big endian 16 bit payload length (the 4.4
`of_parse_cmds` does `ntohs(header->len)`, the 4.14 driver `(wc_h << 8) | wc_l`). So the bytes carry over
unchanged. What differs is how the stock DT stores them. The three CO5300 nodes in the decompiled stock DT
hold `init-data` and `brightness-levels` as 32 bit cell arrays (`<0x23000002 0xfe002300 ...>`); the other
panel nodes use byte strings. A cell is the four bytes big endian, dtc prints it without leading zeros, so a
cell like `0x2f45a` is `00 02 f4 5a`. 4.14 takes a byte string `[ .. ]` (`of_get_property` returns the raw
bytes and cells and bytes are the same bytes in the blob, so the cell form would also be read correctly; the
byte form is for readability and for the tree's other dtsi).

Worked example, start of the 2.13 inch stream: `<0x23000002 0xfe002300 0x23500 ...>` is the bytes
`23 00 00 02 fe 00 | 23 00 00 02 35 00 | ...`: type 0x23 (generic write, 2 bytes), wait 0, length 2,
payload `fe 00` (select page 0), then `35 00` (TE on). The end of the stream
`... 0x29000005 0x2a001501 0xb0290000 0x52b0000 0x1f51378 0x11113 0x14000129` is
`29 00 00 05 2a 00 15 01 b0 | 29 00 00 05 2b 00 00 01 f5 | 13 78 00 01 11 | 13 14 00 01 29`: column window
0x0015..0x01b0, row window 0..0x1f5, sleep out (wait 120 ms), display on (wait 20 ms; note `0x052b0000` is
printed as `0x52b0000`).

Data type: the 4.14 driver ignores `type`. A script check over all 18 + 18 + 10 CO5300 commands found that
every stock `type` equals what the generic write path derives from the length (0x23 for 2 byte payloads, 0x29
for 5 byte payloads, 0x13 for 1 byte payloads), so nothing is lost without `sprd,use-dcs-write` and the
property is not set. Do not set it: it would turn the 0x13/0x23 commands into DCS packets, which the stock
sequence never sent.

### OLED brightness

4.14 has an OLED backlight driver inside `sprd_panel.c` (no separate file). Stock `brightness-levels` is 256
commands `23 0a 00 02 51 NN` with NN = 0..255 (script checked: exactly the identity table, wait 10 ms each).
The 4.14 driver offers two forms: one command per level, or a single command whose payload byte 1 it
overwrites with the level (`level = brightness * max-level / 255`). The table form cannot be used: `struct
sprd_oled` holds `cmds[255]` and `of_parse_oled_cmds` stores one pointer per command, so 256 commands write one
entry past the array (into `cmd_len`). The single command form gives the same bytes on the wire, so the dtsi
uses it. Open points: no `sprd,reg-lock` / `sprd,reg-unlock` (none in stock). A brightness write is sent only
through `sprd_oled_set_brightness`; the panel enable path updates only a PWM `sprd,backlight`, which DM63 does
not have. After a sleep/resume cycle the init stream sets `51 00`, so the screen stays dark until userspace
writes the brightness again. The 4.4 driver behaves the same on a normal resume (it re-sends brightness
only after an ESD recovery); whether the Android side rewrites the level on screen on was not checked. Not tested.

## Gaps and TODO

* AOD / idle mode (`sprd_aod_idle`, DCS 0x38/0x39 on the CO5300) has no 4.14 port; it used the 4.4 `panel_device`
  and the OLED backlight object.
* Only the CO5300 variants are converted. The other stock `lcds` nodes are not.
* The refresh rate that DRM reports for the mode is `128 MHz / (412 * vtotal)`, around 600 Hz instead of 60.
  The panel is paced by TE, so the picture should not depend on it, but a userspace that trusts the mode
  vrefresh for its vsync period would be wrong. A 60 Hz mode (`412 * 520 * 60` = 12.85 MHz for the 2.06 / 1.952
  inch) would make the DPU pick the first divisible source or the 96 MHz default and an approximate DPI
  clock, which can not be ruled out on paper to be slower than the TE period; therefore the stock 128 MHz was
  kept. Decide on hardware.
* avdd timing: stock waits 20 ms after gpio 33, the 4.14 driver 5 ms. Not compensated (no invented values).
* `lcd_id` is not read; `sprd,force-attached` fixes the variant at build time.

## Risks for a first boot

* The first `dsi` encoder enable of 4.14 is skipped on purpose (`is_enabled` in `sprd_dsi.c`, the
  bootloader's logo is assumed to be on, the panel probe sets `is_enabled`). If the bootloader leaves the panel
  off or in another state, the screen stays blank until the first screen off/on cycle, which re-runs the full
  power-on and init sequence.
* Wrong variant or wrong `force-attached`: blank, shifted or cropped picture. The init commands only select
  pages, the TE, colour depth, brightness and windows, nothing that changes panel voltages; no variant-specific
  voltage register is written (the vendor page writes of the 1.952 / 2.06 inch streams were not decoded, they
  are the stock bytes).
* Power-off order: the 4.14 driver cuts avdd (gpio 33) and then pulls reset low; stock pulled reset low first,
  then avdd. The panel is in sleep-in (0x28, 0x10) at that point, so damage is not expected, but a panel
  with reset still high and no supply is a state stock never produced.
* TE: the init stream turns the TE output on and the edpi path waits for TE. If the TE pad is not muxed as in
  stock the DPU waits for the TE timeout (blank or very slow refresh). No pinctrl for the TE pad is in the DTS
  (the stock DT has none either; who muxes the pad was not checked).
* The fixed regulator without gpio is only there to satisfy `devm_regulator_get`; gpio 33 is not behind it and
  stays under the panel driver, so first boot does not change gpio 33 before the first prepare.
* `sprd_backlight` is the backlight name of the OLED driver; `sprd_bl.c` binds only
  `sprd,sharkl5pro-backlight`, no node here, so no name clash. `BACKLIGHT_PWM=y` stays in the defconfig.
