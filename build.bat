@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  HLAE fork - build script (staging, no zip/installer)
REM  Output: build\staging-release\HLAE.exe
REM ============================================================

cd /d "%~dp0"

REM Some hardened shells set this, which stops cmd from finding ShaderBuilder.exe
REM in its own working directory during the shader build step. Clear it.
set "NoDefaultCurrentDirectoryInExePath="

REM Ensure Rust/Cargo, gettext and Go are reachable for the build.
set "PATH=%USERPROFILE%\.cargo\bin;%LOCALAPPDATA%\Programs\gettext-iconv\bin;%ProgramFiles%\Go\bin;%PATH%"

REM ------------------------------------------------------------
REM  Close any running CS2 / HLAE FIRST. The staged AfxHookSource2.dll is loaded
REM  into cs2.exe while the game runs, so the build's install/copy step fails with
REM  "cannot copy file ... being used by another process" unless we free it here.
REM ------------------------------------------------------------
echo === Closing any running CS2 / HLAE so the staged DLL is not locked ===
tasklist /fi "imagename eq cs2.exe" 2>nul | find /i "cs2.exe" >nul
if not errorlevel 1 (
    echo Found cs2.exe - closing it.
    taskkill /f /im cs2.exe >nul 2>nul
)
taskkill /f /im hlae.exe >nul 2>nul
REM Give Windows a moment to release the AfxHookSource2.dll file handle before copying over it.
ping -n 3 127.0.0.1 >nul

echo === Locating Visual Studio 2022 ===
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Visual Studio 2022 is not installed.
    echo Install VS 2022 with the "Desktop development with C++" and
    echo ".NET desktop development" workloads, plus .NET Framework 4.6.2 targeting pack.
    pause
    exit /b 1
)

REM NOTE: don't use `for /f` here - its cmd /c quote-stripping mangles the quoted
REM vswhere path (which lives under "C:\Program Files (x86)\...") once a second
REM quoted arg (-version "[..]") is present, producing a bogus
REM "'C:\Program' is not recognized". Invoke directly and read back via a temp file.
set "VSINSTALL="
"%VSWHERE%" -latest -version "[17.0,18.0)" -property installationPath > "%TEMP%\_hlae_vsinstall.txt"
set /p VSINSTALL=<"%TEMP%\_hlae_vsinstall.txt"
del "%TEMP%\_hlae_vsinstall.txt" 2>nul
if not defined VSINSTALL (
    echo ERROR: Visual Studio 2022 ^(17.x^) not found.
    pause
    exit /b 1
)
echo Found: %VSINSTALL%

echo === Setting up build environment (x64) ===
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 (
    echo ERROR: failed to initialize VS developer environment.
    pause
    exit /b 1
)

echo === Building HLAE (staging release) ===
REM This builds BOTH win32 and x64 parts and stages them to build\staging-release
cmake -DAFX_MULTIBUILD_STAGING=ON -P cmake/MultiBuild.cmake
if errorlevel 1 (
    echo.
    echo ERROR: build failed. See messages above.
    pause
    exit /b 1
)

echo.
echo === Building FilmmakerDemoInfo helper (.dem -^> scoreboard JSON, Go/demoinfocs) ===
REM The filmmaker demo browser shells out to this tool for real player names, the
REM correct end-of-match team sides, MVPs and the per-round timeline. It is a single
REM self-contained Go binary (no .NET runtime needed) built next to AfxHookSource2.dll.
where go >nul 2>nul
if errorlevel 1 (
    echo.
    echo WARNING: 'go' not found on PATH; the demo-info helper was NOT rebuilt.
    echo Install Go ^(https://go.dev/dl/^) so build.bat can produce FilmmakerDemoInfo.exe.
) else (
    set "FM_HELPER_DIR=%~dp0build\staging-release\bin\x64\FilmmakerDemoInfo"
    if exist "!FM_HELPER_DIR!" rmdir /s /q "!FM_HELPER_DIR!"
    mkdir "!FM_HELPER_DIR!"
    pushd "%~dp0FilmmakerDemoInfoGo"
    go build -o "!FM_HELPER_DIR!\FilmmakerDemoInfo.exe" .
    if errorlevel 1 (
        echo.
        echo WARNING: FilmmakerDemoInfo helper failed to build; demo names/sides/MVPs
        echo will be unavailable but the rest of HLAE built fine.
    )
    popd
)

echo.
echo === BUILD OK ===
echo Output: %~dp0build\staging-release\bin\HLAE.exe
echo Starting live dashboard / CS2 in 1 second...
timeout /t 1 /nobreak >nul
call "%~dp0automation\live.bat"
exit /b %ERRORLEVEL%
