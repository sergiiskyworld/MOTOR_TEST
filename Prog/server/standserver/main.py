"""Web server for the motor thrust test stand.

Holds the serial link to the flight controller, fans telemetry out to every
connected browser over a WebSocket, records CSV runs and drives the stepped
autotest profile.

Safety note: the firmware stops the motor if the host goes quiet for 300 ms.
This server only feeds that watchdog while a browser is actively sending
heartbeats, so closing the tab, killing Wi-Fi or suspending the laptop all
propagate through to a real motor stop.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from . import calibration as calib
from .calibration import Calibration, CalibrationError
from .link import SerialLink
from .recorder import Recorder
from .sim import SimulatedLink

STATIC_DIR = Path(__file__).parent / "static"

BROADCAST_HZ = 25
KEEPALIVE_HZ = 20
# A browser heartbeat older than this counts as "nobody is watching".
UI_TIMEOUT_S = 0.4

# The port is released this long after the last browser window closes. A page
# reload disconnects and reconnects within about a second, and dropping the USB
# port on every refresh would be both slow and noisy in the log.
PORT_GRACE_S = 3.0

# How long POST /api/port/release keeps the port free. Long enough to flash the
# board or poke at it with a configurator without racing this server for it.
PORT_RELEASE_HOLD_S = 180.0

# Closing a tab normally closes the WebSocket, and that close is what hands the
# port back. But a phone that goes to sleep, a browser that crashes and a page
# frozen by the operating system all leave the socket open until TCP gives up,
# which takes minutes. The heartbeat arrives every 100 ms and is the one signal
# a half-dead connection cannot fake, so a silent session is closed by force.
UI_DEAD_S = 3.0


class Stand:
    def __init__(self, link, log_dir: Path) -> None:
        self.link = link
        self.recorder = Recorder(log_dir)
        self.calibration = Calibration(log_dir / "calibration.json")
        self.clients: Set[WebSocket] = set()
        self.last_ui_beat = 0.0
        self.throttle_cmd = {1: 0, 3: 0}
        self.note = ""
        self.autotest: Optional[asyncio.Task] = None
        self.autotest_state: Dict[str, Any] = {"running": False}
        self.last_broadcast = 0.0
        self._log: List[Dict[str, Any]] = []
        # What the current sensor reads at rest. Tracked continuously, because
        # comparing it against the zero stored in the board is the only way to
        # notice that the zero has gone stale at a moment when the motor is
        # already spinning and rest cannot be measured.
        self._rest_i_mv: Optional[float] = None
        # The mark consumption is counted from. None means "take the ESC's
        # first reading", so that the tile does not show what has accumulated
        # since the battery was last plugged in.
        self._mah_zero: Optional[int] = None
        self._release_task: Optional[asyncio.Task] = None

    # -- port sessions -----------------------------------------------------

    def claim_port(self) -> None:
        """A browser is here, so the board is ours for the duration."""
        if self._release_task and not self._release_task.done():
            self._release_task.cancel()
            self._release_task = None
        self.link.acquire()

    def schedule_release(self) -> None:
        """Give the port up shortly after the last window goes away."""
        if self._release_task and not self._release_task.done():
            return
        self._release_task = asyncio.create_task(self._release_after_grace())

    async def _release_after_grace(self) -> None:
        try:
            await asyncio.sleep(PORT_GRACE_S)
        except asyncio.CancelledError:
            raise
        if not self.clients:
            self.link.release()
            self.log("info", "port released: no windows left")

    def end_session(self, reason: str) -> None:
        """No browser is watching any more: stop the motor, give the port back."""
        if self._release_task and not self._release_task.done():
            return  # already on the way out, don't log it twice
        self.cancel_autotest()
        self.stop_all()
        self.link.send("D")
        self.schedule_release()
        self.log("info", f"session closed ({reason})")

    async def close_dead_sessions(self) -> None:
        """Drop sockets that stopped sending heartbeats."""
        clients = list(self.clients)
        self.clients.clear()
        for client in clients:
            with contextlib.suppress(Exception):
                await client.close()
        self.end_session("the browser went silent")

    async def reaper_loop(self) -> None:
        while True:
            await asyncio.sleep(0.5)
            if not self.clients:
                continue
            if (time.monotonic() - self.last_ui_beat) < UI_DEAD_S:
                continue
            await self.close_dead_sessions()

    def release_port_for(self, seconds: float) -> None:
        """Hand the port to another tool - a configurator or the flasher."""
        self.cancel_autotest()
        self.stop_all()
        self.link.send("D")
        self.link.release(hold_off=seconds)
        self.log("info", f"port released for {int(seconds)} s on request")

    # -- helpers -----------------------------------------------------------

    def log(self, level: str, msg: str) -> None:
        self._log.append({"level": level, "msg": msg, "at": time.time()})
        del self._log[:-50]

    @property
    def ui_alive(self) -> bool:
        return bool(self.clients) and (time.monotonic() - self.last_ui_beat) < UI_TIMEOUT_S

    def set_throttle(self, motor: int, value: int) -> None:
        value = max(0, min(int(value), 1000))
        self.throttle_cmd[motor] = value
        self.link.send(f"T {motor} {value}")

    def stop_all(self) -> None:
        for motor in (1, 3):
            self.throttle_cmd[motor] = 0
            self.link.send(f"T {motor} 0")

    def estop(self, reason: str) -> None:
        self.cancel_autotest()
        self.stop_all()
        self.link.send("X")
        self.log("warn", f"emergency stop: {reason}")

    def cancel_autotest(self) -> None:
        if self.autotest and not self.autotest.done():
            self.autotest.cancel()

    # -- command dispatch --------------------------------------------------

    async def handle(self, message: Dict[str, Any]) -> None:
        kind = message.get("type", "")
        self.last_ui_beat = time.monotonic()

        if kind == "hb":
            return

        if kind == "throttle":
            motor = int(message.get("motor", 1))
            if motor in self.throttle_cmd and not self.autotest_state["running"]:
                self.set_throttle(motor, int(message.get("value", 0)))
        elif kind == "arm":
            self.stop_all()
            self.link.send("A")
        elif kind == "disarm":
            self.cancel_autotest()
            self.stop_all()
            self.link.send("D")
        elif kind == "estop":
            self.estop("requested from the UI")
        elif kind == "reset":
            self.stop_all()
            self.link.send("R")
        elif kind == "protocol":
            self.cancel_autotest()
            self.stop_all()
            self.link.send(f"P {message.get('name', '')}")
            self.log("info", f"protocol set to {message.get('name')} - power-cycle the ESC")
        elif kind == "setting":
            self.link.send(f"S {message.get('key')} {int(message.get('value', 0))}")
        elif kind == "dshot":
            motor = int(message.get("motor", 1))
            command = int(message.get("command", 0))
            repeat = int(message.get("repeat", 10))
            self.link.send(f"C {motor} {command} {repeat}")
        elif kind == "dfu":
            # Deliberately not forwarded. The firmware's software jump into the
            # ROM bootloader leaves the board off the USB bus entirely, so it
            # takes a physical replug to recover. Flashing goes through the
            # BOOT button and scripts/flash.sh instead.
            self.log("warn", "DFU is entered with the BOOT button only, see docs/FLASHING.md")
        elif kind == "record":
            if message.get("action") == "start":
                path = self.recorder.start(str(message.get("name", "")))
                self.log("info", f"recording to {path.name}")
            else:
                path = self.recorder.stop()
                if path:
                    self.log("info", f"recording saved: {path.name}")
        elif kind == "note":
            self.note = str(message.get("text", ""))[:64]
        elif kind == "autotest":
            if message.get("action") == "start":
                self.start_autotest(message)
            else:
                self.cancel_autotest()
        elif kind == "mahReset":
            self._mah_zero = None
            self.log("info", "consumption reset to zero")
        elif kind == "calibrate":
            self.calibrate(str(message.get("target", "")), message.get("value"), message)
        else:
            # Dropping unknown commands silently is a bad idea: after any
            # update the page runs ahead of the server, and a button then
            # stops responding without saying anything about it.
            self.log("warn", f"the server does not know the command \"{kind}\" - "
                             f"it is most likely running old code, restart the stand")

    # -- calibration -------------------------------------------------------

    def calibrate(self, target: str, value: Any, message: Optional[Dict[str, Any]] = None) -> None:
        """Make a channel agree with an instrument the operator trusts.

        Voltage and current are corrected inside the board, by rewriting the
        scale it already applies, so the correction also protects the current
        cutoff. Thrust is corrected here, because the firmware does not know
        about a load cell yet.
        """
        telemetry = self.link.telemetry or {}
        board = telemetry.get("board") or {}
        load = telemetry.get("load") or {}
        settings = self.link.status().get("settings") or {}
        in_board = target in ("voltage", "current", "currentZero")
        spinning = telemetry.get("state") == 1 and (telemetry.get("thr") or [0])[0] > 0

        try:
            # A scale is the ratio between a real signal and a real reading,
            # and at a standstill neither exists. Without this the sensor's
            # own zero offset would be taken for current and turned into a
            # coefficient that looks plausible and is pure nonsense.
            # A zero taken under load carries that load inside it, and every
            # reading after it comes out low by exactly that much.
            if target in ("currentZero", "thrustZero") and spinning:
                raise CalibrationError("the sensor can only be zeroed with the motor stopped")
            if in_board and not settings:
                raise CalibrationError("the board has not reported its settings yet")
            if in_board and board.get("iPinMv") is None:
                raise CalibrationError(
                    "the board does not send raw sensor readings: it is running "
                    "old firmware, reflash it"
                )
            number = float(value) if value is not None else 0.0
            offset_mv = int(settings.get("currentOffsetMv", 0))

            if target == "current":
                new = calib.current_scale(float(board["iPinMv"]), offset_mv, number)
                self.link.send(f"S currentScaleM {new}")
                self.log("info", f"current calibrated against {number:.2f} A on the "
                                 f"instrument: {new / 1000.0:.1f} A/V")
            elif target == "voltage":
                new = calib.voltage_scale(float(board["vPinMv"]), number)
                self.link.send(f"S vbatScaleM {new}")
                self.log("info", f"voltage calibrated: vbatScaleM = {new}")
            elif target == "currentZero":
                new = calib.current_zero_offset(float(board["iPinMv"]))
                self.link.send(f"S currentOffsetMv {new}")
                self.log("info", f"current zero: currentOffsetMv = {new} mV")
            elif target == "thrustZero":
                zero = self.calibration.set_load_zero(load)
                self.log("info", f"thrust zero: {zero:.0f} counts")
            elif target == "thrust":
                scale = self.calibration.set_load_measured(load, number)
                self.log("info", f"thrust calibrated: {scale:.5f} g per count")
            else:
                return
        except CalibrationError as exc:
            self.log("error", f"calibration not carried out: {exc}")
        except (KeyError, TypeError, ValueError) as exc:
            self.log("error", f"calibration not carried out: {exc}")
        else:
            if in_board:
                # A coefficient that lives only in RAM is lost the moment the
                # battery comes off, and the operator would have to repeat the
                # whole procedure with a multimeter. The board decides when it
                # is safe to burn it into flash; here we only ask.
                self.link.send("W")
                # Ask it to repeat its settings too, so the dialog shows the
                # scale that is actually in force rather than the old one.
                self.link.send("?")

    # -- autotest ----------------------------------------------------------

    def start_autotest(self, message: Dict[str, Any]) -> None:
        if self.autotest_state["running"]:
            return
        steps = message.get("steps") or []
        motor = int(message.get("motor", 1))
        if not steps:
            return
        self.autotest = asyncio.create_task(self._run_autotest(motor, steps))

    async def _run_autotest(self, motor: int, steps: List[Dict[str, Any]]) -> None:
        self.autotest_state = {"running": True, "step": 0, "total": len(steps), "throttle": 0}
        started_recording = False
        try:
            if not self.recorder.active:
                path = self.recorder.start("autotest")
                started_recording = True
                self.log("info", f"autotest recording to {path.name}")

            for index, step in enumerate(steps):
                throttle = max(0, min(int(step.get("throttle", 0)), 1000))
                hold = max(0.2, float(step.get("hold", 3.0)))
                self.autotest_state.update({"step": index + 1, "throttle": throttle})
                self.note = f"step{index + 1}@{throttle}"
                self.set_throttle(motor, throttle)

                deadline = time.monotonic() + hold
                while time.monotonic() < deadline:
                    await asyncio.sleep(0.05)
                    state = self.link.telemetry.get("state", 0)
                    if state != 1:
                        raise RuntimeError("board left the armed state")
                    if not self.ui_alive:
                        raise RuntimeError("browser stopped responding")

            self.log("info", "autotest finished")
        except asyncio.CancelledError:
            self.log("warn", "autotest cancelled")
            raise
        except Exception as exc:  # noqa: BLE001 - surfaced to the operator
            self.log("error", f"autotest aborted: {exc}")
        finally:
            self.note = ""
            self.set_throttle(motor, 0)
            self.autotest_state = {"running": False}
            if started_recording:
                self.recorder.stop()

    # -- background loops --------------------------------------------------

    async def keepalive_loop(self) -> None:
        period = 1.0 / KEEPALIVE_HZ
        was_alive = False
        last_complaint = 0.0
        while True:
            await asyncio.sleep(period)
            # Feeding the watchdog while the operator's screen is frozen would
            # leave a motor spinning with nobody able to see it, so the
            # telemetry fan-out has to be proven healthy too.
            alive = self.ui_alive and (time.monotonic() - self.last_broadcast) < 1.0
            if alive:
                self.link.keepalive()
            elif was_alive:
                # Deliberately stop talking: the board's own watchdog is a more
                # trustworthy last line of defence than anything we do here.
                self.cancel_autotest()
                # A browser under load drops the odd heartbeat, so rate-limit
                # the complaint rather than burying the log in it.
                now = time.monotonic()
                if now - last_complaint > 5.0:
                    last_complaint = now
                    self.log("warn", "browser heartbeat lost, releasing the watchdog")
            was_alive = alive

    def _consumption(self, telemetry: Dict[str, Any]) -> Optional[int]:
        """Consumption since the last reset.

        There is nothing to zero the counter inside the ESC with: it counts
        from the moment power was applied and keeps growing between runs. So
        the stand remembers a mark and shows the difference from it.
        """
        esc = telemetry.get("esc") or {}
        if esc.get("ok") != 1 or esc.get("mah") is None:
            return None
        total = int(esc["mah"])
        if self._mah_zero is None:
            self._mah_zero = total
        # The ESC may have restarted and begun counting from scratch; a mark
        # from its previous life would make the consumption negative.
        if total < self._mah_zero:
            self._mah_zero = total
        return total - self._mah_zero

    def _track_rest_current(self, telemetry: Dict[str, Any]) -> None:
        """Remembers the current sensor's signal while the motor is stopped."""
        board = telemetry.get("board") or {}
        pin_mv = board.get("iPinMv")
        if pin_mv is None or any(telemetry.get("out") or [0]):
            return
        # A time constant of several seconds: the value has to be steady
        # rather than quick, because it is what the stored zero is later
        # judged against.
        if self._rest_i_mv is None:
            self._rest_i_mv = float(pin_mv)
        else:
            self._rest_i_mv += (float(pin_mv) - self._rest_i_mv) / 64.0

    async def broadcast_loop(self) -> None:
        period = 1.0 / BROADCAST_HZ
        last_hello_probe = 0.0
        while True:
            await asyncio.sleep(period)
            try:
                # The port can open onto a board that is still booting, or onto
                # a different device entirely. Keep asking until it introduces
                # itself.
                status = self.link.status()
                now = time.monotonic()
                if status["connected"] and not status.get("identified") and now - last_hello_probe > 2.0:
                    last_hello_probe = now
                    self.link.send("?")

                telemetry = self.link.telemetry
                if telemetry:
                    # Force in grams is computed here rather than in the board,
                    # which reports only raw counts. A copy keeps the reader
                    # thread's own dictionary untouched.
                    telemetry = dict(telemetry)
                    telemetry["thrustG"] = self.calibration.thrust_g(telemetry.get("load"))
                    telemetry["mahUsed"] = self._consumption(telemetry)
                    self._track_rest_current(telemetry)
                if telemetry and self.recorder.active:
                    self.recorder.write(telemetry, self.throttle_cmd[1], self.note)

                for event in self.link.drain_events():
                    self._log.append({**event, "at": time.time()})
                del self._log[:-50]

                payload = json.dumps(
                    {
                        "type": "state",
                        "telemetry": telemetry,
                        "link": status,
                        "recorder": self.recorder.status(),
                        "autotest": self.autotest_state,
                        "calibration": self.calibration.status(),
                        "throttleCmd": self.throttle_cmd,
                        "uiAlive": self.ui_alive,
                        "log": self._log[-12:],
                    }
                )
                await self._send_all(payload)
                self.last_broadcast = time.monotonic()
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - a dead loop is worse
                # Never let one bad iteration kill the loop: last_broadcast
                # stops advancing, which stops the watchdog being fed.
                self.log("error", f"telemetry broadcast failed: {exc}")

    async def _send_all(self, payload: str) -> None:
        dead = []
        for client in list(self.clients):
            try:
                await client.send_text(payload)
            except Exception:  # noqa: BLE001 - client vanished mid-send
                dead.append(client)
        for client in dead:
            self.clients.discard(client)


