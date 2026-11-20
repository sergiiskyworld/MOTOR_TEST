"""Calibration of the measured channels against an external instrument.

Two different things live here, because the two channels are measured in two
different places.

Voltage and current are read by the board's own ADC, and the board already has
settings for their scale and zero offset. Calibrating them means computing a
new scale from "what the board shows" versus "what the multimeter shows" and
storing it in the board, so the correction survives a server restart and
applies to the protective cutoffs too.

Thrust has no hardware support yet: the firmware does not read a load cell, so
there is nothing to store in the board. Its calibration is kept here, in a file
next to the recordings, and applied to the raw reading as soon as the board
starts reporting one.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Optional

# One step of the 12-bit ADC on a 3.3 V reference. Everything below is noise,
# and dividing by noise turns a rounding error into a wildly wrong scale.
ADC_STEP_MV = 3300.0 / 4095.0

# A scale is only as good as the signal it is derived from: two ADC steps is
# the least that can be called a measurement at all.
MIN_SENSE_MV = 2.0 * ADC_STEP_MV


class CalibrationError(ValueError):
    """Raised with a message meant to be shown to the operator as is."""


class Calibration:
    """Thrust calibration, persisted as JSON."""

    def __init__(self, path: Optional[Path] = None) -> None:
        self._path = path
        # None means "never calibrated": the raw load-cell counts are
        # meaningless as grams, so the UI shows a dash rather than a number
        # that looks like a measurement.
        self.load_scale: Optional[float] = None
        self.load_zero: float = 0.0
        self._load()

    # -- persistence -------------------------------------------------------

    def _load(self) -> None:
        if not self._path or not self._path.is_file():
            return
        try:
            data = json.loads(self._path.read_text())
        except (OSError, ValueError):
            return
        scale = data.get("loadScale")
        self.load_scale = float(scale) if scale else None
        self.load_zero = float(data.get("loadZero", 0.0))

    def save(self) -> None:
        if not self._path:
            return
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            self._path.write_text(
                json.dumps(
                    {"loadScale": self.load_scale, "loadZero": self.load_zero},
                    indent=2,
                )
            )
        except OSError:
            pass

    # -- thrust ------------------------------------------------------------

    def thrust_g(self, load: Optional[Dict[str, Any]]) -> Optional[float]:
        """Grams from the raw load-cell reading, or None if not measurable."""
        if not load or self.load_scale is None:
            return None
        raw = load.get("raw")
        if raw is None:
            return None
        return (float(raw) - self.load_zero) * self.load_scale

    def set_load_zero(self, load: Optional[Dict[str, Any]]) -> float:
        raw = (load or {}).get("raw")
        if raw is None:
            raise CalibrationError("плата не сообщает показания датчика тяги")
        self.load_zero = float(raw)
        self.save()
        return self.load_zero

    def set_load_measured(self, load: Optional[Dict[str, Any]], grams: float) -> float:
        raw = (load or {}).get("raw")
        if raw is None:
            raise CalibrationError("плата не сообщает показания датчика тяги")
        if grams <= 0:
            raise CalibrationError("вес должен быть больше нуля")
        span = float(raw) - self.load_zero
        if abs(span) < 1:
            raise CalibrationError(
                "показания датчика не изменились относительно нуля — "
                "сначала обнулите его без груза"
            )
        self.load_scale = grams / span
        self.save()
        return self.load_scale

    def status(self) -> Dict[str, Any]:
        return {
            "loadScale": self.load_scale,
            "loadZero": self.load_zero,
            "loadCalibrated": self.load_scale is not None,
        }


# -- board channels ---------------------------------------------------------
#
# The firmware computes (see firmware/src/main.cpp, sampleAnalog):
#
#     voltage_mv = pin_mv * vbatScaleM / 1000
#     current_ma = (pin_mv - currentOffsetMv) * currentScaleM / 1000
#
# Both corrections are derived from the raw pin voltage rather than from the
# displayed value. Going through the displayed value would mean dividing by a
# number the old, wrong scale produced, so every calibration would inherit the
# rounding of the one before it. Worse, the current reading is clamped at zero
# in the firmware: once the zero offset sits above the signal, the displayed
# value is zero no matter how much current flows, and no ratio can recover the
# scale from it. The raw pin voltage has neither problem.


def voltage_scale(pin_mv: float, measured_v: float) -> int:
    if measured_v <= 0:
        raise CalibrationError("измеренное напряжение должно быть больше нуля")
    if pin_mv < MIN_SENSE_MV:
        raise CalibrationError(
            f"на делителе напряжения всего {pin_mv:.1f} мВ — подключите "
            "аккумулятор и повторите"
        )
    return max(1, round(measured_v * 1_000_000.0 / pin_mv))







def current_scale(pin_mv: float, offset_mv: float, measured_a: float) -> int:
    """Масштаб тока по одному показанию образцового амперметра.

    То же, что делает ArduPilot при калибровке `BATT_AMP_PERVLT`: берётся
    напряжение на выводе за вычетом нуля канала и приводится к току, который
    показал прибор. Считается от сырых милливольт, а не от того, что выводит
    плата, иначе новый масштаб унаследовал бы ошибку старого.
    """
    if measured_a <= 0:
        raise CalibrationError("ток по прибору должен быть больше нуля")
    span = pin_mv - offset_mv
    if span < MIN_SENSE_MV:
        raise CalibrationError(
            f"сигнал с датчика всего {span:.1f} мВ над нулём — дайте мотору "
            "рабочий ток и повторите"
        )
    return max(1, round(measured_a * 1_000_000.0 / span))


def current_zero_offset(pin_mv: float) -> int:
    """The sense voltage at rest is, by definition, the zero of the channel."""
    return int(round(pin_mv))
