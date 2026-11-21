"""Serial link to the flight controller.

The board speaks the line protocol in docs/PROTOCOL.md: plain ASCII commands
up, newline-delimited JSON down. Reading and writing happen on a background
thread so that a stalled USB port can never block the web server's event loop.

The port is only held while a session is active, never "just in case" - see
docs/PORT_SHARING.md. On macOS /dev/cu.* is not exclusive by default, so a
second reader silently steals half the byte stream and both sides merely look
broken; holding the port around the clock made every ArduPilot configurator on
the machine unusable.
"""

from __future__ import annotations

import json
import queue
import threading
import time
from collections import deque
from pathlib import Path
from typing import Any, Deque, Dict, Optional

import serial
from serial.tools import list_ports

# The STM32 USB CDC descriptor the firmware advertises. This is ST's default
# and is emphatically not unique: ArduPilot boards ship the same pair, so a
# VID/PID match alone is not proof we found our own board.
USB_VID = 0x0483
USB_PID = 0x5740

# How long a port gets to introduce itself before we conclude it belongs to
# somebody else and let go of it.
IDENTIFY_TIMEOUT_S = 4.0

# Commands safe to send to a port that has not identified itself yet. Anything
# else could be throttle for a stranger's aircraft.
SAFE_BEFORE_IDENTIFY = {"?"}

# What the board answers to a save request, in the operator's words. Erasing
# flash freezes the processor for about a second, so the board refuses to do
# it while anything is spinning and answers "deferred" instead.
SAVE_EVENTS = {
    "done": {"level": "info", "msg": "settings saved to the board"},
    "unchanged": {"level": "info", "msg": "the board already holds these settings"},
    "queued": {"level": "info", "msg": "saving the settings to the board…"},
    "deferred": {
        "level": "warn",
        "msg": "the settings will be saved to the board as soon as the motor stops",
    },
    "failed": {"level": "error", "msg": "the board could not save the settings"},
}


def candidate_ports() -> list[Any]:
    return [p for p in list_ports.comports() if p.vid == USB_VID and p.pid == USB_PID]


def find_port(
    prefer_serial: Optional[str] = None,
    skip: Optional[set[str]] = None,
) -> Optional[str]:
    """Pick the flight controller's serial port.

    Only an exact VID/PID match is considered - there is no "first usbmodem"
    fallback, because that is how the server used to latch onto whatever board
    happened to be plugged in. When the board's serial number is known it wins,
    so a second ST device cannot take its place.
    """
    skip = skip or set()
    candidates = [p for p in candidate_ports() if p.device not in skip]

    if prefer_serial:
        for port in candidates:
            if port.serial_number == prefer_serial:
                return port.device

    return candidates[0].device if candidates else None


