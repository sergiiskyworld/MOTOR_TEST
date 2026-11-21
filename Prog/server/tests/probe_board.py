#!/usr/bin/env python3
"""Check of a live board through a running server.

Connects to the server over the WebSocket, keeps the heartbeat going and
prints what the board reports. No throttle is applied and the arm command is
never sent.

    python server/tests/probe_board.py [--seconds 6] [--url ws://127.0.0.1:8000/ws]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys

STATE_NAMES = {0: "SAFE", 1: "THROTTLE LIVE", 2: "FAULT"}


async def probe(url: str, seconds: float) -> int:
    try:
        import websockets
    except ImportError:
        print("the websockets package is needed: pip install websockets", file=sys.stderr)
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
        print("the server did not send a single state")
        return 1

    link = last.get("link", {})
    board = link.get("board", {})
    telemetry = last.get("telemetry") or {}
    esc = telemetry.get("esc", {})

    print(f"frames from server:  {frames}")
    print(f"port:                {link.get('port')}")
    print(f"port open:           {'yes' if link.get('connected') else 'no'}")
    print(f"board identified:    {'yes' if link.get('identified') else 'no'}")
    print(f"telemetry flowing:   {'yes' if link.get('streaming') else 'no'}")
    print(f"firmware:            {board.get('hello')} {board.get('version')} / {board.get('board')}")
    print(f"protocols:           {', '.join(board.get('protocols', []) or ['-'])}")
    print()
    print(f"state:               {STATE_NAMES.get(telemetry.get('state'), '?')}")
    print(f"faults:              {telemetry.get('faults')}")
    print(f"protocol on output:  {telemetry.get('proto')}")
    print(f"throttle, per mille: {telemetry.get('thr')}")
    print(f"onto the wire:       {telemetry.get('out')}")
    print(f"board uptime:        {telemetry.get('t')} ms")
    print()
    print(f"ESC telemetry:       "
          f"{'yes' if esc.get('ok') else 'no (ESC unpowered or not configured)'}")
    if esc.get("ok"):
        print(f"  rpm:               {esc.get('rpm')} rpm (eRPM {esc.get('erpm')})")
        print(f"  voltage:           {esc.get('mv', 0) / 1000:.2f} V")
        print(f"  current:           {esc.get('ma', 0) / 1000:.2f} A")
        print(f"  temperature:       {esc.get('tempC')} °C")
    print(f"  packets / CRC errors: {esc.get('pkts')} / {esc.get('crcErr')}")
    # A separate byte counter: zeros in all three mean the line is physically
    # silent, bytes without packets mean rubbish is arriving, or the wrong
    # format is.
    raw = esc.get("bytes")
    if raw is None:
        print("  bytes received:    (not in the firmware - it needs a fresh flash)")
    else:
        print(f"  bytes received:    {raw}")
        if raw == 0:
            print("    => not a single byte arrives on the telemetry wire")
        elif not esc.get("pkts"):
            print("    => bytes are arriving but no frame assembles: check the baud rate / format")
    print()
    board_sense = telemetry.get("board", {})
    print(f"board sensors:       {board_sense.get('mv', 0) / 1000:.2f} V, "
          f"{board_sense.get('ma', 0) / 1000:.2f} A")

    ok = bool(link.get("identified") and link.get("streaming"))
    print()
    print("RESULT: the board answers and sends telemetry" if ok
          else "RESULT: the board does not answer as it should")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="ws://127.0.0.1:8000/ws")
    parser.add_argument("--seconds", type=float, default=6.0)
    args = parser.parse_args()
    return asyncio.run(probe(args.url, args.seconds))


if __name__ == "__main__":
    raise SystemExit(main())
