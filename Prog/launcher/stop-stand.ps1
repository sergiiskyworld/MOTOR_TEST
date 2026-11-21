# Stopping the stand server on Windows.
#
# The file to click is "STOP-WINDOWS.bat" in this same folder.
#
# Usually this is not needed: the USB port is held only while the browser
# window is open. It helps when the server has to be taken down completely.

$ErrorActionPreference = 'Continue'

$Port = if ($env:STAND_PORT) { $env:STAND_PORT } else { '8000' }
$Prog = Split-Path -Parent $PSScriptRoot
$RunPy = Join-Path $Prog 'server\run.py'

Write-Host '==> Stopping the stand server'

# We look at the process command line: the server has no name of its own, it
# is python.
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
    # -ErrorAction Stop: otherwise a refused connection would never reach catch.
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2 `
        -UseBasicParsing -ErrorAction Stop | Out-Null
    $alive = $true
} catch { }

if ($alive) {
    Write-Host "!! The server still answers on port $Port" -ForegroundColor Red
    Write-Host '   Who is holding the port:'
    netstat -ano | Select-String ":$Port\s.*LISTENING"
} elseif ($killed -gt 0) {
    Write-Host "    server stopped (processes: $killed)"
} else {
    Write-Host '    the server was not running anyway'
}

Write-Host ''
Write-Host 'The COM port of the board is free. You can check this in Device Manager.'
Write-Host "Server file: $RunPy"
Write-Host ''
Write-Host 'Press Enter to close this window.'
[void][System.Console]::ReadLine()
