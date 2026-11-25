# bq27220_full

ESPHome external component for the Texas Instruments BQ27220 single-cell fuel gauge (I2C, 7-bit address 0x55).
It exposes every value the gauge offers without unsealing it, where the `bq27220` component in ESPHome itself has eight sensors.
Tested on Arduino Nesso N1 with 250 mAh LiPo.
The datasheet was rather confusing for Claude, a lot of bugs were fixed via reading the Flipper Zero source code.

## What is new compared with the stock `bq27220` component

- **Values:** 16 more sensors, all 30 flags as binary sensors, and 5 text sensors, marked **new** below.
- **Configuration options:** `design_capacity`, `taper_current`, `reset_on_boot` and `average_power_scale`, and the action `bq27220_full.reset`.
  The stock component only reads.
- **Reading:** all standard commands are read in four transfers of consecutive registers per poll, so the values of one poll belong together, and `update_interval` has a floor of 500 ms.
  The stock component reads one register per sensor, and its default interval is 60 s.
- **Same as stock:** the registers and conversions of the eight stock sensors, the NaN for the gauge's 65535 "not applicable" value on `time_to_empty`, and the NaN when a read fails.
- **YAML shape:** a hub `bq27220_full:` with `sensor`, `binary_sensor` and `text_sensor` platforms that point to it with `bq27220_full_id`, where the stock component is a single `sensor` platform.

## What a battery indicator needs on the Nesso N1

The stock `bq27220` component reads the gauge correctly, but on the Nesso N1 it does not give a working indicator.
The reason is how the gauge is configured, not how it is read.

- **The gauge keeps the TI defaults.**
  Its design capacity is 3000 mAh and its taper current is 100 mA, and neither the Arduino core nor M5Unified programs them, they only read the gauge.
- **The level moves about 12 times too slowly.**
  The gauge counts charge against 3000 mAh, and the cell has 250 mAh.
  In a log with the defaults, `battery_level` stayed at 80% for four minutes of charging at 97 mA, while `remaining_capacity` rose by 1 mAh every 37 s and read 2382 mAh.
  All of the capacity based values describe a 3000 mAh pack: remaining and full capacity, time to empty and to full, and health.
- **The end of the charge is wrong too.**
  The taper current of 100 mA is above the 96 mA this board charges at, so the gauge called the battery full at 4.115 V, while the charger was still in constant current.
- **So the gauge has to be programmed.**
  The design capacity (with the learned full charge capacity) and the taper current go into the gauge's RAM, and have to be written again whenever the gauge loses power.
  That needs a reset, full access, the config update mode and a checksummed write, which the stock component cannot do, since it only reads.
  This is `design_capacity` and `taper_current` here, and nothing else in this component is needed for a basic indicator.
- **The charger has to be enabled.**
  The AW32001 starts with charging disabled, so something has to switch it on, `aw32001` with `charge_enabled: true` here.
  Without that, the indicator shows a battery that never charges while USB is connected.

What the indicator itself reads can come from the stock sensors: `battery_level`, `voltage` and `current`.
The additions of this component (average values, time to full, the flags) are for showing more, and are not needed to make the level right.
The gauge also learns the real capacity over full charge and discharge cycles, so the level can still be off at first.

A cheaper way without writing to the gauge is a percentage calculated from `voltage` with a `calibrate_linear` filter.
It is only good at rest, because the voltage rises under charge and sags under load.

## Configuration

```yaml
bq27220_full:
  id: gauge
  update_interval: 1s    # default 10s, at least 500ms
  design_capacity: 250mAh  # optional, see below
  taper_current: 30mA      # optional, see below
  reset_on_boot: false     # optional, resets the gauge once per boot, see below
  average_power_scale: 1   # optional, factor for average_power, see the open questions

sensor:
  - platform: bq27220_full
    bq27220_full_id: gauge
    voltage:
      name: Battery voltage
    # ... any of the sensors below

binary_sensor:
  - platform: bq27220_full
    bq27220_full_id: gauge
    dsg:
      name: Gauge DSG
    # ... any of the flags below

text_sensor:
  - platform: bq27220_full
    bq27220_full_id: gauge
    active_flags:
      name: Gauge flags
```

`update_interval` must be at least 500 ms.
SLUUBD4A 5.3 says the host should not read all standard commands more than twice per second, or the gauge can reset itself on a watchdog.
Every poll reads all standard commands once, in four incremental reads of consecutive registers, so the values of one poll belong together.

## Design capacity

The gauge ships with a design capacity of 3000 mAh.
The Nesso N1 does not come with any override values in the OTP, so its capacity based values (`battery_level`, `remaining_capacity`, `full_charge_capacity`, `time_to_empty`, `state_of_health`) provide wrong data.
To fix that, use the `design_capacity` parameter.
At boot the component reads `DesignCapacity()`, and if it differs from the configured value it rewrites the gauge's RAM settings.

