@echo off
rem Serve the web build on :8080. Emscripten output must be served over HTTP,
rem not opened as a file:// URL.
cd /d "%~dp0..\SportheadOpenBall_build\web" || exit /b 1
echo Serving %CD% at http://localhost:8080/
python -m http.server 8080
