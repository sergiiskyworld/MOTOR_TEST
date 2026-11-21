# Wiring

## Why channels 1 and 3

In the board description (ArduPilot's `hwdef.dat` for this board) the motor
outputs are declared like this:

```
PA3 TIM2_CH4 TIM2 PWM(1) GPIO(50) BIDIR   # M1
PB0 TIM3_CH3 TIM3 PWM(2) GPIO(51)         # M2
PB1 TIM3_CH4 TIM3 PWM(3) GPIO(52) BIDIR   # M3
PA2 TIM2_CH3 TIM2 PWM(4) GPIO(53)         # M4
```

M1 and M3 are the only pair of outputs marked `BIDIR`, that is, routed for
bidirectional DShot. They are also the two outputs used on the stand. A lucky
coincidence: if rpm ever has to be read straight off the signal wire, the
soldering will not have to change.

## Wires

This board has no separate pads for motors and telemetry: everything goes
through an **8-pin JST-GH connector** labelled `ESC`. Its layout, taken from
the official board schematic in the ArduPilot tree:

```
CURR · TEL · MOT4 · MOT3 · MOT2 · MOT1 · VBAT · GND
```

| `ESC` connector pin | Signal on the MCU | What for |
|---|---|---|
| `MOT1` | PA3 (TIM2_CH4) | signal to channel 1 — the one we use |
| `MOT3` | PB1 (TIM3_CH4) | signal to channel 3 — the spare |
| `TEL` | PD2 (UART5_RX) | AM32 telemetry |
| `VBAT` | PC1 (ADC1) | battery voltage |
| `CURR` | PC3 (ADC1) | current |
| `GND` | — | common ground, mandatory |

The practical consequence: if the stock harness of the 4-in-1 ESC is plugged
into the `ESC` connector, then telemetry, voltage and current all arrive
through **the same connector**. So if the interface shows voltage but no
telemetry, the cause is almost certainly not the wire to the flight
controller, but the AM32 settings, or the fact that on the ESC side the `TLM`
pad is not wired into the harness.

Ground is mandatory. Without a common ground the signal to the ESC has no
reference: the motor will either not start or will twitch unpredictably.

### Probing the telemetry line

Zero bytes received explains nothing by itself, so at startup the firmware
reads PD2 as an ordinary input, first with the internal pull-up and then with
the pull-down, and shows the result in a line next to the log. The pull is
weak, on the order of 40 kΩ, about 80 µA, and loses to anything that actually
drives the line. The pair of levels says what is on the other end:

| up / down | What it means |
|---|---|
| 1 / 0 | nothing is connected to the pin, it only follows the pull |
| 0 / 0 | something external is holding the line at zero |
| 1 / 1 | the transmitter is holding the line high, that is, the UART is connected and silent |

The `0 / 0` case has already happened on this stand, and the culprit turned
out to be a **faulty ESC**: the line sat at zero without a single edge, while
the motors ran normally. Replacing the ESC cured the telemetry immediately.
It is worth knowing that the flight controller pin can be checked separately
— unplug the ESC harness and restart the board: a healthy pin gives `1 / 0`.

There is one trap in this probe. An unpowered ESC also gives `0 / 0`: the
protection diodes of its chip pull the line to the dead supply rail, and
about 0.6 V is left on the pin, which for an STM32 input is a zero. So the
probe can only be judged with the ESC **powered**.

There is no documentation for the ESC itself: no pinout, no schematic, no
description of the `TLM` pad. The connector layout above was reconstructed
from the flight controller schematics in the ArduPilot tree, not from the
manufacturer's papers.

The register configuration of the pin has nothing to do with this: on the
stand it matches ArduPilot one for one — alternate function 8, internal
pull-up, 115200 8N1, no inversion. The STM32F405 cannot invert anyway: the
`RXINV`/`TXINV`/`SWAP` bits appeared only in later USARTs, and on F4
ArduPilot does inversion exclusively with an external chip, which this board
does not have.

## Cross-checking against the board documentation

The data is taken from `hwdef.dat`, not from the board README: the README
disagrees with it in two places, and `hwdef` turns out to be right.

| What | Vendor README | `hwdef.dat` (correct) |
|---|---|---|
| Voltage pin | `BATT_VOLT_PIN 13` | `HAL_BATT_VOLT_PIN 11`, this is PC1 |
| Current pin | `BATT_CURR_PIN 12` | `HAL_BATT_CURR_PIN 13`, this is PC3 |
| Bidirectional DShot | "channels 1–4" | only `PWM(1)` and `PWM(3)`, that is M1 and M3 |

The stand firmware follows `hwdef`.

## Power

The safest arrangement is to split the supply:

- the flight controller is powered from the computer's USB;
- the ESC is powered from the battery;
- the grounds are tied together (that same GND link above).

That way, when the battery is disconnected the flight controller stays alive,
the interface does not break off, and the computer's USB port does not carry
the motor current.

If instead you power the flight controller from the ESC's BEC, take care not
to plug in USB and BEC at the same time without isolation: that connects the
computer's 5 V to the ESC's 5 V. Most boards survive it, but it is better not
to find out.

## Onboard voltage and current sensing

The board can measure voltage and current itself, independently of the ESC:

| Signal | Pin | `ESC` connector pin | Default coefficient |
|---|---|---|---|
| Battery voltage | PC1 | `VBAT` | 8.3 V per volt |
| Current | PC3 | `CURR` | 92.6 A/V |

The coefficients come from the ArduPilot description of this board and are
adjustable in the interface.

These measurements are useful as a cross-check on the ESC data: if two
independent circuits disagree, it is worth sorting that out before plotting
thrust curves.

### The measured current-sensor characteristic

The documented 92.6 A/V from ArduPilot **is wrong for our combination** — it
was written for a different ESC. The characteristic measured against an
external ammeter:

```
signal on PC3 [mV] = 24.0 * current [A] - 10.6
```

Six points in the 0.4–1.2 A range fell on this line with a deviation of no
more than 0.026 A at an ADC resolution of 0.034 A, so the sensor is linear
and has no curvature in it. Hence `currentScaleM = 41684` (41.7 A/V) and
`currentOffsetMv = -11`.

An indirect confirmation: 41.7 A/V means a full scale of 138 A. The ESC is
rated for 70 A per motor and 80 A for ten seconds, so a scale of "up to 138 A
on the bus" is exactly what the designer would have chosen. And 92.6 A/V
would give 305 A, which this hardware will not deliver under any
circumstances.

Where the 92.6 came from has also been established. The board's `hwdef.dat`
carries the comment "thanks to betaflight for pin information", and the
vendor's older F7 flight controller target in Betaflight has
`DEFAULT_CURRENT_METER_SCALE 108`, which in Betaflight units means 10.8 mV/A,
that is exactly 1000/10.8 = 92.59 A/V. The value was carried over from **a
different, older flight controller** by the same maker, and nobody ever
checked it against this hardware. That it really is a different board is
visible in the same place: that older target declares the current input as
`ADC_CURR_PIN PC2`, while ours is `PC3`. Copied without looking.
Meanwhile the board's own README claims that the sensor reads up to 130 A,
and 130 A at an ADC ceiling of 3.3 V is 25 mV/A, about 39 A/V. The README and
`hwdef` contradict each other, and the README agrees with our measurement,
not with 92.6.

Two consequences worth knowing about.

Current calibration in the interface is done from **a single reading of a
reference ammeter**, as in ArduPilot: give the motor a working current, type
in what the instrument showed, and the stand recalculates "amps per volt"
from the raw signal minus the channel zero. The larger the current, the more
accurate the result — at low current the division is by a few ADC steps.

About the zero: on this sensor it is negative, and the "Zero" button will not
remove it. The sensor starts responding at roughly 0.44 A, and below that it
rests against the ADC zero, so at idle it shows zero rather than minus eleven
millivolts. At working currents those eleven millivolts are fractions of a
percent, and the single-point calibration absorbs them.

**Below roughly 0.45 A the current is not measured.** The reading will stick
at half an amp. For thrust measurements this does not matter; working
currents are an order of magnitude higher.

About resolution: the step of the 12-bit ADC is 0.8 mV, which at 41.7 A/V is
**34 mA per count**. Calibrating at half an amp is pointless; take several
amps, better a dozen.

The signal from both pins in millivolts is visible in the calibration window
— the fastest way to tell "no current" from "sensor not connected".

The board has no current sensor of its own — the ArduPilot wiki page for this
board says so outright: the board measures voltage itself, while the signal
on the `CURR` pin has to arrive from outside, from the ESC. So a healthy
voltage says nothing about the current: these are two independent circuits,
and the second one may not be connected.

At zero current this input should sit at **exactly zero volts**: ArduPilot
does not set `BATT_AMP_OFFSET` for this board, so it stays zero by default.
This is a ground-referenced shunt amplifier, not a Hall sensor biased to
mid-scale. Zero millivolts with the motor standing still is normal, not a
fault.

### What is actually on this wire

The third pin of the eight-pin harness is `Current` under the Betaflight
connector standard, which this ESC and every other 4-in-1 follow. The order
there is `V+ (VBAT)`, `GND`, `Current`, `Telemetry`, `Signal 1…4`. No other
definition of this pin exists anywhere, and our harness conforms to it.

What the ESC puts on that pin, its maker publishes nowhere: not in the shop,
not on the Enterprise page, not in a separate manual — only "70 A continuous
per motor, 80 A for ten seconds, AM32 preinstalled" and a cable for direct
connection to an F405. There is no schematic, no millivolts per amp, not even
the words "current sensor" in the description. So the coefficient here can
only be measured, which is what we did.

From the measurement, though, the design reads unambiguously. The
characteristic is linear from zero itself and rests against the ADC zero at
low current — which means this is a **ground-referenced shunt amplifier**,
not a Hall sensor, whose zero would sit at mid-scale. 24 mV/A is, say, a
0.5 mΩ shunt on an amplifier with a gain of 50, an entirely ordinary
combination.

An important consequence: there is **one shunt, in the common battery path**,
not one per motor. This wire always carries the full bus current — for a
stand with one motor that is the motor current plus the ESC's and the BEC's
own consumption, tens of milliamps. With all four running you get the sum,
not a share.

The second consequence is more pleasant: this wire lives **entirely in
hardware** and does not touch the ESC firmware. Reflashing AM32, changing the
target, any checkbox in the configurator — none of that changes what arrives
on `CURR`. Our 41.7 A/V calibration is tied to the board and the amplifier,
not to a firmware version, and survives everything except replacing the ESC
itself. Another unit, by the way, is worth recalibrating: the spread of
shunts and amplifiers is a few percent.

The firmware sets the ADC sample time explicitly (`ADC_SAMPLINGTIME` in
`firmware/platformio.ini`): the Arduino core takes 15 cycles by default,
while ArduPilot on this same board takes 480 on all channels. The inputs sit
behind a resistor with a capacitor, and in 0.7 µs the sampling capacitance
does not have time to charge, which makes weak signals read low.

## Checks before the first run

1. Remove the propeller from the motor. The first checks are done with a bare
   shaft only.
2. Plug in USB and open the interface. A green dot and the firmware version
   should light up in the header.
3. Power the ESC. Voltage and temperature should appear in the telemetry
   card, rpm at zero. If everything is dashes, the ESC is not sending
   telemetry — see [ESC_AM32.md](ESC_AM32.md).
4. Press "Beep motor" (DShot mode only). The motor should beep — this
   confirms that the signal wire arrived where it should.
