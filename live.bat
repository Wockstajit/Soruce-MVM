@echo off
setlocal
REM ============================================================
REM  CS2 camera-path LIVE - one click
REM  1) Restarts CS2 with netcon at 1920x1080.
REM  2) Opens the browser dashboard (live marker count, speed
REM     mode, segment progress, console stream + drive buttons).
REM
REM  Usage:
REM    live.bat                 (CS2 1920x1080, netcon 29010, web 8765)
REM ============================================================

cd /d "%~dp0"

set "PORT=29010"
set "WEBPORT=8765"
set "WIDTH=2560"
set "HEIGHT=1080"

if not exist "misc\cs2-live.ps1" (
    echo cs2-live.ps1 not found ^(expected misc\cs2-live.ps1^).
    pause
    exit /b 1
)

REM --- stop any stale dashboard server on the web port --------
echo Checking for existing live dashboard on port %WEBPORT% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$url = 'http://localhost:%WEBPORT%/stop'; try { Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 2 | Out-Null; Write-Host 'Requested existing dashboard shutdown.'; Start-Sleep -Milliseconds 750 } catch { }"
if errorlevel 1 (
    echo WARNING: could not request shutdown for an existing dashboard on port %WEBPORT%.
)

REM --- relaunch CS2/netcon fresh ------------------------------
REM launch-cs2-netcon.ps1 closes any existing cs2.exe before injecting the staged hook.
echo Launching CS2 with netcon at %WIDTH%x%HEIGHT% on port %PORT% ...
powershell -NoProfile -ExecutionPolicy Bypass -File "misc\launch-cs2-netcon.ps1" -Port %PORT% -Width %WIDTH% -Height %HEIGHT%
if errorlevel 1 (
    echo.
    echo ERROR: failed to launch CS2/netcon.
    pause
    exit /b 1
)

REM --- open the live dashboard (auto-retries until netcon is up)
powershell -NoProfile -ExecutionPolicy Bypass -File "misc\cs2-live.ps1" -Port %PORT% -WebPort %WEBPORT%
if errorlevel 1 (
    echo.
    echo ERROR: live dashboard stopped with an error.
    pause
    exit /b 1
)

echo.
echo Dashboard server stopped.
exit /b 0
