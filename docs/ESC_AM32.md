# Configuring the ESC with AM32 firmware

The ESC on the stand runs AM32 with DShot forced. Below is what you need to
know and to turn on for the stand to work.

## The stand requests telemetry, so interval telemetry is not needed

The stand's firmware sets the telemetry-request bit in the DShot frame thirty
times a second, and only on channel 1. AM32 answers that — `Src/dshot.c`:

```c
if (calcCRC == checkCRC) {
    ...
    if (dpulse[11] == 1) {
        send_telemetry = 1;
    }
```

Here `dpulse[11]` is the twelfth bit of the frame, the request bit itself.

**Interval telemetry (`Telemetry on interval`) is better turned off.** It is
not needed and it actively gets in the way: on a 4-in-1 all the ESCs hang on
the same `TEL` contact, and when each one transmits on its own timer they
periodically fall into step and drown each other out. The picture measured on
the stand was that readings disappeared in bursts of up to 750 ms, which made
the plot jerky. With the request, exactly one ESC answers, and the dropouts
disappeared: the longest pause became 246 ms, and the "telemetry is stale"
flag never came up once.

If interval telemetry is on anyway, the stand will survive it — it discards
packets from the others, see the section on the shared line below.

It is worth saying this outright: this file used to claim the opposite, that
the request bit does nothing in AM32 and that telemetry only ever comes on an
interval. That was wrong. The assignment `send_telemetry = 1` occurs in the
AM32 sources **twice**: in `Src/main.c` on the timer and in `Src/dshot.c` on
the request bit. I overlooked the second place at the time because I only
searched in `main.c`.

All of this is checked with the `bytes` field in the stand's telemetry: if it
stays at zero while the ESC is powered, not a single byte is arriving on the
line.

### Two more conditions, this time on the firmware build side

Settings in the configurator are not enough: telemetry has to be compiled
into the firmware of the particular ESC. In `Src/main.c` the send is wrapped
in conditional compilation:

```c
if (send_telemetry) {
#ifdef USE_SERIAL_TELEMETRY
    makeTelemPackage((int8_t)degrees_celsius, battery_voltage, actual_current,
        (uint16_t)(consumed_current >> 16), e_rpm);
    send_telem_DMA(10);
    send_telemetry = 0;
#endif
}
```

The `USE_SERIAL_TELEMETRY` flag is set per target in `Inc/targets.h`. Most
targets have it, but if yours does not, `send_telemetry` will be set for
nothing and not a byte will go out on the line. Checked with the same `bytes`
field in the interface.

### Make sure the target flashed is the right one

AM32 has **two** targets for this ESC, on different microcontrollers, and
both declare `USE_SERIAL_TELEMETRY`:

| Target | Microcontroller | Name in the configurator |
|---|---|---|
| `VENDOR_F421` | AT32F421 | `VENDOR` |
| `VENDOR_F051` | STM32F051 | `VENDOR F0` |

The vendor token in both names has been replaced with `VENDOR` here;
upstream the names carry the vendor's own name.

Both were added to AM32 only in the autumn of 2025, and a mapping of board
revisions (v2.0 / v2.1 / v2.2) to the microcontroller exists nowhere. Open
the configurator and look at the firmware name: if it is anything other than
those two, someone else's target is flashed. This is not a trifle — on the
wrong target the `TLM` pad can lead to a pin that the firmware holds as an
ordinary output at zero. That failure is documented for AM32; in the tracker
it appears under numbers 172 and 240, and there they advise ringing out the
`TLM` pad to the programming pads.

The second flag is `NO_CURRENT_SENSE`. Targets that have it (for example
`GEPRC_12S_F421`) have no shunt, and `ADC_raw_current = 0` goes into the
packet. Telemetry works, rpm and voltage arrive, and the current is always
zero.

## Forced DShot instead of AUTO

On the stand the ESC has been switched from `AUTO` to forced **DShot**, and
the firmware comes up in `DSHOT300` at every power-on.

It is done that way because `AUTO` determines the protocol once at power-on
and remembers the choice. While the protocol could still be changed from the
interface, the two sides fell out of agreement easily: after a reboot the
board would come up in servo-PWM while the ESC still remembered DShot, and
the signal would stop being read without a single error message. With forced
DShot on the ESC and a fixed DSHOT300 on the board there is nothing left to
mismatch, so protocol selection has been removed from the interface.

