@echo off
rem One-click launcher for the motor thrust stand (Windows).
rem
rem Double-click this file. It only calls launcher\start-stand.ps1, which does
rem the real work: cmd.exe is used for nothing except handing over to
rem PowerShell, which is where the startup logic lives.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher\start-stand.ps1"
if errorlevel 1 pause
