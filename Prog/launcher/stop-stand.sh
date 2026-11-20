#!/bin/bash
# Останавливает сервер стенда и освобождает USB-порт.
#
# Кликать нужно по «STOP-MAC.command» в этой же папке.
#
# Обычно это не нужно: порт занят только пока открыто окно браузера.
# Пригодится, если сервер надо убрать совсем — например перед долгой работой
# в конфигураторе ArduPilot.

set -u

PORT="${STAND_PORT:-8000}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROG="$(dirname "$HERE")"

echo "==> Останавливаю сервер стенда"

# Ищем по слушающему TCP-порту, а не по пути к run.py: сервер запускается из
# своей папки, поэтому в его командной строке путь не встречается вообще, и
# поиск по пути не находил ничего.
killed=0
for pid in $(lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t 2>/dev/null); do
    # Порт мог занять и что-то чужое, поэтому убиваем только свой сервер.
    if ps -p "$pid" -o command= 2>/dev/null | grep -q "run\.py"; then
        kill "$pid" 2>/dev/null && killed=$((killed + 1))
    else
        echo "    порт $PORT занят чужим процессом ($pid), не трогаю"
    fi
done

sleep 1
if curl -fsS -m 2 -o /dev/null "http://127.0.0.1:${PORT}/" 2>/dev/null; then
    echo "!! Сервер всё ещё отвечает на порту $PORT"
    echo "   Кто занял порт:"
    lsof -nP -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null || true
elif [ "$killed" -gt 0 ]; then
    echo "    сервер остановлен (процессов: $killed)"
else
    echo "    сервер и так не работал"
fi

echo
echo "Порт USB свободен. Проверить, что его никто не держит:"
echo "    lsof /dev/cu.*"
echo
echo "Файл сервера: $PROG/server/run.py"
echo
echo "Нажмите Enter, чтобы закрыть."
read -r _
