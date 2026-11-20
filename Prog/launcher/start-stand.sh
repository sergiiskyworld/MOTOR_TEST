#!/bin/bash
#
# Запуск стенда тяги (macOS и Linux).
#
# Этот файл — движок запуска. Кликать нужно по «START-MAC.command» в папке
# Prog. На Windows то же самое делает «START-WINDOWS.bat».
#
# Что делает:
#   1. при первом запуске создаёт окружение Python и ставит зависимости;
#   2. поднимает сервер, если он ещё не отвечает;
#   3. открывает страницу в отдельном окне браузера;
#   4. закрывает за собой окно Терминала.
#
# Всё нужное лежит внутри папки Prog, снаружи ничего не требуется.
#
# USB-порт сервер занимает только пока в браузере открыто окно стенда,
# поэтому конфигураторы AM32/ArduPilot остаются работоспособными.
# Подробности: ../../docs/PORT_SHARING.md

set -u

PORT="${STAND_PORT:-8000}"
URL="http://127.0.0.1:${PORT}/"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROG="$(dirname "$HERE")"
SERVER="$PROG/server"
# У Windows своё окружение (.venv-win): папку проекта можно носить между
# системами, а окружения Python у них несовместимы.
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
    note "Подробности в логе: $LOG"
    # Окно не должно закрыться мгновенно, иначе человек не увидит причину.
    if [ -t 0 ]; then
        note "Нажмите Enter, чтобы закрыть."
        read -r _
    else
        osascript -e "display alert \"Стенд тяги\" message \"$1\" as critical" >/dev/null 2>&1
    fi
    exit 1
}

server_answers() {
    curl -fsS -m 2 -o /dev/null "$URL" 2>/dev/null
}

[ -f "$SERVER/run.py" ] || fail "Не найдена папка server — папку launcher нельзя выносить из Prog"

# --- 1. Окружение Python ------------------------------------------------------

if [ ! -x "$PYTHON" ]; then
    note "==> Первый запуск: готовлю окружение Python"
    command -v python3 >/dev/null 2>&1 || fail "Python 3 не найден. Установите его с python.org"
    python3 -m venv "$VENV" || fail "Не удалось создать окружение Python"
    "$VENV/bin/pip" install --quiet --upgrade pip
    "$VENV/bin/pip" install --quiet -r "$SERVER/requirements.txt" \
        || fail "Не удалось установить зависимости (нужен интернет)"
    note "    готово"
fi

# --- 2. Сервер ----------------------------------------------------------------

if server_answers; then
    note "==> Сервер уже работает"
else
    note "==> Запускаю сервер"
    cd "$SERVER" || fail "Не найдена папка server"
    # start_new_session отвязывает сервер от терминала целиком: закрытие окна
    # запуска (или самого Терминала) его больше не уносит. Одного nohup для
    # этого мало — сигнал приходит всей группе процессов.
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
    server_answers || fail "Сервер не поднялся за 10 секунд"
    note "    сервер отвечает на порту $PORT"
fi

# --- 3. Отдельное окно браузера ----------------------------------------------

# Стенд должен открываться своим окном на весь экран, а не вкладкой в куче
# чужих: случайно закрытая вкладка здесь означает остановленный мотор.
# Браузеры на движке Chromium умеют режим --app (окно без адресной строки и без
# вкладок) и --kiosk (тот же экран целиком). Safari ни того, ни другого не
# имеет, поэтому для него открывается обычное новое окно через AppleScript.
#
# Своя папка профиля здесь обязательна. Флаги --kiosk и --start-fullscreen
# читаются только при старте самого браузера, а если он уже запущен, open
# просто передаёт ссылку работающему процессу, и окно открывается обычного
# размера — проверено, так и было. С отдельным профилем стартует свой процесс,
# который эти флаги видит. Заодно стенд не зависит от расширений и вкладок в
# основном профиле.
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
            # -n нужен, чтобы аргументы дошли до нового процесса браузера:
            # без него open отдаёт ссылку старому, и все флаги теряются.
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

note "==> Открываю окно стенда"
if ! open_app_window; then
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$URL" >/dev/null 2>&1
    else
        note "    браузер не найден, откройте вручную: $URL"
    fi
fi

# --- 4. Убрать за собой окно Терминала ---------------------------------------

# Окно запуска больше не нужно: страница открыта в своём окне, сервер живёт
# отдельным процессом. Если что-то не сработает, окно просто останется — на
# запуск это не влияет.
close_launcher_window() {
    [ "${TERM_PROGRAM:-}" = "Apple_Terminal" ] || return 0

    local tty_path window_id
    tty_path="$(tty 2>/dev/null || true)"
    [ -n "$tty_path" ] || return 0

    # Своё окно надо найти сейчас, пока скрипт ещё работает: у живой вкладки
    # busy = true и читается tty. После выхода из оболочки tty превращается в
    # missing value, а его номер переиспользуют новые окна — по одному tty
    # можно закрыть чужое окно, и сравнение с ним к тому же падает с ошибкой.
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

    # Закрыватель нужно и отложить, и отвязать от этой оболочки. Отложить —
    # потому что окно с живым процессом Терминал закрывать отказывается (молча,
    # без ошибки). Отвязать — потому что обычная фоновая задача через `&`
    # умирает вместе с окном и до закрытия не доживает; проверено, ровно как с
    # самим сервером выше.
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
