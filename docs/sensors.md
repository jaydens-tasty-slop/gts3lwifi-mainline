# Sensors

## Status (2026-11-01): accelerometer, gyroscope, magnetometer and light sensor work; auto-rotate and auto-brightness can use them; no compass heading yet

| Sensor | Chip | Models | State |
|---|---|---|---|
| Accelerometer | ST LSM6DS3 (K6DS3TR) | both | works, orientation correct in all four positions |
| Gyroscope | same chip | both | works |
| Magnetometer | AKM AK09916C | both | raw field works (about 0.4-0.6 gauss); no compass heading: iio-sensor-proxy 3.9 cannot make one from a raw magnetometer, see [Compass](#compass) |
| Ambient light (RGB) | Capella CM3323 | both | works with patch 0077: `qcom-smgr-light`, `in_intensity_both` in lux (about 20 indoors, up to ~31000 under a phone torch, 0 when covered); iio-sensor-proxy reports it, KWin's auto-brightness can use it (off by default, see [Auto-brightness](#auto-brightness)) |
| Cover (hall) | PM8994 GPIO 7, low = closed | both | `dts/test/test-hall.dts` (SW_LID), untested; not in the main DT until suspend is reliable |
| Grip (SAR proximity) | Semtech SX9310 | SM-T825 only | `dts/test/test-sar.dts`, untested; mainline driver `sx9310`, nothing on a Linux desktop reads it |
| Proximity | none | - | no proximity sensor on either model |
| Fingerprint | Egis (`etspi`, BLSP1 SPI 6) | both | owned by a TrustZone app on stock; not reachable from Linux |
| Pressure, humidity, heart rate | none | - | |
| Virtual sensors (rotation vector, gravity, step counter, significant motion, gestures) | SLPI algorithms (SAM) | both | the SLPI computes them, but they are separate QMI services that no Linux driver speaks; the desktop does not need them |

The Back/Recent touch keys (Coreriver TC305K) are an input device, not a
sensor; they are not done yet (see the README).

## Inventory from stock

Stock Android's sensor list comes from the SLPI's registry defaults in
`/vendor/etc/sensors/sensor_def_qcomdev.conf` (the same file on both
models) and the HALs in `/vendor/etc/sensors/hals.conf`: `sensors.ssc.so`
(the SLPI sensors), `sensors.bio.so` and `sensors.grip.so` (the SM-T825's
SX9310). The registry configures four SLPI drivers: LSM6DS3 accelerometer
(SMGR sensor ID 0) and gyroscope (10), AK09916C (20) and CM3323 (40,
0x28); three more driver slots are empty (UUID 0). It also enables the
SLPI's algorithms (motion detection, gyroscope calibration, pedometer,
gestures, facing, gravity, orientation, rotation vectors, significant
motion) and carries a default magnetometer factory calibration (a soft-iron
matrix, with the bias marked invalid); each unit's own calibration is in
`sns.reg` on efs (below).

Outside the SLPI, Samsung's kernel (GPL, `msm8996-sec-gts3l*-r05.dtsi`)
has the cover's hall sensor in `gpio_keys` (`hall_flip`, both models) and,
on the SM-T825 only, two SX9310 grip sensors on GPIO I2C buses, of which
the LTE defconfig builds the driver for one (`CONFIG_SENSORS_SX9310`; the
"grip wifi" one, `CONFIG_SENSORS_SX9310_WIFI`, is off).

## How they are connected

There is no Samsung sensor hub and nothing on the application processor's
I2C buses. All of these sensors sit on the buses of the SLPI, Qualcomm's
sensor core (from the stock `sensor_def_qcomdev.conf`):

| Sensor | SLPI bus | Notes |
|---|---|---|
| LSM6DS3 | SSC SPI | interrupt TLMM 122, owned by the SLPI |
| AK09916C | SSC I2C 3, 0x0c | |
| CM3323 | SSC I2C 2, 0x10 | ambient light as the secondary data type |

Linux talks to the SLPI's sensor manager (SMGR) over QRTR. The msm8996
kernel already has the drivers: `qcom_q6v5_pas` boots the SLPI,
`qcom_sns_reg` serves it the sensor registry, and `qcom_smgr` exposes each
sensor as a buffer-only IIO device (`qcom-smgr-accel`, `-gyro`, `-mag`,
`-prox-light`).

## What the SLPI needs

- **Firmware:** Samsung-signed `slpi.mdt` + `slpi.b00`-`b14` from the
  `apnhlos` partition (`image/`). Relocatable, exactly the 10 MiB of
  `slpi_mem` (0x91700000).
- **Sensor registry (`sns.reg`):** Samsung keeps it on the **efs**
  partition, `/efs/FactoryApp/sensors/sns.reg`, not on `/persist` as other
  devices do. It has this unit's factory calibration, so a wiped userdata
  does not lose it. `qcom_sns_reg` loads it as
  `qcom/sensors/sns.reg-samsung,gts3lwifi`.
