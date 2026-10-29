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
