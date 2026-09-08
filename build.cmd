@echo off
rem One-shot build from a fresh checkout.
rem   build.cmd              native game + test exes -> ..\SportheadOpenBall_build\native
rem   build.cmd test         native build + headless tests vs the committed goldens
rem   build.cmd determinism  native vs wasm replay, byte-compared (THE gate)
rem   build.cmd web          wasm build (needs emsdk) -> ..\SportheadOpenBall_build\web
rem First configure downloads raylib 5.5 via CMake FetchContent (needs git + network).
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "OUT=..\SportheadOpenBall_build"

rem --- locate gcc: PATH, then winget Links, then the WinLibs package dir ---
where gcc >nul 2>nul
if errorlevel 1 (
  if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\gcc.exe" (
    set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;!PATH!"
  ) else (
    for /d %%i in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
      if exist "%%i\mingw64\bin\gcc.exe" set "PATH=%%i\mingw64\bin;!PATH!"
    )
  )
)
where gcc >nul 2>nul
if errorlevel 1 (
  echo gcc not found. Install the toolchain first:
  echo   winget install BrechtSanders.WinLibs.POSIX.UCRT
  echo   winget install Kitware.CMake
  exit /b 1
)

if /i "%~1"=="web"         goto :web
if /i "%~1"=="test"        goto :test
if /i "%~1"=="determinism" goto :determinism

:native
rem configure only once: cmake --build re-runs it automatically when
rem CMakeLists.txt changes, so skipping it here costs nothing
if not exist "%OUT%\native\CMakeCache.txt" (
  cmake -S . -B %OUT%\native -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build %OUT%\native -j || exit /b 1
echo.
echo Done. Run %OUT%\native\openball.exe
exit /b 0

:test
call "%~f0" || exit /b 1
echo.
echo --- lint_sim (tier-1 purity gate)
bash tools/lint_sim.sh || exit /b 1
echo --- fixed_test
"%OUT%\native\fixed_test.exe" || exit /b 1
echo --- const_test (vs tests\const_golden.txt)
"%OUT%\native\const_test.exe" > "%TEMP%\ob_const.txt" || exit /b 1
fc /w tests\const_golden.txt "%TEMP%\ob_const.txt" >nul || (echo const_test DIFFERS from its golden & exit /b 1)
echo --- replay_test (vs tests\replay_golden.txt)
"%OUT%\native\replay_test.exe" > "%TEMP%\ob_replay.txt" || exit /b 1
fc /w tests\replay_golden.txt "%TEMP%\ob_replay.txt" >nul || (echo replay_test DIFFERS - determinism moved. Do NOT re-bless without understanding why. & exit /b 1)
echo.
echo All tests passed. For the native-vs-wasm gate: build.cmd determinism
exit /b 0

:determinism
rem The gate the rollback design rests on: the SAME replay built for native AND
rem for wasm, run through both toolchains, byte-compared.
call "%~f0" || exit /b 1
call :findemsdk
where emcmake >nul 2>nul
if errorlevel 1 (
  echo emsdk not found. Point EMSDK at a checkout, e.g. set EMSDK=F:\Programming\emsdk
  exit /b 1
)
call emcmake cmake -S . -B %OUT%\web -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web -DOB_DEV_TOOLS=ON || exit /b 1
cmake --build %OUT%\web -j --target replay_test || exit /b 1
"%OUT%\native\replay_test.exe" > "%TEMP%\ob_native.txt" || exit /b 1
node "%OUT%\web\replay_test.js" > "%TEMP%\ob_wasm.txt" || exit /b 1
fc /w "%TEMP%\ob_native.txt" "%TEMP%\ob_wasm.txt" >nul || (echo NATIVE AND WASM DISAGREE - determinism is broken & exit /b 1)
echo.
echo native == wasm, byte for byte.
exit /b 0

:web
call :findemsdk
where emcmake >nul 2>nul
if errorlevel 1 (
  echo emsdk not found. Either point EMSDK at an existing checkout:
  echo   set EMSDK=F:\Programming\emsdk
  echo or install one next to this project:
  echo   git clone https://github.com/emscripten-core/emsdk ..\emsdk
  echo   ..\emsdk\emsdk install latest ^&^& ..\emsdk\emsdk activate latest
  exit /b 1
)
if not exist "%OUT%\web\CMakeCache.txt" (
  call emcmake cmake -S . -B %OUT%\web -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web -DOB_DEV_TOOLS=OFF || exit /b 1
)
cmake --build %OUT%\web -j || exit /b 1
echo.
echo Done. Run serve.cmd and open http://localhost:8080/
exit /b 0

rem Locate emsdk and put emcmake on PATH.
rem
rem Deliberately does NOT call emsdk_env.bat: that script resolves paths against
rem the current directory and fails outright when the checkout lives on a drive
rem the cmd process cannot cd into (a mapped drive, for instance). Setting PATH
rem and EM_CONFIG directly works wherever the checkout is.
:findemsdk
set "EMROOT="
if defined EMSDK if exist "%EMSDK%\upstream\emscripten\emcmake.py" set "EMROOT=%EMSDK%"
if not defined EMROOT if exist "..\emsdk\upstream\emscripten\emcmake.py" set "EMROOT=..\emsdk"
if not defined EMROOT if exist "F:\Programming\emsdk\upstream\emscripten\emcmake.py" set "EMROOT=F:\Programming\emsdk"
if not defined EMROOT goto :eof
set "PATH=%EMROOT%\upstream\emscripten;%EMROOT%;%PATH%"
if exist "%EMROOT%\.emscripten" set "EM_CONFIG=%EMROOT%\.emscripten"
set "EMSDK=%EMROOT%"
goto :eof