SLUUBD4A chapter 6 is not a reliable guide for this, and the Flipper Zero firmware driver ([bq27220.c](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/lib/drivers/bq27220.c)), which works on real gauges, says why: "Datasheet and technical reference manual are full of bullshit".
The sequence and the timings below follow that driver.
The manual checksum is correctly described in the manual.

1. It reads the design capacity through `DesignCapacity()`, and the taper current from data memory `0x9201` outside the config update mode.
   Only if one of them differs from the configuration the rest happens.
   The words written are the design capacity at `0x929F`, the learned full charge capacity at `0x929D`, which the gauge starts from, and the taper current at `0x9201`, for whichever of the two options are set.
2. It resets the gauge and waits for `INITCOMP`, as the driver advises: "Don't forget to reset chip before writing new config".
3. It enters full access with the keys `0xFFFF`, `0xFFFF`, after the unseal keys `0x0414`, `0x3672` if the gauge is sealed.
   Two control writes in a row fail at about 2.5 ms apart, according to the Flipper Zero driver, which uses 5 ms, and so does this component.
   Before this was known the first pair of keys never worked, because both words were sent at once.
4. It enters CONFIG UPDATE mode by writing `0x0090` to `0x3E`, and waits for the flag.
5. For each parameter it writes the address and the new big-endian value in one write to `0x3E`, and then a checksum and a length in one write to `0x60`.
   The checksum is the complement of the sum of those four bytes, and the length is the data bytes plus four (SLUUBD4A 2.30 and the short example in 4.6).
   Nothing is read in between.
6. It exits with a re-initialisation through the control register, waits 2 s ("Must wait, or DM read returns garbage") and waits for the flag to clear.
7. It puts the security mode back: it seals the gauge, and unseals it again if it was unsealed before.
8. It polls `DesignCapacity()` for up to 3 s and logs whether the new value shows up.

The gauge keeps these settings in RAM, and the manual says they must be written again whenever the gauge loses power.
So the update runs only when the stored value differs, which in practice is after the battery was disconnected.
It blocks `setup()` for several seconds, once, and logs at `INFO`.

### Taper current

`taper_current` sets the gauge's `Taper Current` (data memory `0x9201`, default 100 mA, wrong on Nesso N1), with the same method and in the same session as the design capacity.
It is written whatever the gauge holds now.

The gauge detects a full charge when, in two 40 s windows in a row, the current is below the taper current and more than 0.25 mAh is accumulated, and the voltage is above the charging voltage minus the `Taper Voltage` (SLUUBD4A 4.4.1 and 4.9.47).
Then it sets `fc` and `tca`, and sets the remaining capacity to the full charge capacity.

The manual suggests C/10 to C/20, which is 12.5 to 25 mA for 250 mAh.
But 0.25 mAh per 40 s needs an average above 22.5 mA, so a taper current at or below that never qualifies.
The value of 30 mA is just above that limit and well below the charge current.
Whether it triggers at the end of charge depends on how fast the charger's current falls, and was not tested.
If `fc` never sets, raise it, and if it sets while the charger is still at a high current, lower it.

The action `bq27220_full.reset: gauge` sends the RESET subcommand.
The gauge restarts, its RAM settings return to the defaults, and it estimates the level again from its default open-circuit-voltage curve.
`reset_on_boot: true` does the same at the start of the component's setup, for a single flash, and logs a warning at every boot while it is set.
The option must be removed again, because the reset also throws away what the gauge learned.
A battery disconnect has the same effect.

The gauge learns the real capacity over full charge and discharge cycles, and the level can be off until then.
The charge termination detection has its own settings that are not touched.

## Sensors

The stock component has eight of these: `voltage`, `current`, `battery_level`, `temperature`, `remaining_capacity`, `full_charge_capacity`, `time_to_empty` and `state_of_health`.
Everything marked **new** in the last column is added by this component.