- **Power:** PM8994 LVS2 (SLPI pad supply and the sensors' supply), kept
  always on as in stock.

The device package's `gts3-firmware` (a boot service) copies the firmware
and the registry from the tablet's own partitions into
`/lib/firmware/updates/` on first boot; nothing proprietary is shipped.

## Auto-rotate

iio-sensor-proxy 3.9 works with these trigger-less buffers as they are. The
accelerometer axes come from the SLPI, and the two models do not report them
in the same frame:

| Model | Raw gravity, portrait with USB at the bottom | Mount matrix |
|---|---|---|
| SM-T820 | +X | the SMGR driver's own, `0, -1, 0; -1, 0, 0; 0, 0, 1` (iio-sensor-proxy reports normal, right-up, bottom-up, left-up in the right positions; checked on a board without a panel) |
| SM-T825 | +Y | `1, 0, 0; 0, -1, 0; 0, 0, 1` (with the driver's matrix the desktop was 90 degrees off in every position; confirmed in all positions with this one) |

The device package's udev rule `61-gts3-accel.rules` sets
`ACCEL_MOUNT_MATRIX` (which iio-sensor-proxy prefers over the driver's
matrix) on `qcom-smgr-accel` only when the device tree is the SM-T825 one
(`samsung,gts3llte`). A per-model `mount-matrix` in the device tree would be
cleaner, but the SMGR sensors have no DT nodes of their own.

If iio-sensor-proxy was installed after the sensors appeared, run
`udevadm trigger --subsystem-match=iio --action=add` once (or reboot).

## Light sensor

SMGR lists the CM3323 under the proximity/light sensor ID (0x28) with a
single data type of type 1 (secondary): "Capella CM3323 ALS", 5 Hz. Stock
registers it with no primary data type (`data_type1 0`, `data_type2 6` in
`sensor_def_qcomdev.conf`). The driver always streamed the primary data type,
which SMGR rejects with 0x501. Patch 0077 keeps each data type's own type,
streams the secondary one for such a light-only sensor and registers it as
`qcom-smgr-light` with an `in_intensity_both` channel, the layout
iio-sensor-proxy's buffered light driver reads. The value is plain lux
(scale 1), not 16.16 fixed point like the motion sensors. The sample's second
word moves between about 3960 and 5270 (warm room light vs a cool torch),
probably the colour temperature in kelvin; it is not exposed. Nothing would
read it: iio-sensor-proxy's D-Bus API has only `LightLevel` and
`LightLevelUnit`, and neither KWin nor Plasma (night light included) uses a
colour temperature. Before exposing it (as `in_colortemp`, like
`hid-sensor-als`), compare it with a reference under a few known lights.

## Auto-brightness

KWin 6.8 has ambient-light auto-brightness (`src/utils/lightsensor.cpp`,
`Workspace::applyLightChanges()` in `src/workspace.cpp`). It claims the
light sensor from iio-sensor-proxy, accepts only readings in "lux" (what
iio-sensor-proxy reports for IIO sensors), and sets the output's brightness
from a per-output curve of (lux, brightness) points. It is per output and
off by default:

1. `kscreen-doctor -o` should list the panel with "Automatic brightness:
   supported" (it needs `HasAmbientLight` from iio-sensor-proxy).
2. Turn it on with `kscreen-doctor output.<name>.autoBrightness.enable`, or
   "Automatically adapt to environment" under Brightness in System
   Settings > Display.
3. Train it: the curve starts as a single point (0 lux = 100 %), so until
   then the panel goes to full brightness. Each manual brightness change
   (slider, keys) adds a point at the current light level; keep the light
   steady for a few seconds, then set the brightness, at several levels
   (dark, room, window, outside). KWin interpolates linearly in lux, so a
   curve with only 20 and 30000 lux stays near the 20 lux value indoors.
   The curve is stored as `automaticBrightness` and `autoBrightnessCurve`
   in `~/.config/kwinoutputconfig.json`; edit that file only with Plasma
   stopped.

KWin waits 2 s after a reading changes, ignores changes under 20 %
(`KWIN_AUTO_BRIGHTNESS_DEADZONE`, in percent), and releases the sensor while
the screen is off. Brightness written to sysfs directly is not learned.

## Compass

The magnetometer is a plain 3-axis IIO device (`qcom-smgr-mag`,
`in_magn_{x,y,z}` in gauss, the same mount matrix as the accelerometer).
iio-sensor-proxy 3.9 has no compass driver for that: its only IIO compass
driver reads a tilt-compensated heading channel
(`in_rot_from_north_magnetic_tilt_comp`, which HID sensor hubs provide), so
`HasCompass` stays false. A compass needs either a new iio-sensor-proxy
driver that combines the accelerometer and the magnetometer (upstream
work) or a heading from the SLPI's own algorithms, which no Linux driver
reads. Nothing on the Plasma desktop uses a compass today; GNOME Maps (via
geoclue) and Qt's `QCompass` would.

Before that is worth doing, two things need checking on the tablet:

- **Calibration.** Whether the SMGR stream has the factory soft-iron matrix
  and a hard-iron (bias) correction applied is not known. Lay the tablet
  flat and turn it slowly through 360 degrees: X and Y should trace a
  circle centred on 0 with a radius of the local horizontal field (about
  0.15-0.25 gauss in mid latitudes). A circle off centre needs a bias
  correction, an ellipse a soft-iron one.
- **Axes.** The magnetometer must be in the accelerometer's frame. On the
  SM-T825 the accelerometer needs a different matrix (above); the
  magnetometer probably does too.

## Cover (hall)

`dts/test/test-hall.dts` adds the cover's hall sensor as its own
`gpio-keys` device: SW_LID, set when the cover is closed (PM8994 GPIO 7,
active low, no pull, 15 ms debounce, as in stock). Opening the cover wakes
the tablet; closing it does not (`wakeup-event-action`). It stays out of
the main DT until suspend works: with SW_LID, logind (`HandleLidSwitch`, at
the login screen) and PowerDevil (in a Plasma session, "When laptop lid
closed") suspend the tablet when the cover closes. To turn the switch off
without a reboot:
`echo 0 > /sys/devices/platform/gpio-hall-sensor/disabled_switches`.

To check: `evtest` shows SW_LID 1 with the cover closed and 0 open, also
with the Book Cover Keyboard folded behind the tablet; the switch wakes the
tablet from suspend when the cover opens and not when it closes; the
interrupt does not fire while the cover stays still.

## Grip (SM-T825)

`dts/test/test-sar.dts` adds the SM-T825's SX9310 next to the LTE antenna
(GPIO I2C on TLMM 81/82, interrupt TLMM 132) with the mainline `sx9310`
driver (`CONFIG_SX9310`, not in the package config). It uses the driver's
default register setup, not Samsung's; the readings will tell whether the
`semtech,*` tuning properties are needed. Android lowers the modem's
transmit power when it reports near; nothing on a Linux desktop reads it,
so it only matters once the modem works.


## Tests on the tablet

None of this is tested yet (2026-11-01). In order, cheapest first:

1. Kernel with patches 0140-0142: `monitor-sensor` still shows the
   orientation and the light level; `tools/smgr-read.sh qcom-smgr-accel 5`
   (iio-sensor-proxy stopped) prints about 9.8 on one axis; the
   sensors' `sampling_frequency` keeps a written value across buffer
   enables. Then lower the accelerometer rate step by step (`smgr-read.sh
   qcom-smgr-accel 20 <Hz>`) to find the rates SMGR accepts, before any udev
   rule sets one.
2. Light: in a dark room, read the lux with the panel at minimum and at
   maximum brightness; if the reading follows the panel, auto-brightness
   would chase itself. Then turn on and train KWin's auto-brightness
   (above).
3. Compass groundwork: `tools/smgr-read.sh qcom-smgr-mag 600 10` while
   turning the tablet flat through 360 degrees (calibration), and with the
   top edge pointing north, east, south and west (axes against the
   accelerometer's frame), on both models if possible.
4. Hall: boot `dts/test/test-hall.dts` (kernel unchanged), `evtest` on
   "GPIO Hall Effect Sensor" while closing and opening the cover, with and
   without the Book Cover Keyboard. Set PowerDevil's lid action to "Do
   nothing" (and `HandleLidSwitch=ignore` for logind) before the test while
   suspend is unreliable.
5. Grip (SM-T825, kernel with `CONFIG_SX9310=m`): boot
   `dts/test/test-sar.dts`, read `in_proximity*_raw` with a hand on and off
   the antenna area.

## Known issues

- The driver pushed each sample from an uninitialised stack buffer and
  never set its timestamp, so with the timestamp enabled (iio-sensor-proxy
  enables all channels) every motion sample carried 8 bytes of kernel stack
  to userspace. Patch 0140 clears the sample and timestamps it.
- With fewer than three axes enabled, a 3-axis buffer got the wrong axes;
  patch 0141 lets the IIO core pick them out.
- Writes to `sampling_frequency` were ignored: every buffer enable went
  back to the fastest rate, one QMI message per sample. Patch 0142 keeps
  the written rate. Nothing lowers it yet; which rates SMGR accepts needs a
  test on the tablet before a udev rule sets one (iio-sensor-proxy reads
  the accelerometer every 700 ms).

- `qcom_smgr` registered `qmi_handle_release` through a function-pointer
  cast; with kCFI every unbind (stopping the SLPI, unloading the module)
  oopsed and left the remoteproc lock held. Patch 0076 fixes it. Before
  0076, do not stop or restart the SLPI.
