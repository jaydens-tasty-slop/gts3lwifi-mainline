# Sensors

## Status (2026-10-30): accelerometer, gyroscope and magnetometer work; auto-rotate works with iio-sensor-proxy

| Sensor | Chip | State |
|---|---|---|
| Accelerometer | ST LSM6DS3 (K6DS3TR) | works, orientation correct in all four positions |
| Gyroscope | same chip | works |
| Magnetometer | AKM AK09916C | works (about 0.4-0.6 gauss) |
| Ambient light (RGB) | Capella CM3323 | found, but `qcom_smgr` asks it for proximity data, which it does not have (`Buffering request failed: 0x501`); needs a light channel in the driver |
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

iio-sensor-proxy 3.9 works with these trigger-less buffers as they are.
The driver's mount matrix (`0, -1, 0; -1, 0, 0; 0, 0, 1`) already gives the
right orientation for this tablet, where the landscape panel is mounted at
270 degrees and portrait is the natural position:

| Position | iio-sensor-proxy |
|---|---|
| portrait, USB port at the bottom | normal |
| landscape, USB port on the right | right-up |
| portrait upside down | bottom-up |
| landscape, USB port on the left | left-up |

If iio-sensor-proxy was installed after the sensors appeared, run
`udevadm trigger --subsystem-match=iio --action=add` once (or reboot).

## Known issues

- `qcom_smgr` registered `qmi_handle_release` through a function-pointer
  cast; with kCFI every unbind (stopping the SLPI, unloading the module)
  oopsed and left the remoteproc lock held. Patch 0076 fixes it. Before
  0076, do not stop or restart the SLPI.
- Light sensor: see the table above.
