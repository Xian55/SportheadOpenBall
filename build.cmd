@echo off
rem One-shot build from a fresh checkout.
rem   build.cmd          native game + test exes -> ..\SportheadOpenBall_build\native
rem   build.cmd web      wasm build (needs emsdk) -> ..\SportheadOpenBall_build\web
rem   build.cmd test     native build, then the full headless test battery
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

if /i "%~1"=="web"  goto :web
if /i "%~1"=="test" goto :test

:native
if not exist "%OUT%\native\CMakeCache.txt" (
  cmake -S . -B %OUT%\native -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build %OUT%\native -j || exit /b 1
echo.
echo Done. Run %OUT%\native\openball.exe
exit /b 0

:test
call "%~f0" || exit /b 1
for %%t in (fixed_test const_test sim_test replay_test) do (
  if exist "%OUT%\native\%%t.exe" (
    echo --- %%t
    "%OUT%\native\%%t.exe" || exit /b 1
  )
)
echo.
echo All tests passed.
exit /b 0

:web
rem emsdk: EMSDK env var if set, else the ..\emsdk sibling, else F:\Programming\emsdk
rem (an existing checkout on this machine - avoids a second multi-GB bootstrap).
if defined EMSDK (
  call "%EMSDK%\emsdk_env.bat" >nul 2>nul
) else if exist "..\emsdk\emsdk_env.bat" (
  call "..\emsdk\emsdk_env.bat" >nul 2>nul
) else if exist "F:\Programming\emsdk\emsdk_env.bat" (
  call "F:\Programming\emsdk\emsdk_env.bat" >nul 2>nul
)
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
