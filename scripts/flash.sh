#!/usr/bin/env bash
#
# Заливка прошивки стенда в полётный контроллер через USB DFU.
#
# Скрипт ждёт, пока плата появится в режиме DFU, снимает резервную копию
# текущего содержимого флеша и только потом пишет новую прошивку.
#
#   ./scripts/flash.sh            собрать и залить
#   ./scripts/flash.sh --no-build использовать уже собранный образ
#
# Как ввести плату в DFU:
#   - из интерфейса стенда: кнопка «Перейти в DFU» (если прошивка уже стоит);
#   - удерживая кнопку BOOT на плате, воткнуть USB;
#   - или замкнуть площадки BOOT в момент подачи USB.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIRMWARE_DIR="$ROOT/firmware"
IMAGE="$FIRMWARE_DIR/.pio/build/orqaf405pro/firmware.bin"
BACKUP_DIR="$ROOT/firmware/backup"

# ROM-загрузчик STM32 представляется этим идентификатором.
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
    echo "dfu-util не найден. Установите его: brew install dfu-util" >&2
    exit 1
}

find_pio() {
    for candidate in "$ROOT/.venv-pio/bin/pio" "$(command -v pio 2>/dev/null || true)"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done
    echo "platformio не найден. Установите его: pip install platformio" >&2
    exit 1
}

DFU_UTIL="$(find_dfu_util)"

if [[ "${1:-}" != "--no-build" ]]; then
    echo "==> Сборка прошивки"
    "$(find_pio)" run -d "$FIRMWARE_DIR"
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "Образ не найден: $IMAGE" >&2
    exit 1
fi
echo "==> Образ: $IMAGE ($(wc -c < "$IMAGE" | tr -d ' ') байт)"

# Сервер стенда держит USB-порт, пока в браузере открыто окно, и дерётся за
# него с dfu-util. Просим отпустить; если сервер не запущен, молча идём дальше.
if curl -fsS -m 2 -X POST "http://127.0.0.1:${STAND_PORT:-8000}/api/port/release" \
        -o /dev/null 2>/dev/null; then
    echo "==> Сервер стенда отпустил порт"
fi

echo
echo "==> Ожидание платы в режиме DFU (до ${WAIT_SECONDS} с)"
echo "    Зажмите кнопку BOOT на плате и переподключите USB."
echo

deadline=$(( $(date +%s) + WAIT_SECONDS ))
until "$DFU_UTIL" -l 2>/dev/null | grep -q "$DFU_ID"; do
    if (( $(date +%s) >= deadline )); then
        echo "Плата в DFU так и не появилась." >&2
        echo "Проверьте: кнопка BOOT удерживалась в момент подачи питания по USB." >&2
        exit 1
    fi
    sleep 1
done
echo "==> Плата найдена в DFU"

mkdir -p "$BACKUP_DIR"
BACKUP="$BACKUP_DIR/before-flash-$(date +%Y%m%d-%H%M%S).bin"
echo
echo "==> Резервная копия текущего флеша -> $(basename "$BACKUP")"
# Без этого вернуть исходную прошивку с её загрузчиком будет нечем.
"$DFU_UTIL" -d "$DFU_ID" -a 0 -s "${FLASH_ORIGIN}:${FLASH_SIZE}" -U "$BACKUP"

echo
echo "==> Заливка прошивки"
"$DFU_UTIL" -d "$DFU_ID" -a 0 -s "${FLASH_ORIGIN}:leave" -D "$IMAGE"

echo
echo "==> Готово. Плата перезапускается."
echo "    Резервная копия: $BACKUP"
echo "    Через пару секунд она появится как последовательный порт 0483:5740."
