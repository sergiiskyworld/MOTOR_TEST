# Flashing the flight controller

The firmware goes in at address `0x08000000` over USB DFU and takes the whole
flash.

> **It erases whatever was on the board before**, Betaflight or ArduPilot.
> They can be put back the same way through DFU, from Betaflight Configurator
> or Mission Planner. If you still need the board for flying, save the current
> dump before you start: `dfu-util -a 0 -s 0x08000000:0x100000 -U backup.bin`.

## Installing the tools

```bash
pip install platformio
```

PlatformIO pulls in the `arm-none-eabi-gcc` compiler, the STM32duino core and
`dfu-util` by itself on the first build. On macOS no extra drivers are needed;
on Windows you may have to replace the STM32 BOOTLOADER driver with WinUSB
using Zadig.

## Entering DFU mode

1. **With the BOOT button.** Hold the BOOT button on the board and plug in
   USB.
2. **By shorting the BOOT pads.** If there is no button, short the `BOOT` pads
   with tweezers at the moment USB is connected.

There is no software way, and that has been checked on hardware. The `Z`
command ("jump to DFU") resets the board, but after a software jump the ROM
bootloader does not bring USB up: the board leaves the bus completely, and the
only way to bring it back is to reconnect USB. The button has therefore been
removed from the web interface, and the server does not send the `Z` command.

Three variants of the jump were tried: with a `SYSCFG` memory remap (control
came back to the application), without the remap from `setup()` (the board
left the bus), and from `SystemClock_Config()` before the PLL comes up; the
last variant is in the sources but has not been checked on the board. The
diagnostics for this are still in the telemetry: the `bootFlag` field shows
that the mechanism for passing a flag through a reset works, and the `tries`
counter in backup SRAM counts the jump attempts.

To check that the board is in DFU:

```bash
dfu-util -l
# Found DFU: [0483:df11] ... name="@Internal Flash  /0x08000000/..."
```

## Building and flashing

```bash
cd firmware
pio run            # build only
pio run -t upload  # build and flash
```

After flashing, the board restarts and appears as an ordinary serial port
(VID `0x0483`, PID `0x5740`). The red and green LEDs show the state, and the
buzzer gives a short beep.

## What the firmware does with the clocks

The Arduino core does not know about the crystal on this particular board, so
the firmware overrides `SystemClock_Config()` (`src/main.cpp`): an 8 MHz
crystal, PLL `M=8, N=336, P=2` gives 168 MHz for the core, `Q=7` gives exactly
48 MHz for USB. The APB1 bus is divided by 4, which clocks timers TIM2/TIM3 at
84 MHz, and the DShot bit durations are counted from that frequency.

If USB is not detected after flashing, look here first: the wrong USB clock is
the most frequent cause.

## Checking without the interface

The board can be exercised from an ordinary terminal, the protocol is text:

```bash
screen /dev/tty.usbmodem1234 115200
```

Send `?` and the board identifies itself and reports its settings. Telemetry
lines will come once every 20 ms. The full list of commands is in
[PROTOCOL.md](PROTOCOL.md).

Bear in mind that the watchdog expects a command at least once every 300 ms,
so applying throttle by hand and holding it will not work; that is exactly
what the server is for.
