#!/usr/bin/env python3
"""Послушать плату напрямую, в обход сервера.

Печатает сырые строки как они приходят. Полезно, когда сервер видит плату, но
чего-то не получает: сразу видно, шлёт ли плата вообще и валидный ли это JSON.

    python server/tests/raw_listen.py [--seconds 5] [--send '?']
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import serial  # noqa: E402

from standserver.link import find_port  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None)
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--send", action="append", default=[])
    parser.add_argument("--max-lines", type=int, default=15)
    args = parser.parse_args()

    device = args.port or find_port()
    if not device:
        print("плата не найдена", file=sys.stderr)
        return 1
    print(f"порт: {device}")

    with serial.Serial(device, 115200, timeout=0.1) as port:
        time.sleep(0.2)
        port.reset_input_buffer()
        for command in args.send:
            print(f"-> {command}")
            port.write((command + "\n").encode())
            time.sleep(0.05)

        deadline = time.monotonic() + args.seconds
        buffer = b""
        lines = 0
        good = 0
        bad = 0
        total_bytes = 0

        while time.monotonic() < deadline:
            chunk = port.read(512)
            if not chunk:
                continue
            total_bytes += len(chunk)
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").strip()
                if not text:
                    continue
                lines += 1
                try:
                    json.loads(text)
                    good += 1
                    marker = "ok "
                except json.JSONDecodeError as exc:
                    bad += 1
                    marker = f"BAD({exc.msg} @ {exc.pos})"
                if lines <= args.max_lines:
                    print(f"{marker} [{len(text):4d}] {text[:400]}")

        print()
        print(f"байт получено:  {total_bytes}")
        print(f"строк:          {lines}  (валидный JSON: {good}, битых: {bad})")
        if buffer:
            print(f"хвост без перевода строки: [{len(buffer)}] {buffer[:200]!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
