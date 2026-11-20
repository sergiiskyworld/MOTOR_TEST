"""CSV recording of telemetry samples."""

from __future__ import annotations

import csv
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Optional

COLUMNS = [
    "wall_time",
    "elapsed_s",
    "board_ms",
    "state",
    "protocol",
    "throttle_cmd_pm",
    "throttle_m1_pm",
    "throttle_m3_pm",
    "raw_m1",
    "raw_m3",
    "esc_ok",
    "esc_temp_c",
    "esc_voltage_v",
    "esc_current_a",
    "esc_power_w",
    "esc_consumed_mah",
    "esc_erpm",
    "motor_rpm",
    "board_voltage_v",
    "board_current_a",
    # Raw counts are kept alongside the grams so a run stays usable if the
    # thrust calibration later turns out to have been wrong.
    "thrust_g",
    "thrust_raw",
    "note",
]


class Recorder:
    def __init__(self, log_dir: Path) -> None:
        self.log_dir = log_dir
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self._file = None
        self._writer: Optional[csv.DictWriter] = None
        self._started = 0.0
        self.path: Optional[Path] = None
        self.rows = 0

    @property
    def active(self) -> bool:
        return self._file is not None

    def start(self, name: str = "") -> Path:
        self.stop()
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        safe = "".join(c for c in name if c.isalnum() or c in "-_") or "run"
        self.path = self.log_dir / f"{stamp}-{safe}.csv"
        self._file = self.path.open("w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=COLUMNS)
        self._writer.writeheader()
        self._started = time.monotonic()
        self.rows = 0
        return self.path

    def stop(self) -> Optional[Path]:
        path = self.path
        if self._file is not None:
            self._file.close()
        self._file = None
        self._writer = None
        return path

    def write(self, telemetry: Dict[str, Any], throttle_cmd: int, note: str = "") -> None:
        if self._writer is None or not telemetry:
            return

        esc = telemetry.get("esc", {})
        board = telemetry.get("board", {})
        thrust_g = telemetry.get("thrustG")
        volts = esc.get("mv", 0) / 1000.0
        amps = esc.get("ma", 0) / 1000.0
        throttle = telemetry.get("thr", [0, 0])
        raw = telemetry.get("out", [0, 0])

        self._writer.writerow(
            {
                "wall_time": datetime.now().isoformat(timespec="milliseconds"),
                "elapsed_s": round(time.monotonic() - self._started, 3),
                "board_ms": telemetry.get("t", 0),
                "state": telemetry.get("state", 0),
                "protocol": telemetry.get("proto", ""),
                "throttle_cmd_pm": throttle_cmd,
                "throttle_m1_pm": throttle[0],
                "throttle_m3_pm": throttle[1],
                "raw_m1": raw[0],
                "raw_m3": raw[1],
                "esc_ok": esc.get("ok", 0),
                "esc_temp_c": esc.get("tempC", 0),
                "esc_voltage_v": round(volts, 3),
                "esc_current_a": round(amps, 3),
                "esc_power_w": round(volts * amps, 2),
                # Отсчёт от сброса, как на экране: иначе в файле оказалось бы
                # накопленное с момента подачи питания, к прогону отношения
                # не имеющее.
                "esc_consumed_mah": telemetry.get("mahUsed") or 0,
                "esc_erpm": esc.get("erpm", 0),
                "motor_rpm": esc.get("rpm", 0),
                "board_voltage_v": round(board.get("mv", 0) / 1000.0, 3),
                "board_current_a": round(board.get("ma", 0) / 1000.0, 3),
                "thrust_g": "" if thrust_g is None else round(thrust_g, 1),
                "thrust_raw": (telemetry.get("load") or {}).get("raw", ""),
                "note": note,
            }
        )
        self.rows += 1
        if self._file is not None and self.rows % 50 == 0:
            self._file.flush()

    def status(self) -> Dict[str, Any]:
        return {
            "active": self.active,
            "file": self.path.name if self.path else None,
            "rows": self.rows,
        }
