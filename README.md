# WELL RECORD — Cable Metering System for Artesian Well Inspection

Firmware for Arduino Uno that measures, in real time, the amount of cable lowered into an artesian well during camera inspection. Readings come from an optical encoder coupled to a cable reel, displayed on a 4x20 I2C LCD, and streamed over serial for integration with an external (Python) application.

## Overview

- Counts pulses from a quadrature rotary encoder (4x) coupled to a cable reel
- Converts pulses into actual cable length (meters + centimeters), based on the reel diameter
- LCD shows: fixed title, current cable length, and uptime (HH:MM:SS)
- Counter can be reset via physical button or remote serial command
- Streams data continuously over serial (every 200ms) for consumption by an external app

## Required hardware

| Component | Specification |
|---|---|
| Microcontroller | Arduino Uno R3 |
| Encoder | Optical E38S6G5-360B-G24N, 360 PPR |
| Display | 20x4 LCD with I2C serial module (PCF8574 backpack) |
| Button | Simple 2-pin push-button |
| Resistor | ~10kΩ (external pull-down for the button) |
| Reel | 6.5cm diameter (coupled 1:1 to the encoder shaft) |

## Wiring

### 4x20 LCD (I2C module)

| I2C module | Arduino |
|---|---|
| SDA | A4 |
| SCL | A5 |
| VCC | 5V |
| GND | GND |

> The I2C module is soldered to the back of the LCD, so only these 4 wires go to the Arduino. Contrast is adjusted with the small trimmer on the module. The default I2C address is `0x27` (some modules use `0x3F`).

### E38S6G5-360B-G24N encoder

| Encoder | Arduino |
|---|---|
| A | Pin 8 |
| B | Pin 9 |
| + | 5V *(check the supply voltage for your model)* |
| GND | GND |

### Reset button

| Button | Arduino |
|---|---|
| Signal | Pin 7 |
| GND | GND (via ~10kΩ pull-down resistor) |

> Active-high signal (HIGH = pressed).

## Why Pin Change Interrupt (PCINT)?

On the Arduino Uno, only pins **2 and 3** support native external interrupts (`attachInterrupt`). The encoder (pins 8/9) and the button (pin 7) are wired to other pins, so they rely on **Pin Change Interrupts (PCINT)**:

- Encoder → `PCINT0` group (pins 8-13)
- Button → `PCINT2` group (pins 0-7)

The encoder uses full quadrature decoding (4x), resulting in **1440 pulses per revolution** (360 PPR × 4).

## Cable length calculation

```
Reel circumference = π × diameter (6.5cm) ≈ 20.4203 cm/revolution
Resolution = Circumference / (360 × 4) ≈ 0.0142 cm/pulse (~0.142mm)
```

The value is displayed already converted to meters + centimeters (e.g. `3m 45.2cm`), accounting for direction of rotation (cable going down/coming up).

## Display layout

```
* WELL RECORD *
                    (free line)
3m 45.2cm
            00:12:34
```

- Line 1: fixed title (set by the `display_fixed_msg` constant)
- Line 3: current cable length (meters + centimeters)
- Line 4: uptime, right-aligned (HH:MM:SS)

## Serial protocol (for Python app integration)

**Baud rate: 115200**

| Direction | Message | Frequency |
|---|---|---|
| Arduino → PC | `CM:<total_cm_value>;PULSES:<pulses>;TIME:<HH:MM:SS>` | Every 200ms (continuous) |
| PC → Arduino | `RESET` | On demand |
| Arduino → PC | `OK:RESET` | Response to the `RESET` command |

Example line received:
```
CM:345.20;PULSES:7912;TIME:00:12:34
```

The Python app can read the serial port continuously and convert `CM` to meters/centimeters as needed, and send `RESET\n` at any time to reset the counter remotely.

## Flashing the firmware

1. In the Arduino IDE, install the **LiquidCrystal I2C** library (by Frank de Brabander) via Tools > Manage Libraries
2. Open the `.ino` file
3. Select **Arduino Uno** under Tools > Board
4. Select the corresponding serial port
5. Upload (Sketch > Upload)
6. Open the Serial Monitor at **115200 baud** to validate the readings

## Technical notes / troubleshooting

- **Blank LCD (backlight on, no text)**: first turn the contrast trimmer on the I2C module with a small screwdriver. If still blank, the I2C address is probably `0x3F` instead of `0x27` — change `lcdI2cAddress` in the code, or run an I2C scanner sketch to find the real address.
- **LCD not responding at all**: check that SDA is on A4 and SCL on A5 (not swapped).
- **Decimal number formatting on AVR**: `sprintf`/`snprintf` with `%f` are not reliable on the Arduino Uno (the AVR libc does not include floating-point support by default). The firmware uses `dtostrf()` for this conversion instead.
- **Calibration**: if the actual reel diameter differs from 6.5cm, adjust the `reelDiameterCm` constant at the top of the code.
- **Reversed direction**: if forward/backward feels swapped, invert the increment/decrement cases inside `ISR(PCINT0_vect)`.

## Possible next steps

- Python test script to validate the serial communication (`pyserial`)
- Persisting the cable length across power loss
- Sending meters/centimeters already split apart over serial (currently the app must compute this from the total cm value)

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
