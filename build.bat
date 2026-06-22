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

REM Ensure Rust/Cargo and gettext are reachable for the build.
set "PATH=%USERPROFILE%\.cargo\bin;%LOCALAPPDATA%\Programs\gettext-iconv\bin;%PATH%"

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
echo === Publishing FilmmakerDemoInfo helper (.dem -^> scoreboard JSON) ===
REM The filmmaker demo browser shells out to this .NET tool for player names.
REM Published next to AfxHookSource2.dll so it is found at runtime.
dotnet publish "%~dp0FilmmakerDemoInfo\FilmmakerDemoInfo.csproj" -c Release -o "%~dp0build\staging-release\bin\x64\FilmmakerDemoInfo"
if errorlevel 1 (
    echo.
    echo WARNING: FilmmakerDemoInfo helper failed to publish; demo names will be
    echo unavailable but the rest of HLAE built fine.
)

echo.
echo === BUILD OK ===
echo Output: %~dp0build\staging-release\bin\HLAE.exe
echo Run launch.bat to start it.
exit /b 0
