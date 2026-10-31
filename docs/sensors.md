# Sensors

## Status (2026-10-30): accelerometer, gyroscope, magnetometer and light sensor work; auto-rotate and the light sensor work with iio-sensor-proxy

| Sensor | Chip | State |
|---|---|---|
| Accelerometer | ST LSM6DS3 (K6DS3TR) | works, orientation correct in all four positions |
| Gyroscope | same chip | works |
| Magnetometer | AKM AK09916C | works (about 0.4-0.6 gauss) |
| Ambient light (RGB) | Capella CM3323 | works with patch 0077: `qcom-smgr-light`, `in_intensity_both` in lux (about 20 indoors, up to ~31000 under a phone torch, 0 when covered); iio-sensor-proxy reports it |
| Cover (hall) | PM8994 GPIO 7, low = closed | not in the DT yet: closing the cover would suspend the tablet, and suspend is not reliable yet |

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

iio-sensor-proxy 3.9 works with these trigger-less buffers as they are, but
the orientation needs a device-specific mount matrix. KDE (and GNOME) turn
the output by the panel's mount (the landscape panel sits at 270 degrees)
and the sensor's orientation on top of that, so the accelerometer has to be
expressed in the panel's own frame. With the SMGR driver's generic matrix
(`0, -1, 0; -1, 0, 0; 0, 0, 1`) the desktop was rotated 90 degrees too far in
every position. The device package's udev rule `61-gts3-accel.rules` sets

    ACCEL_MOUNT_MATRIX="1, 0, 0; 0, -1, 0; 0, 0, 1"

for `qcom-smgr-accel`, which iio-sensor-proxy prefers over the driver's
matrix. Confirmed on an SM-T825 at commit 563456e (rotation correct in all
positions); the SM-T820 has the same board.

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
probably the colour temperature in kelvin; it is not exposed.

## Known issues

- `qcom_smgr` registered `qmi_handle_release` through a function-pointer
  cast; with kCFI every unbind (stopping the SLPI, unloading the module)
  oopsed and left the remoteproc lock held. Patch 0076 fixes it. Before
  0076, do not stop or restart the SLPI.
