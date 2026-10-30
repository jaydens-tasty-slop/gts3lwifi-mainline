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
`evtest`, 2026-10-29). Confirmed with a finger the same day: five taps
near the corners and the centre landed within 19 px of the targets
(mean 13 px).

Test: `evtest` on the touchscreen's event device, then tap near the
corners of the screen. In the panel frame (the boot logo's top is the
panel's left edge, so hold the tablet in landscape with the home button
on the right): top-left is X 0 / Y 0, top-right X 2047 / Y 0,
bottom-left X 0 / Y 1535.

The input device's resolution was 13 and 7 units per mm: `rmi_f12`
overwrote `touchscreen-x-mm`/`-y-mm` with its own estimate, pitch times
electrode count, which pairs the 49 RX electrodes (4 mm pitch) with x
while on this sensor they run along y (it read 196 x 148 mm for a
1535 x 2047 sensor). Patch 0064 is the upstream fix (Linux commit
9a29ee801f52: platform data first), and `dts/test/test-touch.dts` gives
the panel's active area in the sensor's frame, 147 x 196 mm: 10 units
per mm on both axes (seen 2026-10-29).

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

Seen on an SM-T820 (2026-10-29): the driver probes,
`wacom_w9000 3-0056: Wacom W9019 Digitizer size X19661Y14746` (the stock
DT's `max_coords`), firmware 0x0124 in the input id, pressure 0-4095,
resolution 100, INPUT_PROP_DIRECT; the supply is on only while the
device is open, and the interrupt stays quiet with no pen near
(level-low, no storm).  With the pen, the same day: five tip touches
near the corners and the centre landed within 19 px of the targets (mean
10 px) with the flags above; hover, pressure (up to 1721 of 4095 for a
light tap) and the side button were all reported, and the interrupt took
1208 interrupts over the test and went quiet afterwards.

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

## Keys

| Key | Line | Driver, code |
|---|---|---|
| Power | PM8994 PON KPDPWR | `pm8941-pwrkey` (pm8994.dtsi, main DTS), KEY_POWER |
| Volume Down | PM8994 PON RESIN | `pm8941-pwrkey` (`&pm8994_resin`), KEY_VOLUMEDOWN |
| Volume Up | PM8994 GPIO 3, active low, 30 uA pull-up | `gpio-keys`, KEY_VOLUMEUP |
| Home | PM8994 GPIO 2, active low, external pull-up | `gpio-keys`, KEY_HOMEPAGE, wakeup source |

From the stock DT: `gpio_keys` in `msm8996-sec-common.dtsi` (15 ms
debounce), `qcom,pon_2` and the PMIC GPIO nodes `gpio@c100`/`gpio@c200`
in `msm8996-sec-gts3lwifi-r05.dtsi` (digital inputs at 1.8 V).
`dts/test/test-keys.dts` describes the three keys on top of the microSD
test.

Home keeps the stock code, KEY_HOMEPAGE, as the other Samsung DTs upstream
do. Plasma desktop does nothing with KEY_HOMEPAGE, so on our install a
udev hwdb entry turns Home into Meta, and a lone Meta tap opens the Plasma
launcher:

    # /etc/udev/hwdb.d/61-gts3-home-key.hwdb
    evdev:name:gpio-keys:*
     KEYBOARD_KEY_1=leftmeta

then `systemd-hwdb update` (or `udevadm hwdb --update`) and a reboot.
`gpio-keys` uses each button's index as its scan code, so 1 is Home, the
second button in `test-keys.dts`. Seen on an SM-T820 (2026-10-29): with
the entry, a Home tap opens the launcher.

Not done:

- the hall sensor of the cover (stock `hall_flip`, PM8994 GPIO 7), which
  would be SW_LID: left out until suspend works, since logind suspends on
  SW_LID;
- the capacitive Back and Recent keys: a Coreriver TC305K at 0x20 on a
  GPIO I2C bus (TLMM 21 SDA, 22 SCL), interrupt TLMM 64, supply PM8994
  L19, key LEDs on TLMM 118 (stock `tc305k@20` in
  `msm8996-sec-gts3lwifi-r05.dtsi`, driver
  `drivers/input/keyboard/tc3xxk.c`). Upstream `tm2-touchkey` drives a
  Coreriver TC360; its protocol needs comparing with `tc3xxk.c` first.