## Packet format

10 bytes at 115200 8N1, with no start marker
(`Inc/kiss_telemetry.h`, compatible with the KISS standard and BLHeli_32):

| Byte | Contents |
|---|---|
| 0 | temperature, °C, signed |
| 1–2 | voltage, centivolts, high byte first |
| 3–4 | current, centiamps |
| 5–6 | consumption, mAh |
| 7–8 | eRPM ÷ 100 |
| 9 | CRC-8, polynomial 0x07, initial value 0 |

The stand's firmware finds the packet boundary from the pause on the line: a
packet takes 0.87 ms to transmit, and between packets there is about 30 ms of
silence. Every packet is additionally checked against its CRC; the error
counter is visible in the interface log.

## The current in the telemetry packet is the ESC's own measurement

It is worth understanding where the number comes from: AM32 digitises it with
its own ADC and converts it using constants baked into the build target.

```c
// Src/main.c, the branch for STM32 and AT32; on NXP the ADC is 16-bit and
// the divisor is different.
// The result is in units of 10 mA.
actual_current = ((smoothed_raw_current * 3300 / 41) - (CURRENT_OFFSET * 100))
                 / (MILLIVOLT_PER_AMP);
```

Which pin to read and how to convert are set by four target definitions in
`Inc/targets.h`: `CURRENT_ADC_PIN`, `CURRENT_ADC_CHANNEL`,
`MILLIVOLT_PER_AMP` and `CURRENT_OFFSET`. The scale differs between boards
from 4 to 67 mV/A, that is by a factor of seventeen.
If a close but wrong target is flashed, telemetry will keep arriving
correctly and the current will be wrong — and the rpm and the voltage will
stay right, which is what misleads you. Cross-check with a clamp meter.

That is how it turned out on this stand: the ESC always reports about
**143 A** flat, whether the motor stands still or spins. So the stand takes
current and voltage from its own calibrated sensors, and from telemetry only
the rpm, the temperature and the consumption. For the same reason the ESC's
reading has been removed from the current cutoff: it used to take the larger
of the two, and an enabled cutoff would have fired instantly, not letting the
motor start moving.

### Why the ESC reports 143 A: it is not about calibration

Both of this ESC's targets in AM32 are equally sparse. Here is the `*_F421`
one in full, nine lines added in August 2025 by an outside contributor:

```c
// The target name here is genericised; upstream it carries the vendor's name.
#ifdef VENDOR_F421
#define FIRMWARE_NAME "VENDOR     "
#define FILE_NAME "VENDOR_F421"
#define DEAD_TIME 100
#define HARDWARE_GROUP_AT_C
#define HARDWARE_GROUP_AT_045
#define USE_SERIAL_TELEMETRY
#endif
```

Missing here are not only `MILLIVOLT_PER_AMP` and `CURRENT_OFFSET` but also
**`CURRENT_ADC_PIN` and `CURRENT_ADC_CHANNEL`**. And that is no longer about
accuracy, it is about which pin of the microcontroller gets read. Without
them the family's fallback value is substituted, and for the AT32F421 it is
this:

```c
#ifndef CURRENT_ADC_PIN
#define CURRENT_ADC_PIN LL_GPIO_PIN_5     // PA5
#endif
```

Now look at what the same target declares through `HARDWARE_GROUP_AT_045`:

```c
#define PHASE_C_COMP 0x400000D5 // pa5
```

**PA5 is the phase-C comparator input.** The very pin on which the firmware
picks up the back-EMF and counts commutations. `ADC_Init()` in
`Mcu/f421/Src/ADC.c` dutifully puts `CURRENT_ADC_PIN` into analogue mode and
places channel 5 second in the conversion queue — that is, under the guise of
current AM32 digitises the voltage on the phase-C divider.

Hence the flat 143 A on a motor that is standing still: 143 × 20 mV/A =
2860 mV, and that is an entirely plausible voltage on a phase divider, but it
has nothing to do with current. No calibration fixes this. It is fixed only
by editing the target in `targets.h` and rebuilding the firmware, and for
that you need to know which pin the output of the current amplifier actually
sits on. The vendor does not publish the schematic.