| Key | Register | Unit | Notes | Compared with stock `bq27220` |
|---|---|---|---|---|
| `voltage` | 0x08 | V | Voltage() | in stock |
| `current` | 0x0C | A | Current(), signed, negative while discharging | in stock |
| `average_current` | 0x14 | A | AverageCurrent(), signed | **new** |
| `standby_current` | 0x1A | A | StandbyCurrent(), signed, adaptive | **new** |
| `max_load_current` | 0x1E | A | MaxLoadCurrent(), signed, adaptive | **new** |
| `average_power` | 0x24 | W | AveragePower(), signed, negative while discharging, converted from the manual's mW | **new** |
| `temperature` | 0x06 | °C | Temperature(), 0.1 K steps; the source (internal, external or host) depends on the gauge configuration | in stock |
| `internal_temperature` | 0x28 | °C | InternalTemperature() | **new** |
| `battery_level` | 0x2C | % | RelativeStateOfCharge() | in stock |
| `state_of_health` | 0x2E | % | StateOfHealth(), full charge capacity over design capacity | in stock |
| `remaining_capacity` | 0x10 | mAh | RemainingCapacity() | in stock |
| `full_charge_capacity` | 0x12 | mAh | FullChargeCapacity() | in stock |
| `design_capacity` | 0x3C | mAh | DesignCapacity(), the value the gauge was configured with | **new** |
| `raw_coulomb_count` | 0x22 | mAh | RawCoulombCount(), signed: it counts up while discharging and down while charging, so it goes below 0 | **new** |
| `btp_discharge_set` | 0x34 | mAh | BTPDischargeSet(), battery trip point | **new** |
| `btp_charge_set` | 0x36 | mAh | BTPChargeSet(), battery trip point | **new** |
| `time_to_empty` | 0x16 | min | TimeToEmpty() | in stock |
| `time_to_full` | 0x18 | min | TimeToFull() | **new** |
| `standby_time_to_empty` | 0x1C | min | StandbyTimeToEmpty() | **new** |
| `max_load_time_to_empty` | 0x20 | min | MaxLoadTimeToEmpty() | **new** |
| `cycle_count` | 0x2A | | CycleCount() | **new** |
| `charge_voltage` | 0x30 | V | ChargingVoltage(), what the battery asks the charger for | **new** |
| `charge_current` | 0x32 | A | ChargingCurrent(), what the battery asks the charger for | **new** |
| `battery_id` | 0x00 | | CONTROL_STATUS()[BATT_ID2:0] | **new** |

The gauge reports 65535 when a value does not apply, for example a time to empty while charging, or a request for the maximum charge voltage.
The sensor then publishes NaN.
When a read fails, the affected sensors publish NaN and the component shows a warning.

## Flags

**All of the flags are new.**
The stock component has no binary sensors.

Each flag is a binary sensor named after the manual, in lower case.
Reserved bits are left out.
The GaugingStatus flags TC, TD, FC and FD are documented as identical to the BatteryStatus flags TCA, TDA, FC and FD, and its VDQ and EDV2 as copies of the OperationStatus ones, so they are not repeated.


| Word | Flags |
|---|---|
| CONTROL_STATUS (0x00) | `snooze`, `bca`, `cca` |
| BatteryStatus (0x0A) | `dsg`, `sysdwn`, `tda`, `battpres`, `auth_gd`, `ocvgd`, `tca`, `chginh`, `fc`, `otd`, `otc`, `sleep`, `ocvfail`, `ocvcomp`, `fd` |
| OperationStatus (0x3A) | `calmd`, `edv2`, `vdq`, `initcomp`, `smth`, `btpint`, `cfgupdate` |
| GaugingStatus (subcommand 0x0056) | `gs_dsg`, `cf`, `edv`, `edv1`, `fccx` |

BatteryStatus `dsg` is set only in discharge mode, while `gs_dsg` is also set in relaxation mode.
The bits `sysdwn`, `otd`, `otc`, `ocvfail` and `chginh` have the problem device class.
GaugingStatus is read through a subcommand, one extra transfer per poll, only when one of its flags or `active_flags` is configured.
If the gauge refuses it, those flags are not published and the component logs a warning once.
The sensors and the flags of a poll are published together, after the GaugingStatus answer when it is read, which is 10 ms after the registers.

## Text sensors

**All of the text sensors are new.**
The stock component has none.


| Key | Source |
|---|---|
| `device_number` | DEVICE_NUMBER subcommand, read once at boot, 0x0220 for the BQ27220 |
| `firmware_version` | FW_VERSION subcommand, read once at boot, the 11 bytes `ddDDvvVVbbBBTTzzZZRREE` as hex |
| `hardware_version` | HW_VERSION subcommand, read once at boot, 2 bytes as hex |
| `security_access` | OperationStatus()[SEC1:0]: `sealed`, `unsealed`, `full_access` or `reserved` |
| `active_flags` | the names of all set flags, upper case, or `none` |

At boot the component checks the device number and marks itself failed if it is not 0x0220.
The manual's DEVICE_NUMBER example shows 0x0320, but its description and the ESPHome component both use 0x0220.

## Not exposed

