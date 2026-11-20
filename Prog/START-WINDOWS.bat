@echo off
rem One-click launcher for the motor thrust stand (Windows).
rem
rem Double-click this file. It only calls launcher\start-stand.ps1, which does
rem the real work: all user-facing text is Russian, and cmd.exe mangles
rem non-ASCII characters in batch files depending on the console code page.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher\start-stand.ps1"
if errorlevel 1 pause
