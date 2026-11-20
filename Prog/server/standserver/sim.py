"""Software stand-in for the flight controller.

Exposes the same duck-typed interface as SerialLink and reproduces the
firmware's state machine, throttle ramp, link watchdog and telemetry shape.
Used by `--simulate` so the UI and the safety chain can be exercised without
putting a real propeller in the room.
"""

from __future__ import annotations

import math
import threading
import time
from collections import deque
from typing import Any, Deque, Dict, List

PROTOCOLS = [
    "PWM50",
    "PWM100",
    "PWM200",
    "PWM400",
    "DSHOT150",
    "DSHOT300",
    "DSHOT600",
]

STATE_SAFE, STATE_ARMED, STATE_FAULT = 0, 1, 2

FAULT_LINK_TIMEOUT = 1 << 0
FAULT_ESTOP = 1 << 1
FAULT_OVERCURRENT = 1 << 2
FAULT_OVERTEMP = 1 << 3

LINK_TIMEOUT_S = 0.3
TICK_S = 0.01

# A plausible load cell: a big reading with nothing on it, and a small change
# per gram. The numbers only matter in that they are wrong until calibrated,
# which is exactly what the calibration dialog is for.
LOAD_ZERO_COUNTS = 128000
LOAD_COUNTS_PER_G = 42.0

# Как на самом деле устроены датчики платы в этой модели. Значения нарочно не
# совпадают с настройками по умолчанию: из коробки плата немного врёт, и
# калибровка по внешнему прибору видимо это исправляет — иначе проверить её
# без железа было бы нечем.
SENSE_VBAT_DIVIDER = 8.05     # вольт на вольт делителя
SENSE_CURRENT_A_PER_V = 85.0  # ампер на вольт датчика тока
SENSE_CURRENT_ZERO_MV = 40.0  # что датчик выдаёт при нулевом токе


