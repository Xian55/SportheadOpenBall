@echo off
rem Regenerate web\trystero-nostr.min.js from npm, then commit the result and
rem update web\TRYSTERO_VERSION.txt with the new version and hash.
rem
rem The bundle is VENDORED rather than pulled from a CDN: the Pages site must
rem keep working if a CDN changes, rate-limits or is blocked, and a committed
rem bundle means no third-party script origin on a page players keep open. The
rem cost is this manual refresh, which is the right trade for a game that may go
rem months without a commit.
setlocal
cd /d "%~dp0vendor"
where npm >nul 2>nul || (echo npm not found & exit /b 1)

call npm install trystero@latest esbuild --no-audit --no-fund || exit /b 1
call npx esbuild --bundle --format=esm --minify --target=es2020 ^
  node_modules/trystero/dist/nostr.mjs ^
  --outfile=..\..\web\trystero-nostr.min.js || exit /b 1
copy /y node_modules\trystero\LICENSE ..\..\web\trystero-LICENSE.txt >nul

for /f %%v in ('node -e "console.log(require(''./node_modules/trystero/package.json'').version)"') do set VER=%%v
for /f %%h in ('node -e "const c=require(''crypto''),f=require(''fs'');console.log(c.createHash(''sha256'').update(f.readFileSync(''../../web/trystero-nostr.min.js'')).digest(''hex''))"') do set SHA=%%h

echo.
echo trystero %VER%
echo sha256   %SHA%
echo.
echo Update web\TRYSTERO_VERSION.txt with those, then commit the bundle.
exit /b 0
