@echo off
rem ============================================================
rem  PetPal run script
rem
rem  usage:  run.bat [selftest] [release]
rem
rem    run.bat             launch the pet (Debug build)
rem    run.bat release     launch the Release build
rem    run.bat selftest    resource self-check, write a report
rem    run.bat walktrace   dump the walk playback order, write a report
rem    run.bat falltrace   dump the gravity fall trajectory, write a report
rem
rem  WHY this file exists:
rem    PetPal.exe links Qt6 dynamically. Windows only finds those DLLs
rem    if Qt's bin folder is on PATH (or after you run windeployqt).
rem    This script puts it there for the duration of the run.
rem
rem  Keep this file ASCII-only.
rem ============================================================
setlocal
set "QTDIR=E:\QT\6.11.2\msvc2022_64"
set "PATH=%QTDIR%\bin;%PATH%"

set "SRCDIR=%~dp0"
set "SRCDIR=%SRCDIR:~0,-1%"

set "CFG=Debug"
if /I "%~1"=="release" set "CFG=Release"
if /I "%~2"=="release" set "CFG=Release"

rem  prefer the Ninja folder, fall back to the Qt Creator / JOM folder
set "EXE="
if exist "%SRCDIR%\build\ninja-%CFG%\PetPal.exe" set "EXE=%SRCDIR%\build\ninja-%CFG%\PetPal.exe"
if not defined EXE if exist "%SRCDIR%\build\Desktop_Qt_6_11_2_MSVC2022_64bit_%CFG%\PetPal.exe" set "EXE=%SRCDIR%\build\Desktop_Qt_6_11_2_MSVC2022_64bit_%CFG%\PetPal.exe"

if not defined EXE (
  echo [ERROR] PetPal.exe not found for %CFG%.
  echo         looked in:
  echo           %SRCDIR%\build\ninja-%CFG%\
  echo           %SRCDIR%\build\Desktop_Qt_6_11_2_MSVC2022_64bit_%CFG%\
  echo         run build.bat first.
  goto fail
)

for %%f in ("%EXE%") do set "EXEDIR=%%~dpf"
cd /d "%EXEDIR%"

if /I "%~1"=="selftest" (
  echo [selftest] checking every pet image in the qrc...
  "%EXE%" --selftest
  echo.
  echo [report] %EXEDIR%petpal_selftest.txt
  echo [image ] %EXEDIR%petpal_selftest_preview.png
  echo.
  echo exit code 0 = all loaded, 2 = some resource missing
  pause
  endlocal
  exit /b 0
)

if /I "%~1"=="walktrace" (
  echo [walktrace] dumping the walk playback order...
  "%EXE%" --walktrace
  echo.
  echo [report] %EXEDIR%petpal_walktrace.txt
  echo.
  echo exit code 0 = ok, 2 = some resource missing
  pause
  endlocal
  exit /b 0
)

if /I "%~1"=="falltrace" (
  echo [falltrace] dumping the gravity fall trajectory...
  "%EXE%" --falltrace
  echo.
  echo [report] %EXEDIR%petpal_falltrace.txt
  echo.
  echo exit code 0 = ok, 2 = some resource missing
  pause
  endlocal
  exit /b 0
)

echo [run] %EXE%
start "" "%EXE%"
endlocal
exit /b 0

:fail
pause
endlocal
exit /b 1
