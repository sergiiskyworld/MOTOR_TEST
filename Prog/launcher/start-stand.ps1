# Starting the thrust stand on Windows.
#
# The file to click is not this one but "START-WINDOWS.bat" in the Prog
# folder. That batch file does nothing except call this script: cmd itself
# copes badly with anything beyond a single command, while PowerShell handles
# the startup logic properly.
#
# What it does:
#   1. on the first run, creates a Python environment and installs the
#      dependencies;
#   2. brings the server up if it is not answering yet;
#   3. opens the page in a separate browser window.
#
# Everything needed is inside the Prog folder, nothing outside it is required.
#
# The server holds the USB port only while the stand window is open in the
# browser, so the AM32 configurator keeps working.
# Details: ..\docs\PORT_SHARING.md

# Deliberately not 'Stop': in PowerShell 7, with 'Stop' any line an external
# program writes to stderr turns into an exception, and pip writes ordinary
# warnings there. Errors are checked explicitly, through $LASTEXITCODE and
# Test-Path, and where an exception is wanted, -ErrorAction Stop is used.
$ErrorActionPreference = 'Continue'

$Port    = if ($env:STAND_PORT) { $env:STAND_PORT } else { '8000' }
$Url     = "http://127.0.0.1:$Port/"
$Prog    = Split-Path -Parent $PSScriptRoot
$Server  = Join-Path $Prog 'server'
# An environment of its own, separate from the .venv for macOS: the project
# folder can be carried between systems, but their Python environments are not
# compatible.
$Venv    = Join-Path $Prog '.venv-win'
$LogDir  = Join-Path $Server 'logs'
# uvicorn writes its journal to stderr, which is why that stream is the one
# that ends up in server.log — a file with the same name as on macOS.
$LogFile = Join-Path $LogDir 'server.log'
$OutFile = Join-Path $LogDir 'server.out.log'

function Note($text) { Write-Host $text }

function Fail($text) {
    Write-Host ''
    Write-Host "!! $text" -ForegroundColor Red
    Write-Host ''
    Write-Host "Details are in the log: $LogFile"
    Write-Host ''
    Write-Host 'Press Enter to close this window.'
    [void][System.Console]::ReadLine()
    exit 1
}

function ServerAnswers {
    try {
        # -ErrorAction Stop is mandatory: without it a refused connection would
        # be a non-terminating error, catch would not fire and the function
        # would return $true for a server that is down.
        Invoke-WebRequest -Uri $Url -TimeoutSec 2 -UseBasicParsing -ErrorAction Stop | Out-Null
        return $true
    } catch {
        return $false
    }
}

if (-not (Test-Path (Join-Path $Server 'run.py'))) {
    Fail 'The server folder was not found - the launcher folder must stay inside Prog'
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

# --- 1. Python environment ---------------------------------------------------

$VenvPython  = Join-Path $Venv 'Scripts\python.exe'
# pythonw keeps no console window, so the server lives out of sight.
$VenvPythonW = Join-Path $Venv 'Scripts\pythonw.exe'

if (-not (Test-Path $VenvPython)) {
    Note '==> First run: preparing the Python environment'

    # The installer from python.org drops the py.exe launcher, while the one
    # from the Microsoft Store brings only python.exe. The branches are spelled
    # out explicitly, without building a list of arguments: passing an array to
    # an external program behaves differently in different PowerShell versions.
    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3 -m venv $Venv
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        & python -m venv $Venv
    } else {
        Fail 'Python 3 was not found. Install it from python.org and tick "Add Python to PATH" during the installation'
    }
    if (-not (Test-Path $VenvPython)) { Fail 'Could not create the Python environment' }

    & $VenvPython -m pip install --quiet --upgrade pip
    & $VenvPython -m pip install --quiet -r (Join-Path $Server 'requirements.txt')
    if ($LASTEXITCODE -ne 0) { Fail 'Could not install the dependencies (an internet connection is needed)' }
    Note '    done'
}

# --- 2. The server -----------------------------------------------------------

if (ServerAnswers) {
    Note '==> The server is already running'
} else {
    Note '==> Starting the server'
    $runner = if (Test-Path $VenvPythonW) { $VenvPythonW } else { $VenvPython }
    # The process is not tied to this script: closing the startup window does
    # not take the server down. Start-Process overwrites the journal files, it
    # cannot append, so the history covers only the current run.
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
        Fail 'The server did not come up within 10 seconds. If Windows asked about the firewall, allow access'
    }
    Note "    the server answers on port $Port"
}

# --- 3. A separate browser window ---------------------------------------------

# The stand has to open in a window of its own, filling the screen, and not as
# one tab among a pile of other people's: a tab closed by accident here means a
# stopped motor. The --app mode gives a window with no address bar and no tabs,
# --kiosk stretches it over the whole screen. Edge is present in any Windows
# 10/11, so usually the first browser found is the one that works; if none was
# found, we open a tab in the default browser.
#
# A profile folder of its own is mandatory: --kiosk is read only when the
# browser itself starts, while an already running process will merely open the
# link in a window of ordinary size. With a separate profile a process of its
# own starts, and that process sees the flag; as a bonus, the stand does not
# depend on the extensions in the main profile.
Note '==> Opening the stand window'

# Not $Profile: that is the name of the built-in PowerShell variable holding
# the path to the shell profile script, and overwriting it is a bad idea.
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
    Note '    no browser on the Chromium engine was found, opening a tab instead'
    Start-Process $Url
}
exit 0
