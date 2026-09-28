@echo off
rem ============================================================
rem  PetPal packaging script  (Release -> dist\PetPal)
rem
rem  usage:  pack.bat [options]        options are order-independent
rem
rem    (nothing)   Release + deploy + prune + zip   <- default
rem    gl          also ship opengl32sw.dll / D3Dcompiler_47.dll
rem                (safety net for old PCs, VMs, remote desktop --
rem                 the app was measured to run fine without them)
rem    nozip       skip the final .zip
rem
rem  output:
rem    dist\PetPal\            the folder to send to other people
rem    dist\PetPal.zip         same thing, zipped
rem    dist\opt-dll\           optional safety DLLs (see "gl" above)
rem
rem  WHY a separate script instead of reusing build.bat:
rem    build.bat produces an exe that only runs on a machine with Qt
rem    installed. Sending that exe alone gets the recipient
rem    "Qt6Core.dll was not found". windeployqt puts the Qt DLLs and
rem    the required plugins next to the exe; this script then removes
rem    the parts windeployqt over-ships (see the prune section).
rem
rem  WHY the file is ASCII-only:
rem    cmd.exe decodes .bat using the active OEM code page, so Chinese
rem    text in here turns into bogus commands. Same as build.bat.
rem ============================================================
setlocal

rem ---------- 1. toolchain (keep in sync with build.bat) ----------
set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "QTDIR=E:\QT\6.11.2\msvc2022_64"
set "CMAKEDIR=E:\QT\Tools\CMake_64\bin"
set "NINJADIR=E:\QT\Tools\Ninja"

rem ---------- 2. parse options ----------
set "WITHGL=0"
set "DOZIP=1"
for %%a in (%*) do (
  if /I "%%a"=="gl"    set "WITHGL=1"
  if /I "%%a"=="nozip" set "DOZIP=0"
)

set "SRCDIR=%~dp0"
set "SRCDIR=%SRCDIR:~0,-1%"
set "BUILDDIR=%SRCDIR%\build\ninja-Release"
set "DIST=%SRCDIR%\dist\PetPal"
set "OPTDLL=%SRCDIR%\dist\opt-dll"

rem ---------- 3. sanity ----------
if not exist "%QTDIR%\bin\windeployqt.exe" (
  echo [ERROR] windeployqt.exe not found: %QTDIR%\bin
  goto fail
)
tasklist /FI "IMAGENAME eq PetPal.exe" /NH 2>nul | find /I "PetPal.exe" >nul
if not errorlevel 1 (
  echo [ERROR] PetPal.exe is running - close the pet first.
  goto fail
)

rem ---------- 4. inject MSVC + SDK environment ----------
rem  vcvars64.bat cannot run on this machine (reg.exe is blocked), so
rem  INCLUDE / LIB / PATH are set by hand. Same as build.bat.
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%QTDIR%\bin;%NINJADIR%;%CMAKEDIR%;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

rem ---------- 5. configure + build Release ----------
if not exist "%BUILDDIR%\CMakeCache.txt" (
  echo [cmake] configuring Release
  cmake.exe -S "%SRCDIR%" -B "%BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%" -DCMAKE_MAKE_PROGRAM="%NINJADIR%\ninja.exe"
  if errorlevel 1 goto fail
)
echo [build] Ninja / Release
cmake.exe --build "%BUILDDIR%" --target PetPal
if errorlevel 1 goto fail

rem ---------- 6. fresh dist folder ----------
echo [deploy] %DIST%
if exist "%DIST%" rmdir /S /Q "%DIST%"
mkdir "%DIST%"

rem  windeployqt must run from a clean folder: it refuses to overwrite
rem  files it did not put there itself.
rem  --no-translations is on purpose: the full translations folder is
rem  5.6 MB of 40+ languages, and this app loads none of it - see 9b.
windeployqt.exe --release --no-opengl-sw --no-system-d3d-compiler --no-translations --dir "%DIST%" "%BUILDDIR%\PetPal.exe"
if errorlevel 1 goto fail

rem  ★ windeployqt only lays down the DEPENDENCIES - it does not copy the
rem    exe itself. Without the next line the folder is a pile of DLLs with
rem    no program in it. Easy to miss because windeployqt exits 0.
copy /Y "%BUILDDIR%\PetPal.exe" "%DIST%\" >nul
if errorlevel 1 goto fail

rem ---------- 7. MSVC runtime ----------
rem  windeployqt skips these unless VCINSTALLDIR is set (it cannot be,
rem  see above), so copy the ones PetPal.exe actually imports:
rem    MSVCP140 / MSVCP140_1 / MSVCP140_2 / VCRUNTIME140 / VCRUNTIME140_1
rem  (verify with:  dumpbin /dependents PetPal.exe)
rem  Only 5 of the ~10 files in the CRT folder are imported - concrt140,
rem  vccorlib140, msvcp140_atomic_wait etc. are dead weight here.
set "CRTDIR="
for /d %%d in ("%MSVC%\..\..\Redist\MSVC\*") do set "CRTDIR=%%~d\x64\Microsoft.VC143.CRT"
if not defined CRTDIR goto fail
for %%n in (msvcp140 msvcp140_1 msvcp140_2 vcruntime140 vcruntime140_1) do (
  copy /Y "%CRTDIR%\%%n.dll" "%DIST%\" >nul
  if errorlevel 1 goto fail
)

