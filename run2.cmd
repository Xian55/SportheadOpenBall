@echo off
rem Two local instances playing each other over UDP loopback.
rem   run2.cmd                 clean link
rem   run2.cmd 100 30 5        lag_ms jitter_ms loss_percent
rem
rem Lag/jitter/loss are injected on the RECEIVE side, so each window is
rem configured independently - which is also why they are passed to both.
setlocal
cd /d "%~dp0"
set "OUT=..\SportheadOpenBall_build\native"
if not exist "%OUT%\openball.exe" ( echo Build first: build.cmd & exit /b 1 )

set "LAG=%~1"
set "JIT=%~2"
set "LOSS=%~3"
if "%LAG%"==""  set "LAG=0"
if "%JIT%"==""  set "JIT=0"
if "%LOSS%"=="" set "LOSS=0"

echo Launching two peers: lag=%LAG%ms jitter=%JIT%ms loss=%LOSS%%%
start "OpenBall A" cmd /c "set OB_ROOM=run2& set OB_PORT=41001& set OB_PEER=127.0.0.1:41002& set OB_LAG_MS=%LAG%& set OB_JITTER_MS=%JIT%& set OB_LOSS=%LOSS%& set OB_NETSTAT=1& set OB_WINSIZE=800x450& "%OUT%\openball.exe""
start "OpenBall B" cmd /c "set OB_ROOM=run2& set OB_PORT=41002& set OB_PEER=127.0.0.1:41001& set OB_LAG_MS=%LAG%& set OB_JITTER_MS=%JIT%& set OB_LOSS=%LOSS%& set OB_NETSTAT=1& set OB_WINSIZE=800x450& "%OUT%\openball.exe""
exit /b 0
