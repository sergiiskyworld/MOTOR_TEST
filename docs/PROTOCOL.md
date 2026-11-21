# Board communication protocol

The channel is USB CDC (a virtual COM port), the baud rate does not matter.
Text commands go up, JSON lines come down. Everything is line-based, with `\n`
as the separator.

All numbers in the JSON are integers in fixed units: millivolts, milliamps,
per mille. That spares the firmware from formatting floating-point numbers and
keeps the stream readable in an ordinary terminal.

## Commands to the board

| Command | Action |
|---|---|
| `?` | identify itself and report the current settings |
| `K` | watchdog heartbeat |
| `A` | enable the throttle (arm) |
| `D` | remove the throttle (disarm) |
| `X` | emergency stop, latching the fault |
| `R` | clear the fault |
| `T <motor> <per mille>` | set the throttle, motor is `1` or `3`, throttle is 0…1000 |
| `P <protocol>` | change the protocol: `PWM50`, `PWM100`, `PWM200`, `PWM400`, `DSHOT150`, `DSHOT300`, `DSHOT600` |
| `S <key> <value>` | change a setting |
| `W` | write the settings into the board's memory |
| `C <motor> <command> <repeats>` | DShot special command (0…47), only while disarmed |
| `Z` | reboot into DFU |

Any command that is received, `K` included, resets the watchdog.

### Setting keys

| Key | Unit | Default |
|---|---|---|
| `maxThrottle` | per mille | 1000 |
| `rampPerS` | per mille per second | 500 |
| `motorPoles` | magnets in the motor | 14 |
| `vbatScaleM` | thousandths of a volt per volt | 8300 |
| `currentScaleM` | thousandths of an amp per volt | 92600 |
| `currentOffsetMv` | mV at zero current | 0 |
| `currentLimitMa` | mA, 0 disables the cutoff | 0 |
| `tempLimitC` | °C, 0 disables the cutoff | 0 |

## Replies from the board

### Identification, in reply to `?`

```json
{"hello":"thrust-stand-fc","version":"1.0.0","board":"F405",
 "motors":[1,3],"protocols":["PWM50","PWM100","PWM200","PWM400",
 "DSHOT150","DSHOT300","DSHOT600"],"controlHz":2000,"linkTimeoutMs":300}
```

### Settings, after `?` and after every `S`

The `saved` field says whether what the board is using right now matches what
is written in its flash. While it is `false`, the settings live only until the
power goes off.

```json
{"settings":{"maxThrottle":1000,"rampPerS":500,"motorPoles":14,
 "vbatScaleM":8300,"currentScaleM":92600,"currentOffsetMv":0,
 "currentLimitMa":0,"tempLimitC":0,"saved":true}}
```

### Command acknowledgement

```json
{"ack":"arm","ok":false,"detail":"throttle must be zero"}
```

The `detail` field appears only when the command is refused.

### Writing the settings, in reply to `W`

```json
{"save":"deferred"}
```

| Value | What happened |
|---|---|
| `unchanged` | memory already holds exactly this, there is nothing to write |
| `queued` | the write starts right away |
| `deferred` | the throttle is on, the write waits for the motor to stop |
| `done` | written and verified by reading back |
| `failed` | the flash did not accept the write |

Erasing a flash sector stalls the processor for about a second: instructions
are fetched from that same memory, so interrupts stop working as well and the
outputs hold their last value. That is why the board never writes settings on
the move; it waits for the throttle to be off and the outputs at zero. After
the write it sends `{"settings":…}` again so that the host sees `"saved":true`.

The settings live in the last flash sector (128 KB at address `0x080E0000`).
The firmware takes about 42 KB at the beginning, and `dfu-util` writes exactly
as many bytes as the image contains, so reflashing does not erase the
calibration.

### Telemetry, 50 times per second

```json
{"t":128400,"state":1,"faults":0,"proto":"DSHOT600",
 "thr":[400,0],"out":[848,0],
 "esc":{"ok":1,"ageMs":14,"tempC":42,"mv":24120,"ma":31500,
        "mah":420,"erpm":24500,"rpm":3500,"pkts":4231,"crcErr":0},
 "board":{"mv":24050,"ma":31100,"vPinMv":2988,"iPinMv":376},
 "load":{"raw":167616}}
```

| Field | Meaning |
|---|---|
| `t` | milliseconds since the board powered up |
| `state` | 0 is safe, 1 is throttle enabled, 2 is fault |
| `faults` | bit mask, see below |
| `thr` | the actual throttle in per mille after the slew-rate limit, for M1 and M3 |
| `out` | what actually went out on the wire: microseconds for PWM, 0…2047 for DShot |
| `esc.ok` | a telemetry packet with a correct CRC arrived recently |
| `esc.ageMs` | how long it has been since the last packet |
| `esc.erpm` | the eRPM as the ESC reported it |
| `esc.rpm` | the mechanical rpm, derived through `motorPoles` |
| `board` | independent measurements from the board's own sensors |
| `board.vPinMv`, `board.iPinMv` | what is on the ADC pins before the scales are applied |
| `load.raw` | raw counts from the thrust sensor, with no conversion to grams |

The raw millivolts are what calibration needs. The converted current is
clipped at zero from below (`dMv < 0 → 0`), so if the sensor zero is set above
its real signal, the reading stays at zero for any current and the scale
cannot be recovered from it; `iPinMv`, on the other hand, shows both the size
of the signal and the fact that it is below the zero.

The firmware does not send the `load` block yet: the thrust sensor is not
connected. The server survives that, thrust is shown as a dash. When the block
does appear, the conversion to grams need not be done on the board: the server
keeps the zero and the scale itself, and they are set in the calibration
window (the gear icon in the page header). So what is wanted from the firmware
is raw counts, with no arithmetic of its own.

### Fault bits

| Bit | Cause |
|---|---|
| 1 | the link to the computer was lost |
| 2 | the emergency stop was pressed |
| 4 | the configured current was exceeded |
| 8 | ESC overheating |

The fault latches: until an `R` arrives, the `A` command is refused.

## Rules the firmware keeps on its own

- The throttle can be enabled only when the commanded throttle is zero and
  there are no faults.
- While disarmed the output is always at rest: a 1000 µs pulse for PWM, a
  frame with value 0 for DShot. That is what the ESC needs in order to
  consider itself ready, and what keeps the motor from turning.
- The throttle does not change in a step but at a rate no higher than
  `rampPerS`. The emergency stop and the cutoffs are the exception, they tear
  the throttle away instantly.
- Changing the protocol first removes the throttle and resets the state, and
  only then reconfigures the timers.
- DShot special commands are accepted only while disarmed.
