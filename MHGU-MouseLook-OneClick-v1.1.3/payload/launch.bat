@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mods\0100770008DD8000\MouseLook\disable_touchscreen.ps1" -GamePath "%~dp0." >nul 2>nul
start "" "%~dp0MONSTER HUNTER GENERATIONS ULTIMATE.exe"
endlocal
