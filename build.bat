@echo off
rem ============================================================
rem  PetPal build script
rem
rem  usage:  build.bat [options]        options are order-independent
rem
rem    (nothing)   Ninja + Debug   <- default: parallel, quiet, fast
rem    ninja       use Ninja
rem    jom         use NMake/JOM   <- the generator Qt Creator uses
rem    debug       Debug config
rem    release     Release config
rem    clean       wipe the build folder first
rem
rem  examples:
rem    build.bat                 Ninja Debug, incremental
rem    build.bat release         Ninja Release
rem    build.bat jom             JOM Debug (same folder as Qt Creator)
rem    build.bat clean release   wipe, then full Ninja Release
rem
rem  WHY this file exists:
rem    vcvars64.bat cannot run on this machine (security policy blocks
rem    reg.exe), so MSVC's INCLUDE / LIB / PATH are injected by hand.
rem
rem  WHY the file is ASCII-only:
rem    cmd.exe decodes .bat using the active OEM code page, so Chinese
rem    text in here turns into bogus commands.
rem ============================================================
setlocal

rem ---------- 1. toolchain (edit these if you upgrade) ----------
set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "QTDIR=E:\QT\6.11.2\msvc2022_64"
set "CMAKEDIR=E:\QT\Tools\CMake_64\bin"
set "JOMDIR=E:\QT\Tools\QtCreator\bin\jom"
set "NINJADIR=E:\QT\Tools\Ninja"

rem ---------- 2. parse options ----------
set "GEN=ninja"
set "CFG=Debug"
set "DOCLEAN=0"
for %%a in (%*) do (
  if /I "%%a"=="jom"     set "GEN=jom"
  if /I "%%a"=="ninja"   set "GEN=ninja"
  if /I "%%a"=="debug"   set "CFG=Debug"
  if /I "%%a"=="release" set "CFG=Release"
  if /I "%%a"=="clean"   set "DOCLEAN=1"
)

rem ---------- 3. resolve paths ----------
rem  %~dp0 is this script's folder WITH a trailing backslash.
set "SRCDIR=%~dp0"
set "SRCDIR=%SRCDIR:~0,-1%"
if /I "%GEN%"=="ninja" (
  set "GENARG=Ninja"
  set "BUILDDIR=%SRCDIR%\build\ninja-%CFG%"
) else (
  set "GENARG=NMake Makefiles JOM"
  set "BUILDDIR=%SRCDIR%\build\Desktop_Qt_6_11_2_MSVC2022_64bit_%CFG%"
)

rem ---------- 4. sanity checks ----------
if not exist "%MSVC%\bin\Hostx64\x64\cl.exe" (
  echo [ERROR] cl.exe not found:
  echo         %MSVC%\bin\Hostx64\x64\cl.exe
  echo         Fix the MSVC path at the top of this script.
  goto fail
)
if not exist "%QTDIR%\bin\qmake.exe" (
  echo [ERROR] Qt not found: %QTDIR%
  echo         Fix QTDIR at the top of this script.
  goto fail
)
if /I "%GEN%"=="ninja" if not exist "%NINJADIR%\ninja.exe" (
  echo [ERROR] ninja.exe not found: %NINJADIR%\ninja.exe
  echo         Use "build.bat jom" instead.
  goto fail
)

rem ---------- 5. the running-exe guard ----------
rem  link.exe cannot overwrite a running executable: you get
rem  "LNK1168: cannot open PetPal.exe for writing". Fail early with
rem  a clear message instead of dumping that wall of output.
tasklist /FI "IMAGENAME eq PetPal.exe" /NH 2>nul | find /I "PetPal.exe" >nul
if not errorlevel 1 (
  echo [ERROR] PetPal.exe is running - close the pet first.
  echo         link.exe cannot overwrite a running executable ^(LNK1168^).
  goto fail
)

rem ---------- 6. inject MSVC + SDK environment ----------
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%QTDIR%\bin;%JOMDIR%;%NINJADIR%;%CMAKEDIR%;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

rem ---------- 7. optional clean ----------
if "%DOCLEAN%"=="1" (
  echo [clean] removing %BUILDDIR%
  if exist "%BUILDDIR%" rmdir /S /Q "%BUILDDIR%"
)

rem ---------- 8. configure (first run only) ----------
if not exist "%BUILDDIR%\CMakeCache.txt" (
  echo [cmake] configuring %CFG% with %GENARG%
  if /I "%GEN%"=="ninja" (
    cmake.exe -S "%SRCDIR%" -B "%BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% -DCMAKE_PREFIX_PATH="%QTDIR%" -DCMAKE_MAKE_PROGRAM="%NINJADIR%\ninja.exe"
  ) else (
    cmake.exe -S "%SRCDIR%" -B "%BUILDDIR%" -G "NMake Makefiles JOM" -DCMAKE_BUILD_TYPE=%CFG% -DCMAKE_PREFIX_PATH="%QTDIR%"
  )
  if errorlevel 1 goto fail
)

rem ---------- 9. build ----------
echo [build] %GENARG% / %CFG%
cmake.exe --build "%BUILDDIR%" --target PetPal
if errorlevel 1 goto fail

echo.
echo [OK] %BUILDDIR%\PetPal.exe
echo      run it:    run.bat
echo      self-test: run.bat selftest
endlocal
exit /b 0

:fail
echo.
echo [FAILED] build did not finish. First error is above.
endlocal
exit /b 1
