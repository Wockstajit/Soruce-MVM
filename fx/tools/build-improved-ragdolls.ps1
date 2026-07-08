#requires -Version 5
# Standalone Improved Ragdolls build. Completely independent of the particle FX packs
# (Povarehok / Modern) -- its own content/game dirs, gameinfo, and CS2 junctions. It only
# shares the runtime `source_mvm_fx` mount at stage time (both features live under one
# USRLOCALCSGO dir at runtime), and even then it owns solely the
# `models/filmmaker/improved_ragdolls/` subtree.
#
# Pipeline: VRF-decompile current CS2 agents -> relocate to the improved namespace ->
# resourcecompiler -> graft Valve's compiled PHYS block (correct joint frames + 272 mass;
# see fx/tools/convert-improved-ragdolls.py) -> optional stage.
#
#   powershell -ExecutionPolicy Bypass -File fx\tools\build-improved-ragdolls.ps1 -Stage
[CmdletBinding()]
param(
    [string]$Cs2Dir = 'F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive',
    [string]$ResourceCompiler = 'F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\resourcecompiler.exe',
    [string]$VrfCli = 'C:\Users\ayden\Documents\Github Projects\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe',
    [string]$Python = 'python',
    [string]$OutputRoot,
    [switch]$Stage
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $repoRoot 'build\fx\improved-ragdolls' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)

$converter   = Join-Path $PSScriptRoot 'convert-improved-ragdolls.py'
$csgoGameDir = Join-Path $Cs2Dir 'game\csgo'
$vpk         = Join-Path $csgoGameDir 'pak01_dir.vpk'
foreach ($p in @($converter, $ResourceCompiler, $VrfCli, $vpk)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Required path not found: $p" }
}

$contentDir = Join-Path $OutputRoot 'content\source_mvm_fx'
$gameDir    = Join-Path $OutputRoot 'game\source_mvm_fx'
New-Item -ItemType Directory -Path $contentDir, $gameDir -Force | Out-Null

# ---- CS2 base-asset junctions + gameinfo so resourcecompiler resolves skeletons/materials.
function Set-CsJunction([string]$Path, [string]$Target) {
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { [IO.Directory]::Delete($Path) }
        else { throw "Refusing to touch non-junction path: $Path" }
    }
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}
foreach ($g in 'csgo','csgo_imported','csgo_core','core') {
    Set-CsJunction (Join-Path (Split-Path $gameDir -Parent) $g) (Join-Path $Cs2Dir "game\$g")
}
@"
"GameInfo"
{
    game "SOURCE:MVM Ragdolls"
    title "SOURCE:MVM Ragdolls"
    FileSystem
    {
        SearchPaths
        {
            Game source_mvm_fx
            Game csgo
            Game csgo_imported
            Game csgo_core
            Game core
            Mod source_mvm_fx
            Mod csgo
            Mod csgo_imported
            Mod csgo_core
        }
    }
}
"@ | Set-Content -LiteralPath (Join-Path $gameDir 'gameinfo.gi') -Encoding ASCII

# ---- 1. Prepare: VRF-decompile agents + relocate to the improved namespace.
Write-Host "Preparing Improved Ragdoll models (VRF decompile + relocate)..." -ForegroundColor Cyan
& $Python $converter --content-root $contentDir --vrf-cli $VrfCli --cs2-vpk $vpk
if ($LASTEXITCODE -ne 0) { throw "Improved Ragdolls prepare failed." }

# ---- 2. Compile only the improved ragdoll models.
Write-Host "Compiling Improved Ragdoll models..." -ForegroundColor Cyan
& $ResourceCompiler -i (Join-Path $contentDir 'models\filmmaker\improved_ragdolls\*.vmdl') -r -game $gameDir -nop4 -fshallow2
if ($LASTEXITCODE -ne 0) { throw "resourcecompiler.exe failed for improved ragdoll models (exit $LASTEXITCODE)." }

# ---- 3. Graft Valve's compiled PHYS (correct joint frames) into each model.
Write-Host "Grafting Valve ragdoll physics..." -ForegroundColor Cyan
& $Python $converter --graft-phys --game-dir $gameDir --vrf-cli $VrfCli --cs2-vpk $vpk
if ($LASTEXITCODE -ne 0) { throw "Improved Ragdolls PHYS graft failed." }

$compiledDir = Join-Path $gameDir 'models\filmmaker\improved_ragdolls'
$count = @(Get-ChildItem $compiledDir -Recurse -Filter *.vmdl_c -ErrorAction SilentlyContinue).Count
if ($count -le 0) { throw "No compiled improved ragdoll models produced." }
Write-Host "Built $count grafted improved ragdoll models." -ForegroundColor Green

# ---- 4. Optional stage into the shared runtime mount (owns only its own subtree).
if ($Stage) {
    $stageDir = Join-Path $repoRoot 'build\staging-release\fx\source_mvm_fx\models\filmmaker\improved_ragdolls'
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    robocopy $compiledDir $stageDir /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Staging improved ragdoll models failed (robocopy $LASTEXITCODE)." }
    Write-Host "Staged improved ragdoll models to: $stageDir" -ForegroundColor Green
}
