#!/bin/bash
# Stops the stand server and frees the USB port.
#
# The file to click is "STOP-MAC.command" in this same folder.
#
# Usually this is not needed: the port is held only while the browser window
# is open. It helps when the server has to be taken down completely — before
# a long session in the ArduPilot configurator, for example.

set -u

PORT="${STAND_PORT:-8000}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROG="$(dirname "$HERE")"

echo "==> Stopping the stand server"

# We look for the listening TCP port rather than for the path to run.py: the
# server is started from its own folder, so the path does not appear in its
# command line at all, and searching by path found nothing.
killed=0
for pid in $(lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t 2>/dev/null); do
    # Something else could have taken the port, so we kill only our server.
    if ps -p "$pid" -o command= 2>/dev/null | grep -q "run\.py"; then
        kill "$pid" 2>/dev/null && killed=$((killed + 1))
    else
        echo "    port $PORT is taken by another process ($pid), leaving it alone"
    fi
done

sleep 1
if curl -fsS -m 2 -o /dev/null "http://127.0.0.1:${PORT}/" 2>/dev/null; then
    echo "!! The server still answers on port $PORT"
    echo "   Who is holding the port:"
    lsof -nP -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null || true
elif [ "$killed" -gt 0 ]; then
    echo "    server stopped (processes: $killed)"
else
    echo "    the server was not running anyway"
fi

echo
echo "The USB port is free. To check that nobody is holding it:"
echo "    lsof /dev/cu.*"
echo
echo "Server file: $PROG/server/run.py"
echo
echo "Press Enter to close this window."
read -r _
