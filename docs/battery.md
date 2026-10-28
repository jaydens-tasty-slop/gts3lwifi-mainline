# Battery and charger: SM5705

## Status (2026-10-28): fuel gauge and charger work

With `dts/test/test-charger.dts` (which includes `test-battery.dts`) and
kernel patches 0012-0015 there are two power supplies:

- `sm5705-battery`, the fuel gauge (`CONFIG_FUEL_GAUGE_SM5705=m`): state of
  charge, voltage, open-circuit voltage, current, temperature, and the
  battery's design data;
- `sm5705-charger`, the charger (`CONFIG_CHARGER_SM5705=m`, on an
  `i2c-gpio` bus, so also `CONFIG_I2C_GPIO=m`): USB power present, charging
  state, health, and the charge limits.

```
POWER_SUPPLY_NAME=sm5705-charger
POWER_SUPPLY_TYPE=USB
POWER_SUPPLY_STATUS=Full
POWER_SUPPLY_HEALTH=Good
POWER_SUPPLY_ONLINE=1
POWER_SUPPLY_CONSTANT_CHARGE_CURRENT=2000000
POWER_SUPPLY_CONSTANT_CHARGE_VOLTAGE=4300000
POWER_SUPPLY_INPUT_CURRENT_LIMIT=1500000
POWER_SUPPLY_CHARGE_TERM_CURRENT=300000

POWER_SUPPLY_NAME=sm5705-battery
POWER_SUPPLY_TYPE=Battery
POWER_SUPPLY_STATUS=Full
POWER_SUPPLY_TECHNOLOGY=Li-ion
POWER_SUPPLY_VOLTAGE_MAX_DESIGN=4300000
POWER_SUPPLY_VOLTAGE_MIN_DESIGN=3400000
POWER_SUPPLY_VOLTAGE_NOW=4296875
POWER_SUPPLY_VOLTAGE_OCV=4377441
POWER_SUPPLY_CURRENT_NOW=-625000
POWER_SUPPLY_CHARGE_FULL_DESIGN=6000000
POWER_SUPPLY_ENERGY_FULL_DESIGN=23100000
POWER_SUPPLY_CAPACITY=100
POWER_SUPPLY_TEMP=375
POWER_SUPPLY_CHARGE_TERM_CURRENT=300000
```

UPower shows the battery (percentage, state, icon) and the charger as line
power (`on-battery: no` while USB is plugged in).

**The charge voltage is now Samsung's 4.30 V.** Without a charger driver the
SM5705 kept the 4.38 V it boots with, and the battery sat at 4.38 V
whenever the tablet was plugged in. The charger driver applies the limits
from the battery node at probe. After that the battery first runs down to
4.30 V on USB power (the current above, -625 mA, is that), then the charger
holds it there.

Pulling the USB cable and plugging it back in (logged every 2 s):

```
12:27:06 online=1 chg=Full        bat=Full        i=-498535 irq=5
12:30:40 online=0 chg=Discharging bat=Discharging i=-229492 irq=5
12:30:42 online=1 chg=Charging    bat=Charging    i=-199707 irq=8
12:30:44 online=1 chg=Full        bat=Full        i=-420410 irq=9
```

Each change raised the charger interrupt and a `change` uevent on both
supplies; the battery follows through `external_power_changed`. UPower
recorded the battery as discharging and set line power back online at
replug. USB networking came back by itself.

Not done yet:

- the cable type: the SM5705 MUIC at 0x25 detects it, no driver yet;
- the flash LED, which shares the charger's I2C device;
- the fuel gauge's alert interrupt and battery model init.

## Hardware

| | |
|---|---|
| Fuel gauge | SM5705 fuel gauge, I2C 0x71, device ID 0x000b |
| Fuel gauge bus | TLMM 6 (SDA) / 7 (SCL). Stock drives them as an `i2c-gpio` bus, but they are the BLSP2 QUP2 I2C pins, so the test DT uses the hardware controller `&blsp2_i2c2` (0x75b6000) at 100 kHz |
| Fuel gauge alert | PM8994 GPIO 1 (stock `fuelgauge,fuel_int`), not used: the driver polls every 30 s |
| Charger | SM5705 charger and flash LED, I2C 0x49, device ID 0xf9, interrupt TLMM 38 (active low, pull-up) |
| MUIC | SM5705 MUIC (cable detection), I2C 0x25, interrupt PM8994 GPIO 4 |
| Charger bus | TLMM 33 (SDA) / 32 (SCL). These are HDMI DDC pins, not a BLSP pair, so like stock it is an `i2c-gpio` bus |
| Charge enable | PM8994 GPIO 13, active low. The bootloader leaves it low (charging on); Linux does not touch it |
| Battery | EB-BT825ABE, 6000 mAh, 3.85 V, 23.1 Wh |
| Stock limits | charges to 4.30 V (`battery,chg_float_voltage`), top-off 300 mA, full condition 4.20 V and 93 %, recharges below 4.23 V, powers off around 3.4 V |