The hypothesis is easy to test: if this really is the phase-C divider, the
reported "current" will move together with the battery voltage and twitch
while the motor turns, instead of following the load.

If your unit is flashed with the F051 variant of that target (firmware name
`VENDOR F0`), the picture is milder. There the default is PA6, and in the
`HARDWARE_GROUP_F0_C` group the comparators sit on PA4, PA5 and PA0, so there
is no collision. What is more, the same group is used by `WRAITH32V2_F051`,
which also does not override the pin, but does so with a calibrated 50 mV/A
and a 600 mV offset — meaning that on boards of this family PA6 is indeed the
output of the current amplifier. So on the F051 target the pin is most likely
right and only the scale is wrong: `MILLIVOLT_PER_AMP 20` remains a
placeholder, and the reading can be brought to the truth with a single
clamp-meter measurement.

The general background is just as cheerless. On the ArduPilot forum it is put
in one phrase: "having current in the telemetry of a 4-in-1 is actually a
rarity, very few of them give it out".

The flight controller's analogue current input (`CURR`, pin PC3) is a
completely separate circuit with no relation to the ESC firmware: what it
needs is the analogue output of the current sensor in the harness. It is an
independent second source, and in the calibration window the stand shows the
raw signal from this pin in millivolts — from which you can see at once
whether anything arrives there or not.

## The shared telemetry line on a 4-in-1

All four ESCs of the assembly sit on the same `TEL` contact, and the KISS
packet has not one field by which the sender could be recognised. If interval
telemetry is on for more than one of them, their packets arrive intermixed,
and both with a correct checksum, so there is nothing to tell them apart by
"from below".

On the stand this looked like a faulty sensor: every other time the rpm
dropped to zero and the plot came out dotted. We worked it out from the
consumption counter: two independent sequences were running in the stream,
21118 → 21158 → 21198 and 21184 → 21224 → 21264, both with a step of 40. Two
counters mean two ESCs.

The firmware discards such packets: while one ESC is reporting rotation, a
packet with zero rpm cannot be from it, because a motor does not stop and
start again in the 30 milliseconds between packets. The rule lives in
`firmware/include/esc_stream.h` and is covered by a native test. How many
packets have been discarded is shown in the line under the tiles.

The right solution is to **turn interval telemetry off completely** and leave
only the DShot request, which the stand sends to one ESC. Then there is
nothing to collide. If the interval is needed for some reason, AM32 can space
the ESCs out: a `telemetry_on_interval` value of 2 and above acts as an index
and shifts each one's interval slightly.

## Low-rpm protection: why throttle above some point has no effect

AM32 limits the duty cycle depending on the **measured rpm** — the slower the
motor turns, the less it is allowed. On by default (`Src/main.c`):

```c
char low_rpm_throttle_limit = 1;
...
duty_cycle_maximum = map(k_erpm, low_rpm_level, high_rpm_level,
                         throttle_max_at_low_rpm, throttle_max_at_high_rpm);
```

Here `k_erpm` is the rpm in thousands of eRPM, `throttle_max_at_low_rpm`
equals 400 out of 2000, that is 20 % duty cycle, and
`throttle_max_at_high_rpm` is the full 2000. The boundaries are computed from
the KV and the magnet count declared in the configurator:

```c
rpm_level_div  = 32 / eepromBuffer.motor_poles;
low_rpm_level  = motor_kv / 100 / rpm_level_div;
high_rpm_level = motor_kv / 12  / rpm_level_div;
```

For our settings (KV 2220, 14 magnets) this gives `rpm_level_div = 2`,
`low_rpm_level = 11`, `high_rpm_level = 92`. In mechanical rpm, with seven
pole pairs:

| Shaft rpm | Permitted duty cycle |
|---|---|
| 1 570 rpm and below | 20 % |
| 6 400 rpm | about 53 % |
| 13 140 rpm and above | 100 % |

**This is a feedback loop, and it latches.** A light propeller spins up, the
rpm raises the ceiling, the ceiling gives more rpm, and so on to the very top
of the range. A heavy propeller turns slowly, the ceiling stays low, and
above the equilibrium point the throttle stops changing anything: the command
grows, the duty cycle stands still. From the outside this looks like a broken
ESC.

