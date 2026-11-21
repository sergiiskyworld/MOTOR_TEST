#!/bin/bash
#
# Starting the thrust stand (macOS and Linux).
#
# This file is the startup engine. The file to click is "START-MAC.command"
# in the Prog folder. On Windows "START-WINDOWS.bat" does the same thing.
#
# What it does:
#   1. on the first run, creates a Python environment and installs the
#      dependencies;
#   2. brings the server up if it is not answering yet;
#   3. opens the page in a separate browser window;
#   4. closes the Terminal window behind itself.
#
# Everything needed is inside the Prog folder, nothing outside it is required.
#
# The server holds the USB port only while the stand window is open in the
# browser, so the AM32 and ArduPilot configurators keep working.
# Details: ../../docs/PORT_SHARING.md

set -u

PORT="${STAND_PORT:-8000}"
URL="http://127.0.0.1:${PORT}/"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROG="$(dirname "$HERE")"
SERVER="$PROG/server"
# Windows has an environment of its own (.venv-win): the project folder can be
# carried between systems, but their Python environments are not compatible.
VENV="$PROG/.venv"
PYTHON="$VENV/bin/python"
LOG_DIR="$SERVER/logs"
LOG="$LOG_DIR/server.log"

mkdir -p "$LOG_DIR"

note() { printf '%s\n' "$1"; }

fail() {
    note ""
    note "!! $1"
    note ""
    note "Details are in the log: $LOG"
    # The window must not close instantly, or the person never sees the reason.
    if [ -t 0 ]; then
        note "Press Enter to close this window."
        read -r _
    else
        osascript -e "display alert \"Thrust stand\" message \"$1\" as critical" >/dev/null 2>&1
    fi
    exit 1
}

server_answers() {
    curl -fsS -m 2 -o /dev/null "$URL" 2>/dev/null
}

[ -f "$SERVER/run.py" ] || fail "The server folder was not found — the launcher folder must stay inside Prog"

# --- 1. Python environment ---------------------------------------------------

if [ ! -x "$PYTHON" ]; then
    note "==> First run: preparing the Python environment"
    command -v python3 >/dev/null 2>&1 || fail "Python 3 was not found. Install it from python.org"
    python3 -m venv "$VENV" || fail "Could not create the Python environment"
    "$VENV/bin/pip" install --quiet --upgrade pip
    "$VENV/bin/pip" install --quiet -r "$SERVER/requirements.txt" \
        || fail "Could not install the dependencies (an internet connection is needed)"
    note "    done"
fi

# --- 2. The server -----------------------------------------------------------

if server_answers; then
    note "==> The server is already running"
else
    note "==> Starting the server"
    cd "$SERVER" || fail "The server folder was not found"
    # start_new_session detaches the server from the terminal completely:
    # closing the startup window (or Terminal itself) no longer takes it down.
    # nohup alone is not enough — the signal reaches the whole process group.
    "$PYTHON" - "$PORT" "$LOG_DIR" "$LOG" <<'PY'
import subprocess
import sys

port, log_dir, log_path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(log_path, "ab") as log:
    subprocess.Popen(
        [sys.executable, "run.py", "--host", "0.0.0.0",
         "--port", port, "--log-dir", log_dir],
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=log,
        start_new_session=True,
    )
PY

    for _ in $(seq 1 40); do
        sleep 0.25
        server_answers && break
    done
    server_answers || fail "The server did not come up within 10 seconds"
    note "    the server answers on port $PORT"
fi

# --- 3. A separate browser window ---------------------------------------------

# The stand has to open in a window of its own, filling the screen, and not as
# one tab among a pile of other people's: a tab closed by accident here means a
# stopped motor.
# Browsers on the Chromium engine have an --app mode (a window with no address
# bar and no tabs) and --kiosk (the same window over the whole screen). Safari
# has neither, so for it an ordinary new window is opened through AppleScript.
#
# A profile folder of its own is mandatory here. The --kiosk and
# --start-fullscreen flags are read only when the browser itself starts, and if
# it is already running, open merely passes the link to the running process and
# the window opens at ordinary size — this was checked, and that is what
# happened. With a separate profile a process of its own starts, and that
# process sees the flags. As a bonus, the stand does not depend on the
# extensions and tabs in the main profile.
PROFILE="$PROG/.browser-profile"

open_app_window() {
    local app
    for app in \
        "/Applications/Google Chrome.app" \
        "$HOME/Applications/Google Chrome.app" \
        "/Applications/Microsoft Edge.app" \
        "/Applications/Brave Browser.app" \
        "/Applications/Yandex.app" \
        "/Applications/Chromium.app"
    do
        if [ -d "$app" ]; then
            # -n is needed so that the arguments reach the new browser process:
            # without it open hands the link to the old one and all the flags
            # are lost.
            open -na "$app" --args \
                --user-data-dir="$PROFILE" \
                --kiosk \
                --app="$URL" \
                --no-first-run \
                --no-default-browser-check >/dev/null 2>&1 && return 0
        fi
    done

    if [ -d "/Applications/Safari.app" ]; then
        osascript >/dev/null 2>&1 <<OSA && return 0
tell application "Safari"
    make new document with properties {URL:"$URL"}
    activate
end tell
OSA
    fi

    return 1
}

note "==> Opening the stand window"
if ! open_app_window; then
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$URL" >/dev/null 2>&1
    else
        note "    no browser was found, open this address by hand: $URL"
    fi
fi

# --- 4. Clean up the Terminal window ------------------------------------------

# The startup window is not needed any more: the page is open in a window of
# its own and the server lives as a separate process. If something here does
# not work, the window simply stays open — that does not affect startup.
close_launcher_window() {
    [ "${TERM_PROGRAM:-}" = "Apple_Terminal" ] || return 0

    local tty_path window_id
    tty_path="$(tty 2>/dev/null || true)"
    [ -n "$tty_path" ] || return 0

    # Our own window has to be found now, while the script is still running: a
    # live tab has busy = true and its tty can be read. After the shell exits,
    # tty turns into missing value and its number is reused by new windows —
    # by tty alone one can close somebody else's window, and comparing against
    # it fails with an error on top of that.
    window_id="$(osascript 2>/dev/null <<OSA
tell application "Terminal"
    repeat with w in windows
        try
            if busy of selected tab of w and tty of selected tab of w is "$tty_path" then
                return id of w as text
            end if
        end try
    end repeat
end tell
return ""
OSA
)"
    case "$window_id" in
        '' | *[!0-9]*) return 0 ;;
    esac

    # The closer has to be both delayed and detached from this shell. Delayed,
    # because Terminal refuses to close a window with a live process in it
    # (silently, without an error). Detached, because an ordinary background
    # job started with `&` dies together with the window and does not live long
    # enough to close it; checked, exactly as with the server above.
    "$PYTHON" - "$window_id" <<'PY'
import subprocess
import sys

window_id = sys.argv[1]
subprocess.Popen(
    ["osascript", "-e", "delay 1",
     "-e", f'tell application "Terminal" to close window id {window_id} saving no'],
    stdin=subprocess.DEVNULL,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
    start_new_session=True,
)
PY
}

close_launcher_window
exit 0
