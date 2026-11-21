#!/usr/bin/env bash
#
# Flashing the stand firmware into the flight controller over USB DFU.
#
# The script waits until the board shows up in DFU mode, takes a backup copy
# of the current flash contents and only then writes the new firmware.
#
#   ./scripts/flash.sh            build and flash
#   ./scripts/flash.sh --no-build use an image that is already built
#
# How to put the board into DFU:
#   - from the stand interface: the "Enter DFU" button (if the firmware is
#     already installed);
#   - hold the BOOT button on the board down and plug in USB;
#   - or short the BOOT pads at the moment USB power is applied.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIRMWARE_DIR="$ROOT/firmware"
IMAGE="$FIRMWARE_DIR/.pio/build/f405/firmware.bin"
BACKUP_DIR="$ROOT/firmware/backup"

# The STM32 ROM bootloader presents itself with this identifier.
DFU_ID="0483:df11"
FLASH_ORIGIN="0x08000000"
FLASH_SIZE="0x100000"
WAIT_SECONDS="${WAIT_SECONDS:-300}"

find_dfu_util() {
    if command -v dfu-util >/dev/null 2>&1; then
        command -v dfu-util
        return
    fi
    local bundled="$HOME/.platformio/packages/tool-dfuutil/bin/dfu-util"
    if [[ -x "$bundled" ]]; then
        echo "$bundled"
        return
    fi
    echo "dfu-util was not found. Install it: brew install dfu-util" >&2
    exit 1
}

find_pio() {
    for candidate in "$ROOT/.venv-pio/bin/pio" "$(command -v pio 2>/dev/null || true)"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done
    echo "platformio was not found. Install it: pip install platformio" >&2
    exit 1
}

DFU_UTIL="$(find_dfu_util)"

if [[ "${1:-}" != "--no-build" ]]; then
    echo "==> Building the firmware"
    "$(find_pio)" run -d "$FIRMWARE_DIR"
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "Image not found: $IMAGE" >&2
    exit 1
fi
echo "==> Image: $IMAGE ($(wc -c < "$IMAGE" | tr -d ' ') bytes)"

# The stand server holds the USB port while the browser window is open and
# fights dfu-util for it. We ask it to let go; if the server is not running,
# we quietly carry on.
if curl -fsS -m 2 -X POST "http://127.0.0.1:${STAND_PORT:-8000}/api/port/release" \
        -o /dev/null 2>/dev/null; then
    echo "==> The stand server released the port"
fi

echo
echo "==> Waiting for the board in DFU mode (up to ${WAIT_SECONDS} s)"
echo "    Hold the BOOT button on the board down and reconnect USB."
echo

deadline=$(( $(date +%s) + WAIT_SECONDS ))
until "$DFU_UTIL" -l 2>/dev/null | grep -q "$DFU_ID"; do
    if (( $(date +%s) >= deadline )); then
        echo "The board never showed up in DFU." >&2
        echo "Check that the BOOT button was held down at the moment USB power was applied." >&2
        exit 1
    fi
    sleep 1
done
echo "==> The board was found in DFU"

mkdir -p "$BACKUP_DIR"
BACKUP="$BACKUP_DIR/before-flash-$(date +%Y%m%d-%H%M%S).bin"
echo
echo "==> Backup copy of the current flash -> $(basename "$BACKUP")"
# Without this there would be nothing to restore the original firmware and its
# bootloader from.
"$DFU_UTIL" -d "$DFU_ID" -a 0 -s "${FLASH_ORIGIN}:${FLASH_SIZE}" -U "$BACKUP"

echo
echo "==> Flashing the firmware"
"$DFU_UTIL" -d "$DFU_ID" -a 0 -s "${FLASH_ORIGIN}:leave" -D "$IMAGE"

echo
echo "==> Done. The board is restarting."
echo "    Backup copy: $BACKUP"
echo "    In a couple of seconds it will appear as serial port 0483:5740."
