@echo off
rem ============================================================
rem  VolumeTray - reproducible build script
rem  Run from anywhere: paths resolve from this file's folder.
rem  Requires w64devkit (or MinGW-w64) on PATH.
rem ============================================================
setlocal
cd /d "%~dp0"
set "OUT=build"
if not exist "%OUT%" mkdir "%OUT%"

echo [1/2] windres: app.rc -- app.res
windres -i app.rc -O coff -o app.res
if errorlevel 1 goto :fail

echo [2/2] g++: main.cpp + app.res -- build\VolumeTray.exe
rem -municode + -DUNICODE/-D_UNICODE are required by MinGW:
rem  - without -municode the entry point is searched as WinMain (wWinMain fails),
rem  - without UNICODE macros MAKEINTRESOURCE() resolves to the ANSI variant
rem    (MAKEINTRESOURCEA) and LoadIconW(IDI_APPICON) does not compile.
g++ -O2 -s -mwindows -municode -DUNICODE -D_UNICODE -Wall main.cpp app.res -o "%OUT%\VolumeTray.exe" -lshell32 -luser32 -lgdi32 -ladvapi32
if errorlevel 1 goto :fail

echo OK: "%OUT%\VolumeTray.exe"
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
