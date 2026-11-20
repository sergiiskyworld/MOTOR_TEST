@echo off
rem Stops the thrust stand server (Windows). See stop-stand.ps1 next to it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop-stand.ps1"
if errorlevel 1 pause