The same thing has a non-obvious consequence: **a large propeller can give
less thrust at full throttle than a small one**, even though at half throttle
it pulls harder. At low throttle it wins on size and pitch, and at the top it
runs into the ceiling and never reaches its own rpm.

How this differs from running out of power: at the duty-cycle ceiling **both
the rpm and the current** stop at the same time. If the rpm stands still
while the current keeps growing, the cause is not this limiter but the power
supply, overheating or desync.

There is only one way to turn the limiter off, and it is not obvious:

```c
if (motor_kv <= 20) {
    low_rpm_throttle_limit = 0;
}
```

And `motor_kv` is computed as `eepromBuffer.motor_kv * 40 + 20`, so the
condition holds only when the stored value is zero — **set the Motor KV field
to its smallest value, 20**. There is no separate checkbox for this
protection in the configurator.

And do enter the **real KV and magnet count** if you want the limiter: the
boundaries are computed straight from them, and an inflated KV shifts the
ceiling upwards, so the protection starts getting in the way at perfectly
usable rpm.

## Rpm: electrical versus mechanical

The packet carries **electrical** rpm. Mechanical rpm comes from dividing by
the number of pole pairs:

```
rpm = eRPM / (magnet count / 2)
```

The magnet count is set in the interface settings; the default is 14, which
is a typical motor with 14 magnets and 12 stator teeth (a marking like 2207).
If you enter the wrong number, the rpm will be proportionally wrong while the
current and the voltage stay right.

## Changing the protocol requires a power cycle

AM32 determines the protocol once and remembers it until a reboot. In
`Src/signal.c`:

```c
void checkServo(void) {
    if (smallestnumber > 200 && smallestnumber < 20000) {
        servoPwm = 1;
        ...
        inputSet = 1;     // detectInput() is not called again after this
    }
}
```

After switching the protocol in the interface, **power the ESC down and back
up**. Otherwise it will keep waiting for a signal in the old format and the
motor will not start. The interface reminds you of this.

## Why Oneshot125 and Multishot are not in the list

Protocol detection looks at the smallest interval between edges
(`detectInput()` in `Src/signal.c`):

| Smallest interval | Average duration | Conclusion |
|---|---|---|
| 1–3 | < 60 | DShot600 |
| 4–8 | < 100 | DShot300 or DShot150 |
| 200–20000 µs | — | servo-PWM |

Oneshot125 gives pulses of 125–250 µs. They are shorter than the 200 µs
threshold for servo-PWM and longer than the DShot range, so in `AUTO` mode
they are not recognised at all. Multishot, with its 5–25 µs, falls into the
gaps between ranges unpredictably. Offering them in the interface would mean
offering a mode known not to work, so the list has only servo-PWM and the
three DShot speeds.

For the same reason the servo-PWM frequencies are capped at 400 Hz: at 490 Hz
and full throttle the pause between pulses collapses to 40 µs and leaves the
detection window. The test `pwm400 gap at full throttle stays detectable` in
`firmware/test/` checks that all the offered frequencies stay within
tolerance.

## Stick calibration is worth turning off

In servo-PWM mode AM32 can enter a range calibration procedure if it sees
high throttle held at startup (`Src/signal.c`, the `enter_calibration_count`
counter). On the stand that is a surprise nobody needs: instead of spinning
up, the motor will start beeping and rewriting its thresholds.

Turn on **Disable stick calibration** in the configurator. In DShot mode it
does not apply.

## Recommended settings for the stand

| Setting | Value | Why |
|---|---|---|
| Telemetry on interval | **off** | the stand requests telemetry itself; leaving it on disturbs the neighbouring ESCs on the shared line |
| Disable stick calibration | on | so as not to run into calibration on PWM |
| Motor KV | the motor's real KV, or 20 | the duty-cycle ceiling by rpm is computed from it; 20 turns this protection off completely |
| Motor poles | the real magnet count | also goes into the ceiling calculation, and into the rpm conversion |
| Input type | **DSHOT300** | forced, not AUTO: the board comes up in DShot on every power-up, so leaving the ESC to guess only creates a way for the two sides to disagree |
| Brake on stop | to taste | stops the propeller faster after the throttle is cut |
| Sinusoidal startup | off | not needed for aircraft motors |
