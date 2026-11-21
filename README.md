# Motor thrust test stand

Motor rpm control on a test stand: the **F405 flight controller** works as a
programmable signal generator for the ESC, while throttle control and
telemetry live in the browser.

The flight controller replaces a servo tester. Unlike a servo tester it speaks
DShot, reads telemetry from the ESC (rpm, current, voltage, temperature) and
stops the motor by itself if the link to the computer is lost.

```
browser  ──WebSocket──►  Python server  ──USB CDC──►  flight controller  ──PWM/DShot──►  ESC (AM32)
                                                              ▲                               │
                                                              └────────── telemetry ──────────┘
```

## What is already done

| Component | Where | State |
|---|---|---|
| Flight controller firmware | `firmware/` | builds, 44 KB of flash |
| Server and web interface | `Prog/server/` | 44 end-to-end checks pass |
| Native firmware tests | `firmware/test/` | 62 checks pass |
| Peak values in the interface | `Prog/server/tests/peaks_test.js` | 12 checks pass |

All of this has been run on real hardware: the motor spins, telemetry is
recorded, the current sensor is characterised against a reference ammeter and
a faulty ESC has been diagnosed and replaced — see
[First power-up](docs/SAFETY.md#first-power-up).

## Quick start

### 1. Flash the flight controller

```bash
./scripts/flash.sh
```

The script builds the firmware, waits for the board to appear in DFU, takes a
backup copy of the current flash contents into `firmware/backup/` and only
then writes the new one.

To enter DFU: hold the **BOOT** button on the board and, without releasing
it, plug in USB. There is no software way to do it — checked on hardware; the
details and what exactly was tried are in
[docs/FLASHING.md](docs/FLASHING.md).

> The firmware is written from the very start of flash, so it overwrites
> whatever was there before (Betaflight or ArduPilot) together with that
> firmware's own bootloader. It is restored from the backup copy over the
> same DFU.

### 2. Connect the wires

Three wires to the ESC at minimum, plus one for telemetry —
[docs/WIRING.md](docs/WIRING.md).

### 3. Set up the ESC

Interval telemetry must be turned off on every ESC. The stand asks for
telemetry itself, with the telemetry-request bit in the DShot frame, 30 times
a second. A 4-in-1 wires all four ESCs to one telemetry pad and the KISS
packet carries no sender ID, so two ESCs talking at once interleave into an
unreadable mess — [docs/ESC_AM32.md](docs/ESC_AM32.md).

### 4. Start the interface

Double-click **`Prog/START-MAC.command`** on macOS or
**`Prog/START-WINDOWS.bat`** on Windows — and that is all: on the first run a
Python environment is created, the server comes up and a separate browser
window opens full screen, without tabs or an address bar. The details, the
layout of the folder and the fixes for common problems are in
[docs/LAUNCH.md](docs/LAUNCH.md).

Everything the application needs lies inside `Prog/`: the server, the
environment and the launch files. Only the firmware, the flashing script and
the documentation are kept outside.

The server listens on all interfaces, so from a phone on the same network the
page is available at the computer's IP. On the first run macOS will ask for
permission for incoming connections — that is normal.

The USB port is held only while the stand window is open in the browser, and
is released three seconds after the last window closes — or after the page
has simply gone quiet, which covers a crashed browser and a phone that went
to sleep. The AM32 and ArduPilot configurators therefore stay usable; why it
was done this way and what it cost to find that out is described in
[docs/PORT_SHARING.md](docs/PORT_SHARING.md).

The server can also be brought up by hand:

```bash
cd Prog
python3 -m venv .venv && .venv/bin/pip install -r server/requirements.txt
.venv/bin/python server/run.py
```

The board's port is found automatically by USB VID/PID. If there are several
boards: `.venv/bin/python server/run.py --serial-port /dev/tty.usbmodem1234`.

### Without hardware

```bash
.venv/bin/python server/run.py --simulate
```

Brings up a software model of the board with plausible telemetry: you can
look at the interface, run the autotest through and check the logic of the
protections.

## Features

- **Protocol**: DShot300. The ESC is forced into DShot in its own settings, so
  the board comes up in DShot on every power-on, and the protocol choice has
  been removed from the interface so that the two sides cannot be made to
  disagree. Servo PWM and the other DShot rates are still in the firmware and
  are switched with the `P` command, see
  [docs/PROTOCOL.md](docs/PROTOCOL.md).
- **ESC telemetry**: rpm, voltage, current, temperature, consumed capacity —
  from AM32 over a separate wire, plus independent voltage and current
  measurements from the board's own sensors.
- **Thrust**: a separate reading and its own line on the graph. The sensor is
  not connected to the board yet, so a dash is shown for now; the data format
  from the firmware is already described in
  [docs/PROTOCOL.md](docs/PROTOCOL.md), and the conversion of counts into
  grams is done by the server.
- **Calibration against an external instrument** — the gear icon in the page
  header. The readings entered from a multimeter, a clamp meter and a
  reference weight are turned into corrections: voltage and current are
  corrected in the board itself and written into its flash, so they survive a
  power cycle and apply to the current cutoff too.
- **Stepped autotest**: runs through a given list of throttle values with a
  dwell on each step and writes CSV — the thing the stand exists for.
- **CSV recording** with rpm, current, power, thrust and the step label.
- **Protections**: current and temperature cutoff, throttle limit, slew rate
  limit, latching emergency stop.

## Safe shutdown

The dead-man chain is stretched from the browser to the motor: the page sends
a heartbeat every 100 ms, the server relays it to the board only while the
heartbeat keeps coming, and the firmware kills the motor if the silence lasts
longer than 300 ms.

The motor therefore stops by itself when the tab is closed, Wi-Fi is lost,
the browser hangs, the server dies or USB is unplugged. In detail —
[docs/SAFETY.md](docs/SAFETY.md).

## Structure

```
firmware/            flight controller firmware (PlatformIO, STM32duino)
  include/config.h     pinout, timings, default values
  include/protocol_bits.h  DShot encoding, telemetry CRC, throttle conversion
  src/motor_out.cpp    M1/M3 outputs: PWM and DShot over DMA
  src/esc_telem.cpp    reception of AM32 telemetry over UART5
  src/main.cpp         states, protections, exchange protocol with the computer
  include/sense_filter.h   filter for the analogue inputs
  include/esc_stream.h     picking our own ESC's packets out of the shared line
  test/                native tests of the bit logic and the arithmetic
Prog/                everything needed to run the application
  START-MAC.command        launch on macOS — click here
  START-WINDOWS.bat        launch on Windows — click here
  launcher/                start and stop mechanics, see docs/LAUNCH.md
  server/standserver/link.py   serial port
  server/standserver/sim.py    board model for working without hardware
  server/standserver/main.py   FastAPI, WebSocket, autotest
  server/standserver/calibration.py  corrections from an external instrument
  server/standserver/static/   the interface
  server/tests/smoke_test.py   end-to-end checks
scripts/
  flash.sh                 build and flash the firmware over DFU with a backup copy
docs/                wiring, flashing, the protocol and safety
```

## Restoring what was on the board before

Before every flashing `scripts/flash.sh` saves a full dump of the flash into
`firmware/backup/`. To bring the original firmware back together with its
bootloader:

```bash
~/.platformio/packages/tool-dfuutil/bin/dfu-util -d 0483:df11 -a 0 \
    -s 0x08000000:leave -D firmware/backup/before-flash-YYYYMMDD-HHMMSS.bin
```

The directory is local only: it is git-ignored and no dump is published. The
dumps are tool output, a megabyte per flashing. If you want to be able to
restore the firmware that was on the board before, keep your own copy of the
first dump somewhere outside the repository.

## Tests

```bash
cc -I firmware/include -o /tmp/pbtest firmware/test/test_protocol_bits.c && /tmp/pbtest
cc -I firmware/include -o /tmp/sstest firmware/test/test_settings_store.c && /tmp/sstest
cc -I firmware/include -o /tmp/sftest firmware/test/test_sense_filter.c && /tmp/sftest
cc -I firmware/include -o /tmp/estest firmware/test/test_esc_stream.c && /tmp/estest
Prog/.venv/bin/python Prog/server/tests/smoke_test.py
osascript -l JavaScript Prog/server/tests/peaks_test.js   # from the project root
```

## License

The project is released under the GNU General Public License version 3, see
[LICENSE](LICENSE).