The battery is a `simple-battery` node (`monitored-battery`). The power
supply core exposes its design capacity, energy, voltages and chemistry on
the battery. The charger driver applies `constant-charge-voltage-max-microvolt`,
`constant-charge-current-max-microamp` (not set, the charger keeps 2 A) and
`charge-term-current-microamp`, and refuses writes above the battery's
maximums. The fuel gauge names the charger in `power-supplies` and takes
its status from it.

## Fuel gauge registers

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
above 4.30 V, and the driver clamps that to 100.

## Charger registers

8-bit registers, layout from Samsung's `sm5705_charger.c`. Values as the
bootloader left them, before the driver:

| Reg | Name | Content | Boot value |
|---|---|---|---|
| 0x00-0x03 | INT1-4 | interrupt flags, cleared by reading | |
| 0x04-0x07 | INTMSK1-4 | 1 = masked | 0xff 0x9b 0xff 0xff |
| 0x08 | STATUS1 | 0 VBUSPOK, 1 VBUSUVLO, 2 VBUSOVP, 3 VBUSLIMIT, 4-7 same for wireless | 0x21 |
| 0x09 | STATUS2 | 0 AICL, 1 BATOVP, 2 NOBAT, 3 CHGON, 4 Q4FULLON, 5 TOPOFF, 6 DONE, 7 WDTMROFF | 0x39 |
| 0x0a | STATUS3 | 0 THEMREG, 1 THEMSHDN, 2 OTGFAIL, 3 DISLIMIT, 4 PRETMROFF, 5 FASTTMROFF, 6 LOWBATT | 0x00 |
| 0x0c | CNTL | bits 2:0 operation mode (5 = charging on) | 0x05 |
| 0x0d | VBUSCNTL | bits 6:0 input limit, 100 mA + 25 mA steps | 0x38 = 1500 mA |
| 0x10 | CHGCNTL2 | bits 5:0 charge current, 100 mA + 50 mA steps | 0x26 = 2000 mA |
| 0x12 | CHGCNTL4 | bits 5:0 BATREG (0: 3.8 V, 1: 4.0 V, then 4.01 V + 10 mV steps), bit 6 AUTOSTOP | 0x67 = 4.38 V |
| 0x13 | CHGCNTL5 | bits 3:0 top-off current, 100 mA + 25 mA steps; 5:4 switching frequency | 0x04 = 200 mA |
| 0x15 | CHGCNTL7 | 7:6 AICL threshold (0 = 4.5 V), 5 AICL enable, 2 OVPSEL, 1 AUTOSET | 0x2e |
| 0x16 | CHGCNTL8 | 6:5 watchdog timer, 4:3 top-off timer (3 = 45 min) | 0x18 |
| 0x37 | DEVICEID | | 0xf9 |

After the driver: BATREG 0x1f (4.30 V), top-off 0x08 (300 mA), interrupt
masks 0xf8 0x95 0xcd 0xff (VBUS good/UVLO/OVP, battery OVP, charging,
top-off, done, thermal shutdown, safety timers).

Stock's init also sets a 45 min top-off timer (CHGCNTL8 0x16 bits 4:3),
AICL on at 4.5 V, OVPSEL on and AUTOSET off. The boot values match except
AUTOSET, which is on; the driver leaves all of them alone.

`STATUS` of the charger, and so of the battery: USB power absent ->
discharging; TOPOFF or DONE -> full; CHGON -> charging; otherwise not
charging. The input current limit is not changed: on the PC's USB port the
charger's AICL holds the input current below what the port can give
(STATUS2 bit 0 set).

## Battery model

Samsung's driver loads a battery model at boot (`battery_params`: OCV
tables, RCE, RS, current and temperature calibration) when FG_OP_STATUS bit
4 is clear. The gauge runs from the battery and keeps that model across
reboots and OS changes, so on this tablet it still reports bit 4 set and its
SOC matches the voltage. If the battery is ever disconnected, the gauge
loses the model; the driver then warns at probe
(`gauge lost its battery model`) and the percentage may be wrong until
Android has booted once, or until the init sequence is ported.

## Next

- SM5705 MUIC: cable type (USB port, charger, ...) and a matching input
  current limit for wall chargers. Mainline's `extcon-sm5502` handles the
  SM5502/SM5504/SM5703 MUICs and may cover it.
- Fuel gauge alert interrupt (PM8994 GPIO 1) instead of only polling.
- Port the gauge model init (`battery_params`) in case the gauge resets.
