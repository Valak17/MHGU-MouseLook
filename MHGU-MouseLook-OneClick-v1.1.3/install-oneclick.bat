@echo off
setlocal EnableExtensions
set "GAME=%~1"

if not defined GAME goto prompt
goto run

:prompt
echo Enter the clean/existing MHGU game folder that contains the game EXE:
set /p "GAME=> "
if not defined GAME exit /b 1

:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -GamePath "%GAME%"
if errorlevel 1 (
  echo.
  echo Installation failed. Read the message above.
  pause
  exit /b 1
)
echo.
pause
exit /b 0