rem ---------- 8. content that lives on disk, not in the exe ----------
rem  Songs/lyrics are plain .txt read at runtime (SongLibrary walks up
rem  from the exe dir). Pet images are NOT here - they are compiled
rem  into the exe through resources.qrc.
if exist "%SRCDIR%\resources\songs" (
  mkdir "%DIST%\resources"
  xcopy /E /I /Y /Q "%SRCDIR%\resources\songs" "%DIST%\resources\songs" >nul
)

rem  ★ ship an EMPTY resources\daily_image ★
rem  The folder is looked up at runtime; when it is absent the page says
rem  "no daily_image folder found / it should be under resources/" - that
rem  string is written for the DEVELOPER and reads like a crash to anyone
rem  else. When the folder exists but holds no image, the page instead
rem  says "put jpg/png here: <full path>, restart to see them", which is
rem  the onboarding text this feature was designed around.
rem  So: always create the folder, and leave it empty.
rem  The author's own 20 files / 67 MB gallery is NOT copied - jpg does
rem  not compress, that alone would take the zip from 15 MB to 81 MB.
rem  Uncomment the next line if you DO want the recipient to see YOUR
rem  pictures:
rem    xcopy /E /I /Y /Q "%SRCDIR%\resources\daily_image" "%DIST%\resources\daily_image" >nul
mkdir "%DIST%\resources\daily_image" 2>nul

rem ---------- 9. prune what windeployqt over-ships ----------
rem  Every line below was verified by deleting the item and re-running
rem  the packaged exe with a PATH that has no Qt on it:
rem    PetPal.exe --selftest   ->  exit 0, 31/31 images, 0 failures
rem  Qt6Svg / qsvgicon / qsvg : grep says nothing in src\ or
rem      resources.qrc uses SVG, so the whole SVG stack is unreachable.
rem  qtuiotouchplugin         : Tuio touch input only; not used.
rem  qico                     : *.ico is not in DailyImagePage's filter and
rem      the app icon is baked into the exe by resources\app.rc.
rem  ★ qgif is KEPT on purpose ★ DailyImagePage's filter accepts *.gif,
rem      so a gif dropped into resources\daily_image must still decode.
rem      (It was deleted here once and the mistake was invisible to
rem      --selftest, because the selftest gallery has no gif in it.)
rem  translations             : none shipped at all - see 9b.
if "%WITHGL%"=="0" (
  del /Q "%DIST%\opengl32sw.dll"       2>nul
  del /Q "%DIST%\D3Dcompiler_47.dll"   2>nul
)
del /Q "%DIST%\Qt6Svg.dll"             2>nul
del /Q "%DIST%\imageformats\qsvg.dll"  2>nul
del /Q "%DIST%\imageformats\qico.dll"  2>nul
if exist "%DIST%\iconengines" rmdir /S /Q "%DIST%\iconengines"
if exist "%DIST%\generic"     rmdir /S /Q "%DIST%\generic"
rem  --no-opengl-sw / --no-system-d3d-compiler / --no-translations above
rem  already keep these out; the deletes are belt-and-braces in case those
rem  flags ever change meaning.

rem ---------- 9b. Qt's own translations : NOT shipped ----------
rem  There is no QTranslator anywhere in src\, so the app never loads
rem  translations\qt_*.qm and shipping them would change nothing.
rem  Consequence the recipient WILL notice: Qt's own dialog strings
rem  (the file dialog when picking a music folder, message box buttons)
rem  read English, because that is Qt's built-in default.
rem  If a translator is ever installed in main.cpp, the two files to
rem  ship are qt_zh_CN.qm AND qtbase_zh_CN.qm - the former is a 99 byte
rem  meta catalog that only lists the latter (Qt 6 split them up), so
rem  shipping qt_zh_CN.qm alone does nothing.

rem ---------- 10. optional safety DLLs (kept OUT of the default zip) ----
if "%WITHGL%"=="1" (
  echo [gl] safety DLLs copied into the package itself
) else (
  if exist "%OPTDLL%" rmdir /S /Q "%OPTDLL%"
  mkdir "%OPTDLL%"
  copy /Y "%QTDIR%\bin\opengl32sw.dll"     "%OPTDLL%\" >nul 2>nul
  copy /Y "%QTDIR%\bin\D3Dcompiler_47.dll" "%OPTDLL%\" >nul 2>nul
  echo [gl] safety DLLs parked in %OPTDLL%
  echo      needed only if the pet refuses to start on an old PC or VM -
  echo      copy them into the PetPal folder next to the exe if that happens
)

rem ---------- 11. zip ----------
if "%DOZIP%"=="1" (
  if exist "%SRCDIR%\dist\PetPal.zip" del /Q "%SRCDIR%\dist\PetPal.zip"
  echo [zip] dist\PetPal.zip
  powershell -NoProfile -Command "Compress-Archive -Path '%DIST%\*' -DestinationPath '%SRCDIR%\dist\PetPal.zip' -CompressionLevel Optimal"
  if errorlevel 1 goto fail
)

echo.
echo [OK] package ready:  %DIST%
echo      send this    :  %SRCDIR%\dist\PetPal.zip
endlocal
exit /b 0

:fail
echo.
echo [FAILED] packaging did not finish. First error is above.
endlocal
exit /b 1
