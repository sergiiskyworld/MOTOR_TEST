#!/usr/bin/env python3
"""End-to-end check of the server against the simulated board.

Exercises the sequences that matter for safety: arming refuses a non-zero
throttle, the ramp is rate limited, the protective cutoffs latch a fault, and
dropping the heartbeat stops the motor.
"""

from __future__ import annotations

import json
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient  # noqa: E402

from standserver.main import UI_DEAD_S, create_app  # noqa: E402
from standserver.sim import SimulatedLink  # noqa: E402

failures: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  -> ' + detail if detail and not ok else ''}")
    if not ok:
        failures.append(name)


def wait_for(ws, predicate, timeout=4.0):
    """Pump the socket until the board state satisfies predicate."""
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        ws.send_text(json.dumps({"type": "hb"}))
        message = json.loads(ws.receive_text())
        last = message.get("telemetry") or {}
        if last and predicate(last, message):
            return last, message
        time.sleep(0.01)
    return last, {}


def main() -> int:
    link = SimulatedLink()
    log_dir = Path("/tmp/thrust-stand-smoke")
    # The thrust calibration is saved on purpose, so a previous run would
    # otherwise leave this one starting from an already calibrated sensor.
    (log_dir / "calibration.json").unlink(missing_ok=True)
    app = create_app(link, log_dir)

    with TestClient(app) as client:
        response = client.get("/")
        check("index page served", response.status_code == 200 and "Стенд тяги" in response.text)

        with client.websocket_connect("/ws") as ws:
            telemetry, _ = wait_for(ws, lambda t, m: t.get("state") is not None)
            check("telemetry stream starts", bool(telemetry), str(telemetry))
            check("starts disarmed", telemetry.get("state") == 0, str(telemetry.get("state")))

            # Arming must not be possible while a throttle command is standing.
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 400}))
            time.sleep(0.05)
            ws.send_text(json.dumps({"type": "arm"}))
            telemetry, _ = wait_for(ws, lambda t, m: True, timeout=0.5)
            check("arm refused with throttle standing", telemetry.get("state") == 0,
                  f"state={telemetry.get('state')}")

            # The UI zeroes the throttle before arming, which the server mirrors.
            ws.send_text(json.dumps({"type": "arm"}))
            telemetry, _ = wait_for(ws, lambda t, m: t.get("state") == 1)
            check("arms after throttle is cleared", telemetry.get("state") == 1)

            # Ramp limiting: 500 per-mille/s means 300 per-mille takes ~0.6 s.
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 300}))
            started = time.monotonic()
            telemetry, _ = wait_for(ws, lambda t, m: t["thr"][0] >= 299)
            elapsed = time.monotonic() - started
            check("throttle ramp is rate limited", 0.4 < elapsed < 1.2, f"{elapsed:.2f}s")
            check("motor reports rpm under throttle", telemetry["esc"]["rpm"] > 1000,
                  str(telemetry["esc"]["rpm"]))

            # Emergency stop must latch a fault, not just idle the motor.
            ws.send_text(json.dumps({"type": "estop"}))
            telemetry, _ = wait_for(ws, lambda t, m: t.get("state") == 2)
            check("estop latches a fault", telemetry.get("state") == 2 and telemetry["faults"] & 0x2)
            check("estop cuts throttle instantly", telemetry["thr"][0] == 0, str(telemetry["thr"]))

            ws.send_text(json.dumps({"type": "arm"}))
            telemetry, _ = wait_for(ws, lambda t, m: True, timeout=0.5)
            check("cannot arm while faulted", telemetry.get("state") == 2)

            ws.send_text(json.dumps({"type": "reset"}))
            telemetry, _ = wait_for(ws, lambda t, m: t.get("state") == 0)
            check("fault clears on reset", telemetry.get("state") == 0)

            # Current cutoff.
            ws.send_text(json.dumps({"type": "setting", "key": "currentLimitMa", "value": 15000}))
            ws.send_text(json.dumps({"type": "arm"}))
            wait_for(ws, lambda t, m: t.get("state") == 1)
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 800}))
            telemetry, _ = wait_for(ws, lambda t, m: t.get("state") == 2, timeout=5.0)
            check("overcurrent cutoff trips", telemetry.get("state") == 2 and telemetry["faults"] & 0x4,
                  f"state={telemetry.get('state')} faults={telemetry.get('faults')}")

            ws.send_text(json.dumps({"type": "setting", "key": "currentLimitMa", "value": 0}))
            ws.send_text(json.dumps({"type": "reset"}))
            wait_for(ws, lambda t, m: t.get("state") == 0)

            # Recording produces a CSV with the expected header.
            ws.send_text(json.dumps({"type": "record", "action": "start", "name": "smoke"}))
            ws.send_text(json.dumps({"type": "arm"}))
            wait_for(ws, lambda t, m: t.get("state") == 1)
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 200}))
            wait_for(ws, lambda t, m: m.get("recorder", {}).get("rows", 0) > 20)
            ws.send_text(json.dumps({"type": "record", "action": "stop"}))
            _, message = wait_for(ws, lambda t, m: not m.get("recorder", {}).get("active", True))
            name = message["recorder"]["file"]
            csv_text = client.get(f"/api/logs/{name}").text
            header = csv_text.splitlines()[0]
            check("csv recorded with telemetry columns",
                  "motor_rpm" in header and "esc_power_w" in header and len(csv_text.splitlines()) > 20,
                  header)
            check("csv carries thrust alongside the raw counts",
                  "thrust_g" in header and "thrust_raw" in header, header)

            # Расход отсчитывается от сброса: счётчик внутри регулятора считает
            # от подачи питания и обнулить его нечем, так что без отметки
            # плитка показывала бы ампер-часы прошлых прогонов.
            _, message = wait_for(ws, lambda t, m: (t.get("esc") or {}).get("mah", 0) > 0)
            ws.send_text(json.dumps({"type": "mahReset"}))
            _, message = wait_for(
                ws, lambda t, m: t.get("mahUsed") == 0 and (t.get("esc") or {}).get("mah", 0) > 0
            )
            check("consumption restarts from zero when reset, while the ESC keeps counting",
                  message.get("telemetry", {}).get("mahUsed") == 0
                  and message.get("telemetry", {}).get("esc", {}).get("mah", 0) > 0,
                  str(message.get("telemetry", {}).get("esc", {}).get("mah")))

            # Calibration. Thrust is meaningless until the operator has weighed
            # something, so it must read as absent rather than as zero.
            _, message = wait_for(ws, lambda t, m: "calibration" in m)
            check("thrust is not reported before calibration",
                  message["calibration"]["loadCalibrated"] is False
                  and message["telemetry"].get("thrustG") is None,
                  str(message.get("calibration")))

            # Tare with the motor stopped, then hold a steady throttle and tell
            # the server what the reference scale says the force is.
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 0}))
            wait_for(ws, lambda t, m: t["thr"][0] == 0)
            ws.send_text(json.dumps({"type": "calibrate", "target": "thrustZero"}))
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 500}))
            wait_for(ws, lambda t, m: t["thr"][0] == 500)
            ws.send_text(json.dumps({"type": "calibrate", "target": "thrust", "value": 1500}))
            _, message = wait_for(ws, lambda t, m: t.get("thrustG") is not None)
            shown = (message.get("telemetry") or {}).get("thrustG")
            check("thrust reads the reference weight after calibration",
                  shown is not None and abs(shown - 1500) < 20, str(shown))

            # Voltage: tell the board the pack is 20.00 V and it must then
            # report 20.00 V, whatever its scale was before.
            _, message = wait_for(ws, lambda t, m: (t.get("board") or {}).get("vPinMv"))
            ws.send_text(json.dumps({"type": "calibrate", "target": "voltage", "value": 20.0}))
            _, message = wait_for(
                ws, lambda t, m: abs((t.get("board") or {}).get("mv", 0) - 20000) < 60, timeout=3.0
            )
            shown = (message.get("telemetry") or {}).get("board", {}).get("mv")
            check("voltage reads what the meter said after calibration",
                  shown is not None and abs(shown - 20000) < 60, str(shown))

            # Current: zero the sensor at a standstill, then run the motor and
            # declare the clamp meter's reading. The model's sensor has a real
            # offset, so skipping the zeroing step would poison the scale.
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 0}))
            wait_for(ws, lambda t, m: t["thr"][0] == 0 and (t.get("esc") or {}).get("ma") == 0)
            ws.send_text(json.dumps({"type": "calibrate", "target": "currentZero"}))
            _, message = wait_for(
                ws, lambda t, m: (t.get("board") or {}).get("ma") == 0, timeout=3.0
            )
            check("current reads zero after zeroing at a standstill",
                  (message.get("telemetry") or {}).get("board", {}).get("ma") == 0,
                  str((message.get("telemetry") or {}).get("board")))

            # Масштаб — это отношение сигнала к показанию прибора, и на
            # стоящем моторе сигнала нет. Без отказа здесь собственный ноль
            # датчика превратился бы в правдоподобный с виду коэффициент.
            before = link.settings["currentScaleM"]
            ws.send_text(json.dumps({"type": "calibrate", "target": "current", "value": 12.0}))
            _, message = wait_for(
                ws,
                lambda t, m: any("мВ над нулём" in e.get("msg", "") for e in (m.get("log") or [])),
                timeout=3.0,
            )
            check("calibrating current at a standstill is refused, and says why",
                  link.settings["currentScaleM"] == before,
                  str(link.settings["currentScaleM"]))

            # Под нагрузкой: сообщаем одно показание образцового амперметра,
            # как это сделал бы человек с клещами, и убеждаемся, что плата
            # после этого показывает именно его.
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 400}))
            _, message = wait_for(ws, lambda t, m: t["thr"][0] == 400 and (t.get("esc") or {})["ma"] > 5000)
            true_ma = message["telemetry"]["esc"]["ma"]
            ws.send_text(json.dumps({
                "type": "calibrate", "target": "current", "value": true_ma / 1000.0,
            }))
            _, message = wait_for(
                ws,
                lambda t, m: abs((t.get("board") or {}).get("ma", 0) - true_ma) < true_ma * 0.02,
                timeout=3.0,
            )
            shown = (message.get("telemetry") or {}).get("board", {}).get("ma")
            check("current reads what the clamp meter said after calibration",
                  shown is not None and abs(shown - true_ma) < true_ma * 0.02,
                  f"{shown} vs {true_ma}")

            # A coefficient that only lives in RAM is lost with the battery,
            # so the board must be asked to keep it - and must refuse to erase
            # flash until the motor has stopped.
            check("the board reports the calibration as unsaved while running",
                  link.settings["saved"] is False, str(link.settings.get("saved")))
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 0}))
            ws.send_text(json.dumps({"type": "disarm"}))
            wait_for(ws, lambda t, m: link.settings["saved"] is True, timeout=3.0)
            check("the calibration is written to the board once it stops",
                  link.settings["saved"] is True, str(link.settings.get("saved")))

            # Ноль, снятый под нагрузкой, унёс бы эту нагрузку в каждое
            # последующее показание.
            ws.send_text(json.dumps({"type": "arm"}))
            wait_for(ws, lambda t, m: t.get("state") == 1)
            before = link.settings["currentOffsetMv"]
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 300}))
            wait_for(ws, lambda t, m: t["thr"][0] == 300)
            ws.send_text(json.dumps({"type": "calibrate", "target": "currentZero"}))
            wait_for(
                ws,
                lambda t, m: any("только на неподвижном" in e["msg"] for e in m.get("log", [])),
            )
            check("zeroing is refused while the motor is running",
                  link.settings["currentOffsetMv"] == before,
                  str(link.settings["currentOffsetMv"]))
            ws.send_text(json.dumps({"type": "throttle", "motor": 1, "value": 0}))
            wait_for(ws, lambda t, m: t["thr"][0] == 0)

            # If telemetry fan-out breaks, the operator goes blind. Feeding the
            # watchdog in that state would leave a motor spinning unseen, so
            # the keepalive must stop too.
            ws.send_text(json.dumps({"type": "arm"}))
            wait_for(ws, lambda t, m: t.get("state") == 1)
            stand = app.state.stand
            stand.last_broadcast = 0.0  # pretend the broadcast loop stalled
            time.sleep(0.8)
            check("watchdog is released when telemetry fan-out stalls",
                  link.telemetry["state"] == 2 and bool(link.telemetry["faults"] & 0x1),
                  f"state={link.telemetry['state']} faults={link.telemetry['faults']}")

            ws.send_text(json.dumps({"type": "reset"}))
            wait_for(ws, lambda t, m: t.get("state") == 0)
            ws.send_text(json.dumps({"type": "disarm"}))

        # Socket closed: the server must stop the motor on its own.
        time.sleep(0.3)
        check("motor stopped after the browser disconnects",
              link.telemetry["state"] != 1 and link.telemetry["thr"][0] == 0,
              str(link.telemetry.get("thr")))

        # A phone that falls asleep, or a browser that crashes, leaves the
        # socket open with nobody behind it. Waiting for TCP to notice takes
        # minutes, during which the USB port stays held, so the heartbeat has
        # to be what ends the session.
        stand = app.state.stand
        try:
            with client.websocket_connect("/ws") as ws:
                wait_for(ws, lambda t, m: t.get("state") is not None)
                ws.send_text(json.dumps({"type": "arm"}))
                wait_for(ws, lambda t, m: t.get("state") == 1)
                check("armed before going silent", link.telemetry["state"] == 1)
                # Deliberately send nothing at all from here on.
                time.sleep(UI_DEAD_S + 1.5)
        except Exception:  # noqa: BLE001 - the server hangs up, which is the point
            pass
        check("a silent browser session is closed by the server", not stand.clients,
              f"{len(stand.clients)} client(s) left")
        check("motor stopped when the browser went silent",
              link.telemetry["state"] != 1 and link.telemetry["thr"][0] == 0,
              str(link.telemetry.get("thr")))

    # Watchdog: arm the simulated board, then go silent.
    link2 = SimulatedLink()
    link2.start()
    link2.send("A")
    link2.send("T 1 500")
    time.sleep(0.2)
    armed = link2.telemetry["state"] == 1
    time.sleep(0.6)
    faulted = link2.telemetry["state"] == 2 and (link2.telemetry["faults"] & 0x1)
    link2.stop()
    check("board arms when driven", armed)
    check("watchdog faults the board when the host goes quiet", bool(faulted),
          str(link2.telemetry.get("faults")))

    # Port sharing: docs/PORT_SHARING.md. These guard the rules that made every
    # ArduPilot configurator on the machine unusable when they were missing.
    from standserver.link import SAFE_BEFORE_IDENTIFY, SerialLink, find_port

    link3 = SerialLink(port="/dev/null-does-not-exist")
    check("port is not wanted until a session asks for it", not link3.wanted)
    link3.acquire()
    check("port is wanted once a session starts", link3.wanted)
    link3.release()
    check("port is given up when the session ends", not link3.wanted)
    link3.acquire()
    link3.release(hold_off=30.0)
    check("release with hold-off keeps the port free", not link3.wanted)
    link3.acquire()
    check("acquire overrides an active hold-off", link3.wanted)

    # An unidentified device must not be commanded: it could be somebody's
    # aircraft with props on.
    link4 = SerialLink(port="/dev/null-does-not-exist")
    check("only the identification probe is safe before identify",
          SAFE_BEFORE_IDENTIFY == {"?"}, str(SAFE_BEFORE_IDENTIFY))

    # There must be no "grab the first usbmodem" fallback any more.
    class FakePort:
        def __init__(self, device, vid, pid, serial_number=None):
            self.device, self.vid, self.pid = device, vid, pid
            self.serial_number = serial_number

    import standserver.link as link_mod

    real_comports = link_mod.list_ports.comports
    try:
        link_mod.list_ports.comports = lambda: [FakePort("/dev/cu.usbmodemFOO", 0x1234, 0x5678)]
        check("a foreign usbmodem is never picked", find_port() is None)

        link_mod.list_ports.comports = lambda: [
            FakePort("/dev/cu.usbmodemA", 0x0483, 0x5740, "AAA"),
            FakePort("/dev/cu.usbmodemB", 0x0483, 0x5740, "BBB"),
        ]
        check("the remembered board wins over another ST device",
              find_port("BBB") == "/dev/cu.usbmodemB", str(find_port("BBB")))
        check("a rejected port is skipped",
              find_port(None, {"/dev/cu.usbmodemA"}) == "/dev/cu.usbmodemB")
    finally:
        link_mod.list_ports.comports = real_comports

    # The reader thread owns opening and closing the port, so it has to outlive
    # every error. It once died on a TypeError thrown from inside pyserial when
    # the handle was closed underneath its blocking read, and with it went the
    # only code that could release or reopen the board.
    class FlakyPort:
        """Fails its read the way pyserial does after a concurrent close()."""

        def __init__(self, *args, **kwargs):
            self.closed = False
            self.fail_next_read = False

        def read(self, size):
            if self.fail_next_read:
                self.fail_next_read = False
                raise TypeError("'NoneType' object cannot be interpreted as an integer")
            time.sleep(0.02)
            return b""

        def write(self, data):
            return len(data)

        def close(self):
            self.closed = True

    opened: list[FlakyPort] = []

    def fake_serial(*args, **kwargs):
        port = FlakyPort()
        opened.append(port)
        return port

    real_serial = link_mod.serial.Serial
    link5 = SerialLink(port="/dev/fake-board")
    try:
        link_mod.serial.Serial = fake_serial
        link5.start()
        link5.acquire()
        deadline = time.monotonic() + 2.0
        while not link5.connected and time.monotonic() < deadline:
            time.sleep(0.02)
        check("port opens when a session asks for it", link5.connected)

        opened[-1].fail_next_read = True
        time.sleep(0.8)
        check("the reader thread survives an unexpected pyserial error",
              any(thread.is_alive() for thread in link5._threads))
        check("the port is reopened after such an error", len(opened) > 1, f"{len(opened)} open(s)")

        link5.release()
        check("release closes the port handle", opened[-1].closed and not link5.connected,
              f"closed={opened[-1].closed} connected={link5.connected}")
    finally:
        link5.stop()
        link_mod.serial.Serial = real_serial

    print()
    if failures:
        print(f"{len(failures)} check(s) failed: {', '.join(failures)}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
