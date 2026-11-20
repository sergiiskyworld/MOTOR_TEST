# Остановка сервера стенда на Windows.
#
# Кликать нужно по «STOP-WINDOWS.bat» в этой же папке.
#
# Обычно это не нужно: USB-порт занят только пока открыто окно браузера.
# Пригодится, если сервер надо убрать совсем.

$ErrorActionPreference = 'Continue'

$Port = if ($env:STAND_PORT) { $env:STAND_PORT } else { '8000' }
$Prog = Split-Path -Parent $PSScriptRoot
$RunPy = Join-Path $Prog 'server\run.py'

Write-Host '==> Останавливаю сервер стенда'

# Ищем по командной строке процесса: у сервера нет своего имени, это python.
$killed = 0
$processes = Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" -ErrorAction SilentlyContinue
foreach ($process in $processes) {
    if ($process.CommandLine -and $process.CommandLine -like "*run.py*" -and
        $process.CommandLine -like "*$Port*") {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
        $killed++
    }
}

Start-Sleep -Seconds 1

$alive = $false
try {
    # -ErrorAction Stop: иначе отказ соединения не дошёл бы до catch.
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2 `
        -UseBasicParsing -ErrorAction Stop | Out-Null
    $alive = $true
} catch { }

if ($alive) {
    Write-Host "!! Сервер всё ещё отвечает на порту $Port" -ForegroundColor Red
    Write-Host '   Кто занял порт:'
    netstat -ano | Select-String ":$Port\s.*LISTENING"
} elseif ($killed -gt 0) {
    Write-Host "    сервер остановлен (процессов: $killed)"
} else {
    Write-Host '    сервер и так не работал'
}

Write-Host ''
Write-Host 'COM-порт платы свободен. Проверить можно в Диспетчере устройств.'
Write-Host "Файл сервера: $RunPy"
Write-Host ''
Write-Host 'Нажмите Enter, чтобы закрыть.'
[void][System.Console]::ReadLine()
