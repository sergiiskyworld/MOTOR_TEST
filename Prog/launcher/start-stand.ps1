# Запуск стенда тяги на Windows.
#
# Кликать нужно не по этому файлу, а по «START-WINDOWS.bat» в папке Prog.
# Тот батник только вызывает этот скрипт: сам cmd плохо переносит русский
# текст в теле файла, а PowerShell с ним работает нормально.
#
# Что делает:
#   1. при первом запуске создаёт окружение Python и ставит зависимости;
#   2. поднимает сервер, если он ещё не отвечает;
#   3. открывает страницу в отдельном окне браузера.
#
# Всё нужное лежит внутри папки Prog, снаружи ничего не требуется.
#
# USB-порт сервер занимает только пока в браузере открыто окно стенда,
# поэтому конфигуратор AM32 остаётся работоспособным.
# Подробности: ..\docs\PORT_SHARING.md

# Намеренно не 'Stop': в PowerShell 7 при 'Stop' любая строка, которую внешняя
# программа напишет в stderr, превращается в исключение, а pip пишет туда
# обычные предупреждения. Ошибки проверяются явно — по $LASTEXITCODE и
# Test-Path, а там где нужно исключение, стоит -ErrorAction Stop.
$ErrorActionPreference = 'Continue'

$Port    = if ($env:STAND_PORT) { $env:STAND_PORT } else { '8000' }
$Url     = "http://127.0.0.1:$Port/"
$Prog    = Split-Path -Parent $PSScriptRoot
$Server  = Join-Path $Prog 'server'
# Своё окружение, отдельно от .venv для macOS: папку проекта можно носить
# между системами, а окружения Python у них несовместимы.
$Venv    = Join-Path $Prog '.venv-win'
$LogDir  = Join-Path $Server 'logs'
# uvicorn пишет журнал в stderr, поэтому именно он попадает в server.log —
# файл с тем же именем, что и на macOS.
$LogFile = Join-Path $LogDir 'server.log'
$OutFile = Join-Path $LogDir 'server.out.log'

function Note($text) { Write-Host $text }

function Fail($text) {
    Write-Host ''
    Write-Host "!! $text" -ForegroundColor Red
    Write-Host ''
    Write-Host "Подробности в логе: $LogFile"
    Write-Host ''
    Write-Host 'Нажмите Enter, чтобы закрыть.'
    [void][System.Console]::ReadLine()
    exit 1
}

function ServerAnswers {
    try {
        # -ErrorAction Stop обязателен: без него отказ соединения был бы
        # нетерминирующей ошибкой, catch не сработал бы и функция вернула
        # бы $true для лежащего сервера.
        Invoke-WebRequest -Uri $Url -TimeoutSec 2 -UseBasicParsing -ErrorAction Stop | Out-Null
        return $true
    } catch {
        return $false
    }
}

if (-not (Test-Path (Join-Path $Server 'run.py'))) {
    Fail 'Не найдена папка server — папку launcher нельзя выносить из Prog'
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

# --- 1. Окружение Python ------------------------------------------------------

$VenvPython  = Join-Path $Venv 'Scripts\python.exe'
# pythonw не держит окно консоли, поэтому сервер живёт незаметно.
$VenvPythonW = Join-Path $Venv 'Scripts\pythonw.exe'

if (-not (Test-Path $VenvPython)) {
    Note '==> Первый запуск: готовлю окружение Python'

    # Установщик с python.org кладёт лончер py.exe, из Microsoft Store
    # приходит только python.exe. Ветки развёрнуты явно, без сборки списка
    # аргументов: передача массива внешней программе ведёт себя по-разному
    # в разных версиях PowerShell.
    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3 -m venv $Venv
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        & python -m venv $Venv
    } else {
        Fail 'Python 3 не найден. Установите его с python.org и при установке поставьте галку "Add Python to PATH"'
    }
    if (-not (Test-Path $VenvPython)) { Fail 'Не удалось создать окружение Python' }

    & $VenvPython -m pip install --quiet --upgrade pip
    & $VenvPython -m pip install --quiet -r (Join-Path $Server 'requirements.txt')
    if ($LASTEXITCODE -ne 0) { Fail 'Не удалось установить зависимости (нужен интернет)' }
    Note '    готово'
}

# --- 2. Сервер ----------------------------------------------------------------

if (ServerAnswers) {
    Note '==> Сервер уже работает'
} else {
    Note '==> Запускаю сервер'
    $runner = if (Test-Path $VenvPythonW) { $VenvPythonW } else { $VenvPython }
    # Процесс не привязан к этому скрипту: закрытие окна запуска сервер не
    # уносит. Start-Process перезаписывает файлы журнала, дописывать он не
    # умеет, поэтому история остаётся только за текущий запуск.
    Start-Process -FilePath $runner `
        -ArgumentList @('run.py', '--host', '0.0.0.0', '--port', $Port, '--log-dir', $LogDir) `
        -WorkingDirectory $Server `
        -RedirectStandardOutput $OutFile `
        -RedirectStandardError $LogFile `
        -WindowStyle Hidden | Out-Null

    $ok = $false
    foreach ($i in 1..40) {
        Start-Sleep -Milliseconds 250
        if (ServerAnswers) { $ok = $true; break }
    }
    if (-not $ok) {
        Fail 'Сервер не поднялся за 10 секунд. Если Windows спросила про брандмауэр — разрешите доступ'
    }
    Note "    сервер отвечает на порту $Port"
}

# --- 3. Отдельное окно браузера ----------------------------------------------

# Стенд должен открываться своим окном на весь экран, а не вкладкой в куче
# чужих: случайно закрытая вкладка здесь означает остановленный мотор. Режим
# --app даёт окно без адресной строки и без вкладок, --kiosk растягивает его на
# весь экран. Edge есть в любой Windows 10/11, поэтому обычно срабатывает
# первый же найденный браузер; если не нашёлся ни один, открываем браузером по
# умолчанию вкладкой.
#
# Своя папка профиля обязательна: --kiosk читается только при старте самого
# браузера, а уже запущенный процесс просто откроет ссылку окном обычного
# размера. С отдельным профилем запускается свой процесс, который этот флаг
# видит, и стенд заодно не зависит от расширений в основном профиле.
Note '==> Открываю окно стенда'

# Не $Profile: так называется встроенная переменная PowerShell с путём к
# скрипту профиля оболочки, и перезаписывать её не стоит.
$BrowserProfile = Join-Path $Prog '.browser-profile'

$browsers = @(
    (Join-Path $env:ProgramFiles 'Google\Chrome\Application\chrome.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path $env:ProgramFiles 'BraveSoftware\Brave-Browser\Application\brave.exe')
)

$browser = $browsers | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($browser) {
    Start-Process -FilePath $browser -ArgumentList @(
        "--user-data-dir=$BrowserProfile",
        '--kiosk',
        "--app=$Url",
        '--no-first-run',
        '--no-default-browser-check'
    )
} else {
    Note '    браузер на движке Chromium не найден, открываю вкладкой'
    Start-Process $Url
}
exit 0