- AtRate() and AtRateTimeToEmpty() are an input and its result for host calculations, and read as 0 and 65535 unless the host writes an AtRate.
- AnalogCount(), RawCurrent(), RawVoltage() and RawIntTemp() at 0x79 to 0x7F are calibration data, and the manual says reads above 0x6B are not acknowledged.
- Data memory is written only for `design_capacity` (with the learned full charge capacity) and `taper_current`.
  Everything else keeps its default, including the CEDV profile, the open-circuit-voltage curve and the end-of-discharge voltages.

## Verified on the Nesso N1

- The device number is `0x0220`, and the firmware version bytes are `02 20 00 03 00 04 00 00 00 00 00`.
- The four block reads work at 1 s, with 14.8 ms per poll on average and 28.6 ms at most (ESPHome's runtime statistics).
- The GaugingStatus subcommand works while the gauge is unsealed, and the status words agree with each other: BatteryStatus `TDA` and GaugingStatus `TD`, which the manual calls identical, were set together.
- `raw_coulomb_count` is signed and counts down while charging (-1 to -37 mAh in the logs).
- The design capacity update works.
  The log shows `Design capacity set to 250 mAh, the gauge reports it after 0 ms`, and the gauge was back in the unsealed mode afterwards.
- `reset_on_boot` resets the gauge, and the component carries on after it.
- The 10 ms wait after a subcommand is enough for all the MAC reads.
  The Flipper Zero driver gets by with 1 ms, and the manual's 66 µs or 66 ms never mattered.

## Tests

The tests are in `tests/components/bq27220_full/`, laid out as in ESPHome itself.

How a poll is decoded is in `bq27220_regs.h`, standard C++ only and constexpr: the register blocks, which registers and status words a poll read, the conversions of the sensors, and the security mode.
The tests of it are `static_assert`s in `regs_test.cpp`, so a successful compilation means they all passed:

```sh
cd tests/components/bq27220_full
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/bq27220_full -fsyntax-only regs_test.cpp
```

`test.esp32-idf.yaml` is a build test with every option, every entity and the action, on two gauges.
It needs no hardware: `esphome compile tests/components/bq27220_full/test.esp32-idf.yaml`.

The driver allocates nothing on the heap while it runs.
The code generator writes one function per gauge that publishes the configured entities from what a poll read, so the component keeps no list of its entities.
The names of the flags for `active_flags` are a table in flash, made by the code generator only when `active_flags` is configured, and the list is built in a buffer of a fixed size.

## Open questions

- `average_power` reads a tenth of voltage times current, by a constant factor, from 6 mA to 140 mA:

  | Reported | Voltage | Current | Voltage times current |
  |---|---|---|---|
  | 0.038 W | 3.95 V | +97 mA | 0.383 W |
  | 0.040 W | 4.12 V | +97 mA | 0.399 W |
  | -0.055 W | about 4 V | about -140 mA | about -0.56 W |
  | 0.020 W, 0.20 W on the LCD with the scale of 10 | 4.20 V | +47 mA | 0.197 W |
  | 0.003 W, 0.03 W on the LCD with the scale of 10 | 4.20 V | +6 mA | 0.025 W |

  The last two rows are taken from photos of the LCD, so their raw values are the displayed ones divided by 10.
  The BQ27220 manual and datasheet document mW for this register and no scaling option, so the sensor follows them by default.
  Other sources point to units of 10 mW:
  the Linux `bq27xxx` driver, which reads this register for the whole TI gauge family and says "Other gauges return a signed value in units of 10mW",
  the BQ27545-G1, whose `Design Energy Scale` parameter switches AveragePower between mW and cW,
  and the BQ27220 manual's own glossary, which lists "cWh".
  The BQ27220 itself is not in the Linux driver, and neither M5Unified, the Flipper Zero driver nor the Arduino core for the Nesso N1 read or scale the register.
  `average_power_scale` multiplies the sensor by a factor, and 10 turns the readings above into the expected values.
  The cause is still not known.
- The taper current write has not been seen in a log.
  The photos show no `fc` or `tca` flag, neither at 45 mA nor at 6 mA, which is not proof of the 30 mA setting.
- Whether the gauge ever detects the end of a charge with a taper current of 30 mA is not known.
  It needs more than 0.25 mAh per 40 s window, so an average above 22.5 mA, and the current fell from 45 mA to 6 mA without `fc` being set.
  A higher taper current widens the window, and flags the battery full earlier.
- `temperature` and `internal_temperature` are the same value, so the gauge measures its own chip temperature and not the cell's.
  Whether the Nesso N1 wires a thermistor to the gauge is not known.
- Whether the settings written to the gauge survive a reset of the ESP has not been checked in a log.
  The manual says they are kept as long as the gauge has power, and the component writes them again if they differ.
- The first transfer to the gauge after a power-up failed once, and never again in the boots after it.
  The cause is not known, and the device number read is tried three times.
