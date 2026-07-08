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

echo === FX asset packs (Povarehok + Modern) ===
REM Ask about the slow particle rebuild before starting the normal build sequence.
REM One converter run (fx\tools\convert-povarehok-source1.ps1 -Compile) builds BOTH packs:
REM   - Povarehok  (On/Less modes)   from reference\csgo effect mod\
REM   - Modern     (MW2019 modes)    from the committed fx\sources\modern-warfare-gmod\ tree
REM No GMod install is needed anymore -- the Modern source tree lives in the repo.
REM The conversion takes several minutes and the SOURCE files almost never change, so it is
REM opt-in per build: a 3-second Y/N prompt that defaults to NO keeps day-to-day builds fast.
REM EXCEPTION: if no compiled pack exists on disk at all (fresh checkout, or a previous
REM conversion was cancelled mid-run, which wipes the pack before rebuilding it), the rebuild
REM is forced -- otherwise CS2 would silently launch with the effect system failing open to
REM vanilla. The finished pack is a generated artifact (~150 MB after the converter prunes
REM to the runtime closure derived from ParticleFx.cpp; not committed to git). It is staged
REM into build\staging-release\fx\source_mvm_fx after the normal build completes so a shipped
REM build carries it, and launch-cs2-netcon.ps1 mounts it via USRLOCALCSGO.
set "FM_FX_PACK_DIR=%~dp0build\fx\povarehok-source1import\source2\game\source_mvm_fx"
set "FM_FX_REBUILD=0"
if not exist "%FM_FX_PACK_DIR%\particles" (
    echo No compiled FX pack found on disk - rebuilding it now ^(required, ~a few minutes^).
    set "FM_FX_REBUILD=1"
) else (
    choice /c YN /t 3 /d N /m "Rebuild the particle FX packs before the full build (auto-No in 3s)"
    if not errorlevel 2 set "FM_FX_REBUILD=1"
)
echo.

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

if "!FM_FX_REBUILD!"=="1" (
    echo Rebuilding converted FX asset packs ^(Povarehok + Modern^) first...
    REM -NonInteractive: any unexpected confirmation prompt fails the step (caught by the
    REM WARNING branch below) instead of blocking the whole build waiting for keyboard input.
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0fx\tools\convert-povarehok-source1.ps1" -Compile
    if errorlevel 1 (
        echo.
        echo ============================================================
        echo WARNING: FX asset conversion FAILED; the full build will continue
        echo with whatever pack ^(if any^) is already on disk, or launch with
        echo vanilla particles. Scroll up for the failing step.
        echo ============================================================
        echo Continuing with the full build in 8 seconds ^(Ctrl+C to stop and read^)...
        timeout /t 8 >nul
    )
) else (
    echo Keeping the existing FX pack ^(skipped; answer Y within 3s to rebuild^).
)
echo.

echo === Improved Ragdolls ^(standalone; independent of the particle FX packs^) ===
REM Ragdolls are built by their OWN pipeline (fx\tools\build-improved-ragdolls.ps1) into a
REM separate build tree; they only share the runtime source_mvm_fx mount at stage time and
REM own solely the models\filmmaker\improved_ragdolls subtree there. Never fold this into the
REM particle converter. Same opt-in model: forced if none exist, else a 3-second Y/N.
set "FM_RAGDOLL_DIR=%~dp0build\staging-release\fx\source_mvm_fx\models\filmmaker\improved_ragdolls"
set "FM_RAGDOLL_REBUILD=0"
if not exist "%FM_RAGDOLL_DIR%" (
    echo No compiled Improved Ragdolls found on disk - building them now.
    set "FM_RAGDOLL_REBUILD=1"
) else (
    choice /c YN /t 3 /d N /m "Rebuild the Improved Ragdolls before the full build (auto-No in 3s)"
    if not errorlevel 2 set "FM_RAGDOLL_REBUILD=1"
)
if "!FM_RAGDOLL_REBUILD!"=="1" (
    echo Building Improved Ragdolls...
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0fx\tools\build-improved-ragdolls.ps1" -Stage
    if errorlevel 1 (
        echo WARNING: Improved Ragdolls build FAILED; continuing ^(ragdoll toggle falls back to stock physics^).
        timeout /t 5 >nul
    )
) else (
    echo Keeping the existing Improved Ragdolls ^(skipped; answer Y within 3s to rebuild^).
)
echo.

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
REM Let MSBuild build independent projects in parallel (/m). Per-file parallelism
REM within a project comes from /MP set in the root CMakeLists.txt.
set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"
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

REM --- Stage the compiled pack into the shipped build so a release is self-contained ---
REM Mirror the generated game dir next to HLAE.exe. robocopy exit codes 0-7 are success;
REM only >=8 is a real failure, so guard on errorlevel 8 (a bare errorlevel 1 would false-fail).
set "FM_FX_STAGE_DIR=%~dp0build\staging-release\fx\source_mvm_fx"
if exist "%FM_FX_PACK_DIR%\particles" (
    echo Staging compiled FX pack into the release folder: %FM_FX_STAGE_DIR%
    REM /XD excludes the Improved Ragdolls subtree: it is owned + staged by the separate
    REM standalone ragdoll build, so this particle /MIR must not delete or overwrite it.
    robocopy "%FM_FX_PACK_DIR%" "%FM_FX_STAGE_DIR%" /MIR /XD "%FM_FX_STAGE_DIR%\models\filmmaker\improved_ragdolls" /NFL /NDL /NJH /NJS /NP /R:1 /W:1 >nul
    if errorlevel 8 (
        echo WARNING: failed to stage the FX pack into build\staging-release; the shipped
        echo build may launch without custom particles. Scroll up for robocopy errors.
    )
) else (
    echo No compiled FX pack on disk to stage into the release folder ^(skipped^).
)

echo.
echo === BUILD OK ===
echo Output: %~dp0build\staging-release\bin\HLAE.exe
echo Starting live dashboard / CS2 in 1 second...
timeout /t 1 /nobreak >nul
call "%~dp0automation\launch\live.bat"
exit /b %ERRORLEVEL%
