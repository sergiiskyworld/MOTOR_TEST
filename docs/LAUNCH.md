# Starting the application

Everything the browser application needs lives in the `Prog` folder: the
server, the Python environment and the launch files. Only the firmware
(`firmware/`), the flashing script (`scripts/flash.sh`) and the documentation
(`docs/`) are kept outside it.

```
Prog/
  START-MAC.command       ← click here on macOS
  START-WINDOWS.bat       ← click here on Windows
  launcher/               the machinery of starting and stopping
    start-stand.sh          the engine for macOS and Linux
    start-stand.ps1         the engine for Windows
    stop-stand.sh           stops the server, macOS/Linux
    stop-stand.ps1          stops the server, Windows
    STOP-MAC.command        stop the server on macOS
    STOP-WINDOWS.bat        stop the server on Windows
  server/                 the server and web interface code
  .venv, .venv-win        Python environments, made on their own (hidden)
  .browser-profile        browser profile for the stand window, made on its own
```

## Why two launch files and not one

A single file clickable on both systems cannot be made: macOS and Windows both
choose the program to open a file by its extension. macOS runs only `.command`
as a script (and knows nothing about `.bat`), Windows only `.bat` or `.cmd`
(and knows nothing about `.command`). A file has one extension, so two files
is the minimum. The names were made to say the same thing so that there is
nothing to choose between: on each system the one with that system's name in
it is the one that works.

The "one `.html`" option will not do: a page opened from a file cannot bring
up the server, and without the server there is nothing to show.

## What happens at startup

1. The script checks whether the server answers at `http://127.0.0.1:8000/`.
2. On the first run the Python environment is created and the dependencies
   from `server/requirements.txt` are installed (needs the internet, once).
   On macOS the environment is called `.venv`, on Windows `.venv-win`: the
   project folder can be carried between the systems, while their Python
   environments are not compatible with each other.
3. If the server was not answering, it is brought up, detached from the window
   it was launched from. Closing the Terminal or the console no longer takes
   it down with them.
4. A **separate full-screen browser window** opens, with no tabs and no
   address bar.

Clicking a second time is safe: a second server will not come up, one more
window will open.

### A separate full-screen window, not a tab

The stand opens in a window of its own deliberately: a tab closed by accident
here means a stopped motor, and among a dozen other tabs it is easy to lose.

The `--app` and `--kiosk` modes of browsers on the Chromium engine are used —
Chrome, Edge, Brave, Chromium, Yandex. Together they give a window with no
tabs and no address bar, expanded to the full screen. To close it: **⌘Q** on
macOS, **Alt+F4** on Windows. To reload the page: **⌘R** and **F5**
respectively.

The window opens in a browser profile of its own (`Prog/.browser-profile`),
and this is not cosmetic. The full-screen flags are read only when the browser
itself starts: if Chrome is already running, the system hands the link to the
running process, the flags are lost and the window opens at the usual size —
which is exactly how it was before the separate profile appeared. A side
benefit: the stand does not depend on the extensions and tabs of the main
profile.

If no such browser is found: on macOS a new Safari window opens (Safari has
none of these modes), on Windows a tab in the default browser. On Windows this
is next to impossible, because Edge is present in any Windows 10/11.

### The Terminal window on macOS

Clicking `START-MAC.command` opens a Terminal window with the progress of the
startup, and it closes itself once the stand window is ready. If it stays
open, nothing is broken; close it by hand.

There is no application bundle (`.app`) here, and that is deliberate. macOS
does not let a bundle sitting in `~/Documents` execute a script next to it:
startup failed with `Operation not permitted` and would have worked only after
granting the application access to the Documents folder in the privacy
settings. A `.command` file is run by the Terminal, which has that access
already.

## From a phone

The server listens on all interfaces, so from a phone on the same network the
page opens at the computer's IP address, for example
`http://192.168.1.50:8000`.

To find the address:

```bash
ipconfig getifaddr en0     # macOS, Wi-Fi
```

```powershell
ipconfig | Select-String IPv4   # Windows
```

On the first run the system will ask for permission for incoming
connections — without it the phone will not see the page.

## Calibration

The gear in the page header opens a window with three blocks: voltage, current
and thrust. The order is the same in each one — look at what the stand shows,
measure the same thing with an external instrument, type in what was measured
and press "Apply".

Voltage and current are recalculated **in the board itself**: the server
computes the new scale, sends it with the `S` command and immediately asks the
board to write the settings into its own flash memory with the `W` command.
The correction therefore survives a power cycle and applies to the current
cutoff as well. The line at the top of the window shows whether the settings
have been written yet or not.

Writing erases a sector of flash memory and stops the processor for a second,
so the board never does it on the move: if throttle is applied, it answers
"will be saved when the motor stops" and performs the write once the motor has
stopped.

The order for current is mandatory: first "Zero" with the motor at rest, then
the scale under load. The firmware clips negative current to zero, so with a
wrong zero the reading stays at zero at any current, and no scale will fix
that — in such a case the server says so outright.

Thrust is recalculated **on the server**, because the firmware does not know
about the sensor yet and will send only raw counts. The zero and the scale are
kept in `Prog/server/logs/calibration.json`; the file survives a restart but
does not go into git, so when the stand is moved to another computer the
thrust calibration has to be done again.

## The USB port

The board's port is held only while a stand window is open in the browser. It
is released:

- 3 seconds after the last window is closed;
- 3 seconds after the page goes silent — a crashed browser and a phone that
  has gone to sleep also count as a closed window;
- at once from the "Release port" button in the interface (for 3 minutes, for
  the AM32 configurator or for flashing firmware).

Why this matters and how it broke the first time — [PORT_SHARING.md](PORT_SHARING.md).

## If it does not start

Look at the log: `Prog/server/logs/server.log`.

| Symptom | Cause |
|---|---|
| "Python 3 not found" | install Python from python.org; on Windows tick "Add Python to PATH" |
| "Could not install the dependencies" | no internet on the first run |
| "The server did not come up within 10 seconds" | port 8000 is taken by another program, or on Windows access through the firewall was not allowed |
| "The server folder was not found" | the `launcher` folder was taken out of `Prog` — it has to sit next to `server/` |
| the page opened, but "board not found" | check the USB cable; the port may have stayed with the AM32 configurator |

The port can be set with an environment variable: `STAND_PORT=8080`.

## Checking without hardware

```bash
Prog/.venv/bin/python Prog/server/run.py --simulate
```

Brings up a software model of the board with plausible telemetry: the
interface, the autotest and the protection logic can be looked at without a
motor.

## Tests

```bash
Prog/.venv/bin/python Prog/server/tests/smoke_test.py
```
