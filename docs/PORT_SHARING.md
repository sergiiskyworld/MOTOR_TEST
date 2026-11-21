# The shared USB port: rules for living next to the configurator

On this machine one more program reaches for the same USB port — ArduFly
Configurator (`~/Documents/Ardu_Fly`), a ground station for ArduPilot flight
controllers. On 23.09.2026 this left the user unable to connect to any flight
controller at all. This document describes what happened and what to change in
the stand's server so that it does not happen again.

## What happened

`server/run.py` (now `Prog/server/run.py`) opened `/dev/cu.usbmodem1101` at startup and held it
permanently, reading actively (the descriptor offset in `lsof` kept growing).
The configurator opened the same port — without an error.

`/dev/cu.*` on macOS is **not exclusive**: several processes open it at the
same time, and the incoming stream is divided between the readers. Not one of
them assembles a whole MAVLink frame. From the outside this looks not like a
conflict but like "the board does not answer" — there is no error anywhere,
and the cause is visible only through `lsof`.

The server could not be stopped with `kill`: `~/Library/LaunchAgents/com.motortest.stand.plist`
declares `KeepAlive=true` and `ThrottleInterval=5`, and launchd brought up a
new process five seconds later (PID 89389 → 3897). Only
`launchctl bootout gui/$(id -u)/com.motortest.stand` helped.

**Current state: there is no autostart at all.** The service is unloaded, the
plist template and the installation scripts are deleted — the server is
brought up by clicking `Prog/START-MAC.command` or `Prog/START-WINDOWS.bat`.
If the service is still registered from earlier times, remove it:

```bash
launchctl bootout gui/$(id -u)/com.motortest.stand
rm -f ~/Library/LaunchAgents/com.motortest.stand.plist
```

## What was done (23.09.2026)

All six items below are implemented; the links to the checks are in
`Prog/server/tests/smoke_test.py` (8 separate checks on the rules of this
document).

| Item | How it was done |
|---|---|
| 1. The port for the length of a session | `link.acquire()` on the first WebSocket client, release after `PORT_GRACE_S = 3 s` once the last window is closed |
| 2. Remove `KeepAlive` | the plist template now has `KeepAlive` with a dictionary `SuccessfulExit=false` |
| 3. Exclusive open | `serial.Serial(..., exclusive=True)` |
| 4. Selection by VID/PID | "the first `cu.usbmodem*`" is gone entirely; on identification the `serial_number` is remembered in `logs/board.json` and takes priority from then on |
| 5. Release the port on request | `POST /api/port/release` (for 180 s) and `POST /api/port/acquire`, the "Release port" / "Take port" buttons in the interface, automatic release in `scripts/flash.sh` |
| 6. Do not send commands blindly | the writer drops everything except `?` until `hello` has arrived; a port that has not identified itself within 4 s is released and put on a blacklist, so that the next candidate can be tried |

Item 6 turned out to matter more than it looked: the VID/PID `0483:5740` is
ST's default value, and ArduPilot flight controllers present themselves in
exactly the same way. A matching VID/PID does not prove that our board has
been found, so the only reliable sign is the answer to `?`. Until it arrives,
nothing goes into the port.

Autostart through launchd is gone completely (the `install-autostart.sh` and
`uninstall-autostart.sh` scripts and the plist template are deleted): starting
is done by clicking `Prog/START-MAC.command`, and keeping a second entry point
would mean ending up again with a server that nobody remembers starting.

## The port was not released all the way (24.09.2026)

The session did not always end, and two different things were to blame.

**The reader thread was dying.** `release()` closed the port from the web
server's thread while the reader thread sat in a blocking `read()`. `close()`
in pyserial clears its own `fd`, and the concurrent `os.read(self.fd, …)`
failed with `TypeError: 'NoneType' object cannot be interpreted as an
integer` — that is neither an `OSError` nor a `SerialException`, so no
`except` caught it. The thread died, and with it went the only code that knew
how to close and open the port. The traceback is kept in
`Prog/server/logs/server.log`.

Now the closing is done by the reader thread itself, and `release()` only
clears a flag and waits for confirmation (the `_closed` event, 2 s timeout).
The body of the loop is wrapped so that the thread survives any error, and
accesses to the port go through a local reference — the attribute can be
cleared from another thread at any moment.

**The socket stayed open.** Releasing the port hung on the WebSocket being
closed, but a phone that has gone to sleep, a crashed browser and a page
frozen in the back/forward cache leave the socket open until the TCP
timeout — that is minutes. The heartbeat arrives every 100 ms and is the only
thing a half-dead connection cannot fake, so a session with no heartbeat for
longer than `UI_DEAD_S = 3 s` is closed by force (`Stand.reaper_loop`). On the
page's side the socket is closed explicitly on `pagehide` and `beforeunload`:
`beforeunload` does not fire on iOS at all, and a frozen page stops its timers
but keeps the socket.

Checks: "a silent browser session is closed by the server", "the reader
thread survives an unexpected pyserial error", "release closes the port
handle" in `Prog/server/tests/smoke_test.py`.

## What to change in the server

1. **Open the port for the length of a session, not forever.** Take the
   USB CDC when the first WebSocket client appears, and close it when the last
   one leaves. A port held around the clock "in case it is needed" blocks any
   configurator — ArduFly, Mission Planner, QGroundControl, AM32.

2. **Remove the unconditional `KeepAlive` from the process that owns the
   hardware.** `RunAtLoad` is enough, or `KeepAlive` with a dictionary
   `SuccessfulExit=false`, so that a normal exit is not brought back up. As it
   is now the process cannot be stopped by anything but `launchctl bootout`,
   and a person has no way of guessing that.

3. **Open the port exclusively:** `serial.Serial(..., exclusive=True)`. Then
   the second claimant gets an honest "port is busy" instead of a silent loss
   of bytes, and the cause is visible at once, without `lsof`.

4. **Choose our own board by VID/PID, not the first `cu.usbmodem*`.** As it is
   now the server latches onto whatever is plugged in, including somebody
   else's ArduPilot flight controller. If the board has a `serial_number`,
   bind to that.

5. **Release the port before DFU and on request.** `scripts/flash.sh` fights
   for the port with its own server. A handle in the API (for example
   `POST /port/release`) and an automatic release for the length of the
   flashing are both useful.

6. **Do not send commands into the board blindly at startup.** If somebody
   else's flight controller turns out to be on the port, sending the stand's
   signals is control of someone else's craft with its motors connected. Read
   the identification first and make sure the firmware is ours.

## How to check that there is no conflict

```bash
lsof /dev/cu.*                      # who has the port open
ps -o pid,lstart,command -p <PID>   # whose process it is
```

The offset (`0t…`) in the `lsof` output, taken twice in a row: growing means
the process is reading the port right now; unchanged means it is a forgotten
open descriptor.

## The commitment in return

On the configurator's side the symmetrical problem has already been fixed: it
closed the port only on an explicit disconnect and left behind descriptors
from cancelled connection attempts (four of them were found on this same
port). Now the port is closed on every exit path, and the configurator does
not hold it while it is not connected.
