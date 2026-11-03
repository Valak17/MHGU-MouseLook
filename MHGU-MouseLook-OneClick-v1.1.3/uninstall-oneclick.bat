@echo off
setlocal EnableExtensions
set "GAME=%~1"
if not defined GAME (
  echo Enter the MHGU game folder:
  set /p "GAME=> "
)
if not defined GAME exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -GamePath "%GAME%"
pause