class SimulatedLink:
    def __init__(self, port: Any = None, baud: int = 115200) -> None:
        self.port_name = "simulated"
        self.connected = True
        self.rx_errors = 0
        self.events: Deque[Dict[str, Any]] = deque(maxlen=200)
        self.last_telemetry_at = 0.0
        self.telemetry: Dict[str, Any] = {}

        self.hello = {
            "hello": "thrust-stand-fc",
            "version": "1.0.0-sim",
            "board": "OrqaF405Pro",
            "motors": [1, 3],
            "protocols": PROTOCOLS,
            "controlHz": 2000,
            "linkTimeoutMs": int(LINK_TIMEOUT_S * 1000),
        }
        self.settings: Dict[str, Any] = {
            "maxThrottle": 1000,
            "rampPerS": 500,
            "motorPoles": 14,
            "vbatScaleM": 8300,
            "currentScaleM": 92600,
            "currentOffsetMv": 0,
            "currentLimitMa": 0,
            "tempLimitC": 0,
            # Настоящая плата сообщает, совпадает ли то, что в оперативной
            # памяти, с тем, что записано во флеш. Только это и отвечает на
            # вопрос, переживёт ли калибровка выключение питания.
            "saved": True,
        }
        self._save_pending = False

        self._state = STATE_SAFE
        self._faults = 0
        # То же значение, с которым поднимается настоящая плата
        # (firmware/src/motor_out.cpp, gProtocol). Расхождение здесь означало
        # бы, что симуляция показывает протокол, которого на стенде не бывает.
        self._protocol = "DSHOT300"
        self._target = [0.0, 0.0]
        self._applied = [0.0, 0.0]
        self._last_host = time.monotonic()
        self._temp = 24.0
        self._mah = 0.0
        self._started = time.monotonic()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        self.events.append({"level": "info", "msg": "simulated board attached"})

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)

    # There is no real port to fight over, so the session calls are no-ops that
    # keep the duck type identical to SerialLink.
    def acquire(self) -> None:
        return

    def release(self, hold_off: float = 0.0) -> None:
        return

    @property
    def wanted(self) -> bool:
        return True

    # -- command interface -------------------------------------------------

    def keepalive(self) -> None:
        self.send("K")

    def send(self, line: str) -> None:
        with self._lock:
            self._handle(line.strip())

    def _handle(self, line: str) -> None:
        if not line:
            return
        self._last_host = time.monotonic()
        cmd, _, args = line.partition(" ")
        head = cmd[:1].upper()
        rest = (cmd[1:] + " " + args).strip()
        parts = rest.split()

        if head == "K":
            return
        if head == "?":
            return
        if head == "A":
            if self._faults == 0 and all(t == 0 for t in self._target):
                self._state = STATE_ARMED
            else:
                self.events.append({"level": "warn", "msg": "arm rejected"})
        elif head == "D":
            self._cut()
            if self._state == STATE_ARMED:
                self._state = STATE_SAFE
        elif head == "X":
            self._faults |= FAULT_ESTOP
            self._state = STATE_FAULT
            self._cut()
        elif head == "R":
            if self._state == STATE_FAULT:
                self._faults = 0
                self._state = STATE_SAFE
                self._cut()
        elif head == "T" and len(parts) >= 2:
            index = 0 if parts[0] == "1" else 1
            value = max(0, min(int(parts[1]), int(self.settings["maxThrottle"])))
            self._target[index] = float(value)
        elif head == "P" and parts:
            if parts[0].upper() in PROTOCOLS:
                self._protocol = parts[0].upper()
                self._cut()
                self._state = STATE_SAFE
                self._faults = 0
        elif head == "S" and len(parts) >= 2:
            if parts[0] in self.settings:
                self.settings[parts[0]] = int(parts[1])
                self.settings["saved"] = False
        elif head == "W":
            # The real board refuses to erase flash while anything spins,
            # because that freezes the processor for about a second. The model
            # reproduces the refusal rather than the delay.
            if self.settings["saved"]:
                self.events.append({"level": "info", "msg": "настройки в плате уже такие же"})
            elif self._state == STATE_ARMED:
                self._save_pending = True
                self.events.append(
                    {
                        "level": "warn",
                        "msg": "настройки сохранятся в плату, как только мотор остановится",
                    }
                )
            else:
                self._commit_settings()

    def _commit_settings(self) -> None:
        self._save_pending = False
        self.settings["saved"] = True
        self.events.append({"level": "info", "msg": "настройки сохранены в плату"})

    def _cut(self) -> None:
        self._target = [0.0, 0.0]
        self._applied = [0.0, 0.0]

    # -- physics -----------------------------------------------------------

    def _loop(self) -> None:
        while not self._stop.is_set():
            time.sleep(TICK_S)
            with self._lock:
                self._tick()

    def _tick(self) -> None:
        now = time.monotonic()

        if self._state == STATE_ARMED and (now - self._last_host) > LINK_TIMEOUT_S:
            self._faults |= FAULT_LINK_TIMEOUT
            self._state = STATE_FAULT
            self._cut()

        step = float(self.settings["rampPerS"]) * TICK_S
        for i in range(2):
            target = self._target[i] if self._state == STATE_ARMED else 0.0
            delta = target - self._applied[i]
            if abs(delta) <= step:
                self._applied[i] = target
            else:
                self._applied[i] += math.copysign(step, delta)

        thr = self._applied[0] / 1000.0
        pack_v = 25.2 - 3.2 * thr**2
        current_a = 62.0 * thr**2.1
        rpm = 21000.0 * math.sqrt(thr) if thr > 0 else 0.0
        erpm = rpm * (self.settings["motorPoles"] / 2)

        self._temp += (24.0 + current_a * 1.1 - self._temp) * 0.01
        self._mah += current_a * 1000.0 * TICK_S / 3600.0

        limit_ma = int(self.settings["currentLimitMa"])
        if self._state == STATE_ARMED and limit_ma > 0 and current_a * 1000 > limit_ma:
            self._faults |= FAULT_OVERCURRENT
            self._state = STATE_FAULT
            self._cut()

        limit_c = int(self.settings["tempLimitC"])
        if self._state == STATE_ARMED and limit_c > 0 and self._temp >= limit_c:
            self._faults |= FAULT_OVERTEMP
            self._state = STATE_FAULT
            self._cut()

        if self._save_pending and self._state != STATE_ARMED and not any(self._applied):
            self._commit_settings()

        # Выводы АЦП, как их видит плата, и уже из них — то, что она покажет.
        # Порядок именно такой, а не наоборот: прошивка считает так же
        # (firmware/src/main.cpp, sampleAnalog), поэтому смена масштаба здесь
        # меняет показания ровно так же, как на железе.
        v_pin_mv = pack_v * 1000.0 / SENSE_VBAT_DIVIDER
        i_pin_mv = SENSE_CURRENT_ZERO_MV + current_a * 1000.0 / SENSE_CURRENT_A_PER_V
        board_mv = v_pin_mv * self.settings["vbatScaleM"] / 1000.0
        span_mv = max(0.0, i_pin_mv - self.settings["currentOffsetMv"])
        board_ma = span_mv * self.settings["currentScaleM"] / 1000.0

        dshot = self._protocol.startswith("DSHOT")
        raw = [
            int(48 + 1999 * a / 1000) if (dshot and a > 0) else (0 if dshot else int(1000 + a))
            for a in self._applied
        ]

        self.telemetry = {
            "t": int((now - self._started) * 1000),
            "state": self._state,
            "faults": self._faults,
            "proto": self._protocol,
            "thr": [int(self._applied[0]), int(self._applied[1])],
            "out": raw,
            "esc": {
                "ok": 1,
                "ageMs": 12,
                "tempC": int(self._temp),
                "mv": int(pack_v * 1000),
                "ma": int(current_a * 1000),
                "mah": int(self._mah),
                "erpm": int(erpm),
                "rpm": int(rpm),
                "pkts": int((now - self._started) * 33),
                "crcErr": 0,
            },
            "board": {
                "mv": int(board_mv),
                "ma": int(board_ma),
                "vPinMv": int(v_pin_mv),
                "iPinMv": int(i_pin_mv),
            },
            # Raw counts from a load cell amplifier, the way the firmware will
            # report them: a large offset with no load and roughly forty counts
            # per gram. Deliberately not converted to grams here - turning
            # counts into force is the calibration's job.
            "load": {"raw": int(LOAD_ZERO_COUNTS + 3400.0 * thr**1.85 * LOAD_COUNTS_PER_G)},
        }
        self.last_telemetry_at = now

    # -- status ------------------------------------------------------------

    def status(self) -> Dict[str, Any]:
        return {
            "connected": True,
            "streaming": True,
            "identified": True,
            "wanted": True,
            "heldOffFor": 0.0,
            "port": self.port_name,
            "board": self.hello,
            "settings": self.settings,
            "rxErrors": 0,
            "simulated": True,
        }

    def drain_events(self) -> List[Dict[str, Any]]:
        out = []
        while self.events:
            out.append(self.events.popleft())
        return out
