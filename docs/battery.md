# Battery: SM5705 fuel gauge

## Status (2026-10-28): the fuel gauge works

With `dts/test/test-battery.dts` and kernel patches 0012-0013 (new driver
`sm5705_fuel_gauge`, `CONFIG_FUEL_GAUGE_SM5705=m`), the battery shows up as
`/sys/class/power_supply/sm5705-battery`:

```
POWER_SUPPLY_STATUS=Charging
POWER_SUPPLY_TECHNOLOGY=Li-ion
POWER_SUPPLY_VOLTAGE_MAX_DESIGN=4300000
POWER_SUPPLY_VOLTAGE_MIN_DESIGN=3400000
POWER_SUPPLY_VOLTAGE_NOW=4374511
POWER_SUPPLY_VOLTAGE_OCV=4377441
POWER_SUPPLY_CURRENT_NOW=79589
POWER_SUPPLY_CHARGE_FULL_DESIGN=6000000
POWER_SUPPLY_ENERGY_FULL_DESIGN=23100000
POWER_SUPPLY_CAPACITY=100
POWER_SUPPLY_TEMP=378
```

UPower (`upower -d`) picks it up, so desktops get a battery percentage and
icon.

The driver only reads what the gauge computes: state of charge, voltage,
open-circuit voltage, current and temperature. It does not load a battery
model, and there is no charger driver yet (see below).

## Hardware

| | |
|---|---|
| Fuel gauge | SM5705 fuel gauge, I2C address 0x71, device ID 0x000b |
| Bus | TLMM 6 (SDA) / 7 (SCL). Stock drives them as an `i2c-gpio` bus, but they are the BLSP2 QUP2 I2C pins, so the test DT uses the hardware controller `&blsp2_i2c2` (0x75b6000) at 100 kHz |
| Alert interrupt | PM8994 GPIO 1 (stock `fuelgauge,fuel_int`), not used: the driver polls every 30 s |
| Charger | SM5705 charger at 0x25 and 0x49 on a separate `i2c-gpio` bus, TLMM 33 (SDA) / 32 (SCL); charge enable PM8994 GPIO 13. No driver yet |
| Battery | EB-BT825ABE, 6000 mAh, 3.85 V, 23.1 Wh |
| Stock limits | charges to 4.30 V (`battery,chg_float_voltage`), full condition 4.20 V and 93 %, recharges below 4.23 V, powers off around 3.4 V |

The battery data is a `simple-battery` node (`monitored-battery`). The power
supply core exposes design capacity, energy, voltages and chemistry from it
by itself.

## Registers

All registers are 16 bit, read with an SMBus word read (low byte first).
Layout and formulas from Samsung's `sm5705_fuelgauge.c`:

| Reg | Name | Decoding | Example |
|---|---|---|---|
| 0x00 | DEVICE_ID | | 0x000b |
| 0x05 | SOC | bits 15:8 percent, 7:0 1/256 % | 0x6424 = 100.1 % |
| 0x06 | OCV | bits 13:11 volts, 10:0 1/2048 V | 0x2305 = 4.377 V |
| 0x07 | VOLTAGE | as OCV | 0x2307 = 4.378 V |
| 0x08 | CURRENT | bit 15 sign (set = discharging), 12:11 amps, 10:0 1/2048 A | 0x800a = -4.9 mA |
| 0x09 | TEMPERATURE | bit 15 sign, 14:8 degrees, 7:4 1/16 degree | 0x2590 = 37.5 °C |
| 0x10 | FG_OP_STATUS | bit 4 set: battery model loaded | 0x0010 |

The current sign was checked on the tablet. Right after a reboot the charger
topped the full battery up at +78 mA, CPU load on all cores lowered that to
+58 mA, and once charging ended the current settled at -3 to -5 mA.
Positive means charging, as the power supply ABI expects.

Stock shows the raw SOC: `fuelgauge,capacity_max` is 1000 (100.0 %) and
`capacity_min` 0. The gauge reports slightly over 100 % when the battery is
above stock's 4.30 V, and the driver clamps that to 100.

## Battery model

Samsung's driver loads a battery model at boot (`battery_params`: OCV
tables, RCE, RS, current and temperature calibration) when FG_OP_STATUS bit
4 is clear. The gauge runs from the battery and keeps that model across
reboots and OS changes, so on this tablet it still reports bit 4 set and its
SOC matches the voltage. If the battery is ever disconnected, the gauge
loses the model; the driver then warns at probe
(`gauge lost its battery model`) and the percentage may be wrong until
Android has booted once, or until the init sequence is ported.

## Status without a charger driver

`STATUS` is guessed from the gauge: current above +20 mA means charging,
below -20 mA discharging, otherwise full at 100 % and not charging below
that. There is no USB or AC power supply yet, so UPower cannot tell whether
a cable is plugged in.

The charger still charges on its own settings. Note: the battery reached
4.378 V (OCV 4.377 V) on mainline, above Samsung's 4.30 V float voltage.
That is within what a 3.85 V-nominal cell is usually rated for (4.40 V),
but Samsung stays lower to slow ageing, so leaving the tablet plugged in for
long periods on mainline wears the battery faster than stock. A charger
driver should set the float voltage back to 4.30 V. It should also set the
input current limit and report USB online status.

## Next

- SM5705 charger: float voltage, input current, online status (i2c-gpio
  on TLMM 32/33).
- Use the alert interrupt (PM8994 GPIO 1) for low battery instead of only
  polling.
- Port the model init (`battery_params`) in case the gauge ever resets.