class SerialLink:
    """Owns the serial port and the two threads that pump it."""

    def __init__(
        self,
        port: Optional[str] = None,
        baud: int = 115200,
        state_file: Optional[Path] = None,
    ) -> None:
        self._requested_port = port
        self._baud = baud
        self._serial: Optional[serial.Serial] = None
        self._tx: "queue.Queue[str]" = queue.Queue(maxsize=256)
        self._stop = threading.Event()
        # Set every time the port handle is actually closed, so release() can
        # wait for the reader thread to do it instead of doing it itself.
        self._closed = threading.Event()
        self._threads: list[threading.Thread] = []
        self._lock = threading.Lock()

        # Session state: the port is only opened between acquire() and
        # release(), so idle minutes leave it free for other tools.
        self._wanted = False
        self._hold_off_until = 0.0
        self._opened_at = 0.0
        self._rejected: set[str] = set()

        self._state_file = state_file
        self.board_serial: Optional[str] = self._load_board_serial()

        self.port_name: Optional[str] = None
        self.connected = False
        self.hello: Dict[str, Any] = {}
        self.settings: Dict[str, Any] = {}
        self.telemetry: Dict[str, Any] = {}
        self.last_telemetry_at = 0.0
        self.events: Deque[Dict[str, Any]] = deque(maxlen=200)
        self.rx_errors = 0

    # -- board identity ----------------------------------------------------

    def _load_board_serial(self) -> Optional[str]:
        if not self._state_file or not self._state_file.is_file():
            return None
        try:
            return json.loads(self._state_file.read_text()).get("boardSerial")
        except (OSError, ValueError):
            return None

    def _remember_board_serial(self) -> None:
        """Pin future connections to the board that just identified itself."""
        if not self.port_name:
            return
        for port in candidate_ports():
            if port.device == self.port_name and port.serial_number:
                if port.serial_number == self.board_serial:
                    return
                self.board_serial = port.serial_number
                if self._state_file:
                    try:
                        self._state_file.parent.mkdir(parents=True, exist_ok=True)
                        self._state_file.write_text(
                            json.dumps({"boardSerial": self.board_serial}, indent=2)
                        )
                    except OSError:
                        pass
                return

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        self._stop.clear()
        for target in (self._reader_loop, self._writer_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self._threads.append(thread)

    def stop(self) -> None:
        self._stop.set()
        for thread in self._threads:
            thread.join(timeout=1.0)
        self._threads.clear()
        self._close()

    def acquire(self) -> None:
        """Ask for the port. Called when an operator session begins."""
        with self._lock:
            if self._wanted and self._hold_off_until == 0.0:
                return
            self._wanted = True
            self._hold_off_until = 0.0
            self._rejected.clear()

    def release(self, hold_off: float = 0.0) -> None:
        """Let the port go so other tools can use it.

        `hold_off` keeps it released for that many seconds even if a browser is
        still connected, which is what flashing and the configurator need.

        The close itself is left to the reader thread. Closing from here used
        to kill that thread: pyserial's close() sets its own `fd` to None, and
        the reader was sitting in a blocking read() on it, which raised a
        TypeError that no except clause caught. With the reader gone nothing
        was left to reopen the port, so the board stayed missing until the
        server was restarted - see the traceback in server/logs/server.log.
        """
        # Cleared before the reader can observe the release, so its set() can
        # never be missed and this never waits on an event already consumed.
        self._closed.clear()
        with self._lock:
            self._wanted = False
            self._hold_off_until = time.monotonic() + hold_off if hold_off > 0 else 0.0
            open_now = self._serial is not None

        if not open_now:
            return
        if not self._threads:
            self._close()
            return
        if not self._closed.wait(timeout=2.0):
            self.events.append(
                {"level": "warn", "msg": "the port did not close within 2 s, forcing it"}
            )
            self._close()

    @property
    def wanted(self) -> bool:
        with self._lock:
            if not self._wanted:
                return False
            return time.monotonic() >= self._hold_off_until

    def _close(self) -> None:
        with self._lock:
            if self._serial is not None:
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None
            self.connected = False
            self.hello = {}
        # Stale telemetry outlives the port otherwise, and a frozen readout
        # that looks live is worse than an obviously empty one.
        self.telemetry = {}
        self.last_telemetry_at = 0.0
        while not self._tx.empty():
            try:
                self._tx.get_nowait()
            except queue.Empty:
                break
        self._closed.set()

    # -- transmit ----------------------------------------------------------

    def send(self, line: str) -> None:
        try:
            self._tx.put_nowait(line.strip() + "\n")
        except queue.Full:
            self.events.append({"level": "warn", "msg": "command queue full, dropping"})

    def keepalive(self) -> None:
        self.send("K")

    # -- threads -----------------------------------------------------------

    def _open(self) -> bool:
        device = self._requested_port or find_port(self.board_serial, self._rejected)
        if device is None:
            return False
        try:
            # exclusive=True turns a silent split of the byte stream into an
            # honest "port is busy" for whoever comes second.
            port = serial.Serial(
                device,
                self._baud,
                timeout=0.1,
                write_timeout=0.5,
                exclusive=True,
            )
        except (OSError, serial.SerialException) as exc:
            self.events.append({"level": "error", "msg": f"{device}: {exc}"})
            self._rejected.add(device)
            return False

        with self._lock:
            self._serial = port
            self.port_name = device
            self.connected = True
            self._opened_at = time.monotonic()
        self.events.append({"level": "info", "msg": f"port opened: {device}"})
        # Ask the board to introduce itself. Until it does, nothing else is
        # allowed out of the writer.
        self.send("?")
        return True

    def _reader_loop(self) -> None:
        buffer = b""
        while not self._stop.is_set():
            try:
                buffer = self._reader_step(buffer)
            except Exception as exc:  # noqa: BLE001 - deliberately everything
                # This thread owns both opening and closing the port, so it has
                # to outlive every possible error. It once died on a TypeError
                # raised from inside pyserial, and with it went the only code
                # that could release or reopen the port.
                self.events.append({"level": "error", "msg": f"port read failed: {exc}"})
                self._close()
                buffer = b""
                time.sleep(0.5)

    def _reader_step(self, buffer: bytes) -> bytes:
        """One pass of the reader loop, returning the new line buffer."""
        if not self.wanted:
            if self.connected:
                self.events.append({"level": "info", "msg": "port released"})
                self._close()
            time.sleep(0.2)
            return b""

        if not self.connected:
            if not self._open():
                time.sleep(1.0)
            return buffer

        # A port that never introduces itself is somebody else's board.
        # Holding it exclusively would block their configurator, so drop it
        # and try the next candidate.
        if not self.hello and (time.monotonic() - self._opened_at) > IDENTIFY_TIMEOUT_S:
            device = self.port_name
            self.events.append(
                {
                    "level": "warn",
                    "msg": f"{device} does not answer as the stand's board, letting it go",
                }
            )
            if device:
                self._rejected.add(device)
            self._close()
            return b""

        # Read through a local reference: _close() may null the attribute out
        # from another thread between here and the read.
        port = self._serial
        if port is None:
            return buffer
        try:
            chunk = port.read(512)
        except Exception as exc:  # noqa: BLE001 - pyserial raises more than it documents
            self.events.append({"level": "error", "msg": f"read failed: {exc}"})
            self._close()
            return b""

        if not chunk:
            return buffer

        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            self._handle_line(raw.decode("utf-8", errors="replace").strip())
        return buffer

    def _handle_line(self, line: str) -> None:
        if not line:
            return
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            self.rx_errors += 1
            return
        if not isinstance(message, dict):
            return

        if "hello" in message:
            first = not self.hello
            self.hello = message
            if first:
                self.events.append(
                    {"level": "info", "msg": f"board identified: {message.get('version')}"}
                )
                self._remember_board_serial()
        elif "settings" in message:
            self.settings = message["settings"]
        elif "save" in message:
            # Whether the calibration will survive a power cycle is the whole
            # point of saving it, so every outcome is reported rather than
            # only the failures.
            outcome = str(message["save"])
            self.events.append(SAVE_EVENTS.get(outcome, {
                "level": "warn",
                "msg": f"saving the settings: {outcome}",
            }))
        elif "ack" in message:
            if not message.get("ok", True):
                self.events.append(
                    {
                        "level": "warn",
                        "msg": f"{message['ack']} rejected: {message.get('detail', '')}",
                    }
                )
        else:
            self.telemetry = message
            self.last_telemetry_at = time.monotonic()

    def _writer_loop(self) -> None:
        while not self._stop.is_set():
            try:
                line = self._tx.get(timeout=0.1)
            except queue.Empty:
                continue
            if not self.connected:
                continue
            # Never command a device that has not proven it is our board: it
            # could be a flight controller with props on.
            if not self.hello and line.strip() not in SAFE_BEFORE_IDENTIFY:
                continue
            port = self._serial
            if port is None:
                continue
            try:
                port.write(line.encode("ascii"))
            except Exception as exc:  # noqa: BLE001 - see the reader loop
                # Closing here would pull the handle out from under the
                # reader's blocking read(). A port broken enough to fail a
                # write will fail its next read too, and the reader closes it.
                self.events.append({"level": "error", "msg": f"write failed: {exc}"})

    # -- status ------------------------------------------------------------

    def status(self) -> Dict[str, Any]:
        fresh = (time.monotonic() - self.last_telemetry_at) < 1.0
        with self._lock:
            held = self._hold_off_until
        return {
            "connected": self.connected,
            "streaming": self.connected and fresh,
            # An open port is not the same as the right board on the other end.
            # Until the hello handshake lands we could just as well be talking
            # to whatever else was plugged in.
            "identified": bool(self.hello),
            "wanted": self.wanted,
            "heldOffFor": max(0.0, held - time.monotonic()) if held else 0.0,
            "port": self.port_name,
            "board": self.hello,
            "settings": self.settings,
            "rxErrors": self.rx_errors,
        }

    def drain_events(self) -> list[Dict[str, Any]]:
        out = []
        while self.events:
            out.append(self.events.popleft())
        return out
