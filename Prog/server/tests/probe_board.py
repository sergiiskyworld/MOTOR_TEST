#!/usr/bin/env python3
"""Проверка живой платы через работающий сервер.

Подключается к серверу по WebSocket, держит пульс и печатает, что сообщает
плата. Газ не подаётся, команда arm не отправляется.

    python server/tests/probe_board.py [--seconds 6] [--url ws://127.0.0.1:8000/ws]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys

STATE_NAMES = {0: "БЕЗОПАСНО", 1: "ГАЗ ПОДАН", 2: "АВАРИЯ"}


async def probe(url: str, seconds: float) -> int:
    try:
        import websockets
    except ImportError:
        print("нужен пакет websockets: pip install websockets", file=sys.stderr)
        return 2

    async with websockets.connect(url) as ws:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + seconds
        last = None
        frames = 0

        async def heartbeat() -> None:
            while loop.time() < deadline:
                await ws.send(json.dumps({"type": "hb"}))
                await asyncio.sleep(0.1)

        beat = asyncio.create_task(heartbeat())
        try:
            while loop.time() < deadline:
                remaining = max(deadline - loop.time(), 0.01)
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
                except asyncio.TimeoutError:
                    break
                message = json.loads(raw)
                if message.get("type") == "state":
                    last = message
                    frames += 1
        finally:
            beat.cancel()

    if not last:
        print("сервер не прислал ни одного состояния")
        return 1

    link = last.get("link", {})
    board = link.get("board", {})
    telemetry = last.get("telemetry") or {}
    esc = telemetry.get("esc", {})

    print(f"кадров от сервера:   {frames}")
    print(f"порт:                {link.get('port')}")
    print(f"порт открыт:         {'да' if link.get('connected') else 'нет'}")
    print(f"плата опознана:      {'да' if link.get('identified') else 'нет'}")
    print(f"телеметрия идёт:     {'да' if link.get('streaming') else 'нет'}")
    print(f"прошивка:            {board.get('hello')} {board.get('version')} / {board.get('board')}")
    print(f"протоколы:           {', '.join(board.get('protocols', []) or ['-'])}")
    print()
    print(f"состояние:           {STATE_NAMES.get(telemetry.get('state'), '?')}")
    print(f"аварии:              {telemetry.get('faults')}")
    print(f"протокол на выходе:  {telemetry.get('proto')}")
    print(f"газ (промилле):      {telemetry.get('thr')}")
    print(f"в провод:            {telemetry.get('out')}")
    print(f"аптайм платы:        {telemetry.get('t')} мс")
    print()
    print(f"телеметрия ESC:      {'есть' if esc.get('ok') else 'нет (ESC обесточен или не настроен)'}")
    if esc.get("ok"):
        print(f"  обороты:           {esc.get('rpm')} об/мин (eRPM {esc.get('erpm')})")
        print(f"  напряжение:        {esc.get('mv', 0) / 1000:.2f} В")
        print(f"  ток:               {esc.get('ma', 0) / 1000:.2f} А")
        print(f"  температура:       {esc.get('tempC')} °C")
    print(f"  пакетов / ошибок CRC: {esc.get('pkts')} / {esc.get('crcErr')}")
    # Раздельный счётчик байт: нули во всех трёх — линия молчит физически,
    # байты без пакетов — приходит мусор или не тот формат.
    raw = esc.get("bytes")
    if raw is None:
        print("  принято байт:      (нет в прошивке — нужна свежая заливка)")
    else:
        print(f"  принято байт:      {raw}")
        if raw == 0:
            print("    => по проводу телеметрии не приходит ни одного байта")
        elif not esc.get("pkts"):
            print("    => байты идут, но кадр не собирается: проверьте скорость/формат")
    print()
    board_sense = telemetry.get("board", {})
    print(f"датчики платы:       {board_sense.get('mv', 0) / 1000:.2f} В, "
          f"{board_sense.get('ma', 0) / 1000:.2f} А")

    ok = bool(link.get("identified") and link.get("streaming"))
    print()
    print("ИТОГ: плата отвечает и шлёт телеметрию" if ok else "ИТОГ: плата не отвечает как надо")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="ws://127.0.0.1:8000/ws")
    parser.add_argument("--seconds", type=float, default=6.0)
    args = parser.parse_args()
    return asyncio.run(probe(args.url, args.seconds))


if __name__ == "__main__":
    raise SystemExit(main())
