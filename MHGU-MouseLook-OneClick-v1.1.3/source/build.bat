@echo off
setlocal
set "HERE=%~dp0"
set "OUT=%~dp0..\payload\MouseLook\plugins\mouselook.dll"
set "CC="

if defined CXX set "CC=%CXX%"
if not defined CC where clang++ >nul 2>nul && set "CC=clang++"
if not defined CC where g++ >nul 2>nul && set "CC=g++"
if not defined CC (
  echo [MouseLook] No clang++ or g++ found in PATH.
  exit /b 1
)

"%CC%" -O2 -std=c++20 -shared -static -fvisibility=hidden ^
  -I"%HERE%" -o "%OUT%" "%HERE%mouselook.cpp" ^
  -luser32 -lole32 -ladvapi32 -static-libstdc++ -static-libgcc
if errorlevel 1 exit /b 1
echo Built: %OUT%
endlocal