def create_app(link, log_dir: Path) -> FastAPI:
    stand = Stand(link, log_dir)

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        # Deliberately no acquire() here: starting the server must not take the
        # USB port away from anything else. It is claimed when a browser shows
        # up. See docs/PORT_SHARING.md.
        link.start()
        tasks = [
            asyncio.create_task(stand.keepalive_loop()),
            asyncio.create_task(stand.broadcast_loop()),
            asyncio.create_task(stand.reaper_loop()),
        ]
        try:
            yield
        finally:
            for task in tasks:
                task.cancel()
            stand.stop_all()
            link.send("D")
            await asyncio.sleep(0.1)
            link.stop()
            stand.recorder.stop()

    app = FastAPI(title="Motor Thrust Test Stand", lifespan=lifespan)
    app.state.stand = stand

    @app.get("/")
    async def index() -> FileResponse:
        return FileResponse(STATIC_DIR / "index.html")

    @app.get("/api/logs")
    async def list_logs() -> JSONResponse:
        files = sorted(stand.recorder.log_dir.glob("*.csv"), reverse=True)
        return JSONResponse(
            [
                {"name": f.name, "bytes": f.stat().st_size, "mtime": f.stat().st_mtime}
                for f in files[:100]
            ]
        )

    @app.get("/api/logs/{name}")
    async def get_log(name: str) -> FileResponse:
        path = (stand.recorder.log_dir / name).resolve()
        if path.parent != stand.recorder.log_dir.resolve() or not path.is_file():
            raise FileNotFoundError(name)
        return FileResponse(path, media_type="text/csv", filename=path.name)

    @app.post("/api/port/release")
    async def release_port() -> JSONResponse:
        """Free the USB port for a configurator or for flashing."""
        stand.release_port_for(PORT_RELEASE_HOLD_S)
        return JSONResponse({"released": True, "seconds": PORT_RELEASE_HOLD_S})

    @app.post("/api/port/acquire")
    async def acquire_port() -> JSONResponse:
        stand.claim_port()
        return JSONResponse({"acquired": True})

    @app.get("/api/port")
    async def port_status() -> JSONResponse:
        return JSONResponse(stand.link.status())

    @app.websocket("/ws")
    async def websocket(ws: WebSocket) -> None:
        await ws.accept()
        stand.clients.add(ws)
        stand.claim_port()
        stand.last_ui_beat = time.monotonic()
        try:
            while True:
                raw = await ws.receive_text()
                try:
                    message = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if isinstance(message, dict):
                    await stand.handle(message)
        except WebSocketDisconnect:
            pass
        finally:
            stand.clients.discard(ws)
            if not stand.clients:
                # Last window closed: cut throttle immediately rather than
                # waiting for the firmware watchdog to notice, then give the
                # port back so other tools can reach the board.
                stand.end_session("the window was closed")

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
    return app


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Motor thrust test stand server")
    parser.add_argument("--serial-port", default=None, help="FC serial port (auto-detected by default)")
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind address")
    parser.add_argument("--port", type=int, default=8000, help="HTTP port")
    parser.add_argument("--log-dir", default="logs", help="directory for CSV recordings")
    parser.add_argument("--simulate", action="store_true", help="run against a simulated board")
    return parser


def main(argv: Optional[List[str]] = None) -> None:
    import uvicorn

    args = build_parser().parse_args(argv)
    log_dir = Path(args.log_dir).resolve()
    link = (
        SimulatedLink()
        if args.simulate
        else SerialLink(args.serial_port, state_file=log_dir / "board.json")
    )
    app = create_app(link, log_dir)
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
