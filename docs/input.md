# Input: touchscreen and S-Pen on the Galaxy Tab S3

Both digitizers sit on top of a landscape panel (2048 x 1536) in a
portrait device. DRM expects touch input in the panel's own frame
(drm_connector.c, "panel orientation"): user space that turns the
picture to portrait applies the same turn to the touch input. So the
DT flags below map each digitizer to the landscape panel frame, origin
at the panel's top-left, and the panel's `rotation = <270>`
(docs/display.md, round 20) tells user space about the portrait
mounting.

## Touchscreen (Synaptics S5807, RMI4)

Stock (Samsung GPL kernel, `msm8996-sec-gts3lwifi-r04.dtsi`): the
`synaptics_dsx2` driver reports `max_coords = <1536 2048>` with no
flip properties set, and stock Android rotates the display by 270
degrees (`SF_PRIMARY_DISPLAY_ORIENTATION := 270` in LineageOS's
`BoardConfigCommon.mk`). So the raw coordinates are Android's portrait
frame, and in the panel's frame (x, y) = (y_raw, 1535 - x_raw).

RMI4 applies `touchscreen-inverted-x/y` before `touchscreen-swapped-x-y`
(`rmi_2d_sensor_abs_process()`), so `dts/test/test-touch.dts` sets:

    touchscreen-inverted-x;
    touchscreen-swapped-x-y;

With them the input device reports X 0-2047 and Y 0-1535 (seen with
`evtest`, 2026-10-29). The mapping is not yet confirmed with a finger.

Test: `evtest` on the touchscreen's event device, then tap near the
corners of the screen. In the panel frame (the boot logo's top is the
panel's left edge, so hold the tablet in landscape with the home button
on the right): top-left is X 0 / Y 0, top-right X 2047 / Y 0,
bottom-left X 0 / Y 1535.

The input device's resolution (13 and 7 units per mm) comes from
`rmi_f12`'s own estimate and overrides `touchscreen-x-mm`/`-y-mm`;
the true value is about 10.4 units per mm on both axes.

## S-Pen (Wacom W9019, Samsung firmware)

Stock DT: `wacom@56` on `i2c_12` (BLSP2 QUP6 at 0x75ba000, GPIO 87/88:
mainline `blsp2_i2c6`), compatible `wacom,w90xx`, `wacom,ic_type =
<9019>`, interrupt GPIO 34, pen detect GPIO 117, firmware write enable
GPIO 24, 3.3 V from a fixed regulator on GPIO 126 (boot-on), boot loader
at 0x09, `max_coords = <19661 14746>`, `max_pressure = <4095>`,
`invert = <1 0 1>` (x flipped, then x and y swapped).

Samsung's firmware does not speak the HID-style protocol of mainline's
`wacom_i2c`. From Samsung's `wacom_tab` driver: command 0x2a returns a
15-byte query (header 0x0f, then maximum x, y, pressure and the firmware
version, big endian), and each interrupt has a 14-byte packet to read:

| byte | meaning |
|---|---|
| 0 | bit 7 in range, bit 6 eraser, bit 5 side button, bit 4 tip |
| 1-2, 3-4 | x, y (big endian) |
| 5-6 | pressure (big endian) |
| 7 | height |
| 8, 9 | tilt x, tilt y (signed) |
| 12 | bit 7: soft key event instead of a pen sample |

Linux 7.3 has a driver for this protocol family, `wacom_w9000`
(W9002, W9007A; Hendrik Noack, Ferass El Hafidi).  Patches 0056-0057 are
its two upstream commits, verbatim (185cd1d8cb7f binding, acaefbacc997
driver).  Patches 0058-0062 add the W9019 to it: compatible
`wacom,w9019`, which requires `vdd-supply` and an active-high
`flash-mode-gpios` held low; the split query (send 0x2a, wait 100 ms,
read 15 bytes, three tries 100 ms apart, as `wacom_i2c_query()`); the
14-byte report with the soft-key marker; the 10 ms off interval; KUnit
tests (14/14 on this tree).  Patch 0063 sets INPUT_PROP_DIRECT, which
the upstream driver did not.  Enable with `CONFIG_TOUCHSCREEN_WACOM_W9000`.

The driver reports ABS_X/Y, pressure, BTN_TOOL_PEN or BTN_TOOL_RUBBER,
BTN_TOUCH for the tip and BTN_STYLUS for the side button.  Hover shows as
BTN_TOOL_PEN without BTN_TOUCH; the W9019 variant reports no distance or
tilt, since the stock source gives no unit for them.  The digitizer is
powered only while the input device is open.  The axes go through the
common `touchscreen-inverted-x/y` and `touchscreen-swapped-x-y`, and
`touchscreen-x-mm`/`-y-mm` set the resolution (196 x 147 mm, the panel's
active area: 100 units per mm).

Axes: stock maps (x, y) to portrait (y, 19661 - x), and the panel's
frame is portrait turned back by 270 degrees, so in the panel's frame
the pen's x and y are both inverted and not swapped:

    touchscreen-inverted-x;
    touchscreen-inverted-y;

Seen on an SM-T820 (2026-10-29, no pen to hand): the driver probes,
`wacom_w9000 3-0056: Wacom W9019 Digitizer size X19661Y14746` (the stock
DT's `max_coords`), firmware 0x0124 in the input id, pressure 0-4095,
resolution 100, INPUT_PROP_DIRECT; the supply is on only while the
device is open, and the interrupt stays quiet with no pen near
(level-low, no storm).  Not yet tested: hover, tip, pressure, side
button and the axis directions.

Test: `evtest` on "Wacom W9019 Digitizer"; hover the pen (BTN_TOOL_PEN
1), touch the screen (BTN_TOUCH 1, ABS_PRESSURE rising with force), press
the side button (BTN_STYLUS), and touch near the corners as for the
touchscreen: top-left in the panel frame is X 0 / Y 0, top-right X 19661
/ Y 0.  If nothing arrives, `grep w9019 /proc/interrupts` while the pen
is near and evtest runs: a count stuck at 0 means the interrupt does not
fire (try `IRQ_TYPE_EDGE_FALLING`, the stock trigger).

An earlier version of this branch had a standalone `wacom_w9019` driver
(distance and tilt too); it duplicated `wacom_w9000` and is gone.

Not done: firmware update (the IC keeps its firmware in flash), the pen
detect line (GPIO 117; stock only logs it), the stock "survey" low-power
mode.
