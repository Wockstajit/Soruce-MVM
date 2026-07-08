#requires -Version 5
<#
Converts the local CS:GO Povarehok mod variants for the CS2 runtime effect system.

Requires a checkout of https://github.com/long0900/source1import with its Python
dependencies installed, plus `pip install srctools vtf2img` for lossless VTF export.
The script stages the Source 1 PCFs into the Source 2 resource
namespaces used by AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp:

  On   -> particles/filmmaker/povarehok/regular/...
  Less -> particles/filmmaker/povarehok/less/impacts/... and less/smoke/...
         (combined per file: impacts from less/impacts, the rest from less/smoke)

Usage:
  powershell -ExecutionPolicy Bypass -File fx/tools/convert-povarehok-source1.ps1 `
    -Source1ImportDir C:\path\to\source1import -Compile
#>
[CmdletBinding()]
param(
    # All three tool/asset inputs default to repo-local copies so a fresh checkout converts
    # without any machine-specific setup: the converters are cloned under misc\ and the two
    # stock CS:GO model dependencies are bundled (extracted from pak01) next to the mod.
    [string]$Source1ImportDir = '',
    [string]$Source2ConverterDir = '',
    [string]$LegacyCsgoDir = '',
    [string]$Python = 'python',
    [string]$OutputRoot,
    [string]$Cs2Dir = 'F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive',
    [string]$ResourceCompiler = 'F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\resourcecompiler.exe',
    [string]$VrfCli = 'C:\Users\ayden\Documents\Github Projects\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe',
    # MW2019 "modern" pack sources. These are now COMMITTED to the repo under
    # fx\sources\modern-warfare-gmod\ (the ~44 MB extracted Source 1 tree), so a normal
    # build never touches a GMod install. The GMod paths below are only used when the
    # committed tree is absent, or when -RefreshModernFromGmod is passed to re-extract it
    # from an updated GMod (writes straight back into the committed folder).
    [string]$ModernSourceDir = '',
    [string]$GmodWorkshopRoot = 'G:\SteamLibrary\steamapps\workshop\content\4000',
    [string]$GmodRoot = 'G:\SteamLibrary\steamapps\common\GarrysMod',
    [switch]$RefreshModernFromGmod,
    [switch]$SkipModern,
    [switch]$IncludeInsurgencySmoke,
    [switch]$Compile
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingPath([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label is required."
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-Under([string]$Path, [string]$Root, [string]$Label) {
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (-not ($fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + '\', [StringComparison]::OrdinalIgnoreCase))) {
        throw "$Label must stay under $fullRoot, got $fullPath"
    }
}

function Copy-TreeContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    Ensure-Directory $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

function Copy-EffectPcf([string]$VariantSource, [string]$VariantKey, [string]$RelativePath, [string]$StageRoot) {
    $src = Join-Path $VariantSource $RelativePath
    if (-not (Test-Path -LiteralPath $src)) {
        # Expected for some variant/PCF combos -- the mod author did not ship every PCF in every
        # variant (e.g. polished_weapons_fx.pcf exists only in the two classic variants, not in
        # lessimpacts/lesssmoke). The variant is still complete; nothing is being skipped that
        # exists on disk.
        Write-Host "Note: $VariantKey does not ship $RelativePath (normal for this variant); skipping." -ForegroundColor DarkGray
        return
    }
    $insideParticles = $RelativePath -replace '^particles\\', ''
    $dst = Join-Path (Join-Path $StageRoot "particles\filmmaker\povarehok\$VariantKey") $insideParticles
    Ensure-Directory (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

function Resolve-LegacyCsgoGameRoot([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $root = Resolve-ExistingPath $Path 'LegacyCsgoDir'
    if (Test-Path -LiteralPath (Join-Path $root 'models')) {
        return $root
    }
    $gameCsgo = Join-Path $root 'game\csgo'
    if (Test-Path -LiteralPath (Join-Path $gameCsgo 'models')) {
        return (Resolve-Path -LiteralPath $gameCsgo).Path
    }
    $csgo = Join-Path $root 'csgo'
    if (Test-Path -LiteralPath (Join-Path $csgo 'models')) {
        return (Resolve-Path -LiteralPath $csgo).Path
    }
    throw "LegacyCsgoDir must point at a CS:GO game root containing models\, or an install containing game\csgo\: $Path"
}

function Copy-LegacyModelDependency([string]$LegacyGameRoot, [string]$RelativeMdl, [string]$StageRoot) {
    $base = $RelativeMdl.Substring(0, $RelativeMdl.Length - 4)
    foreach ($ext in @('.mdl', '.vvd', '.dx90.vtx', '.phy')) {
        $rel = $base + $ext
        $source = Join-Path $LegacyGameRoot $rel
        if (-not (Test-Path -LiteralPath $source)) {
            if ($ext -eq '.phy') { continue }
            throw "Required legacy CS:GO model dependency missing: $rel under $LegacyGameRoot"
        }
        $destination = Join-Path $StageRoot $rel
        Ensure-Directory (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

function Remove-GeneratedJunction([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        throw "Refusing to remove non-junction generated dependency path: $Path"
    }
    # Delete ONLY the reparse point. Remove-Item (without -Recurse) sees the junction's target
    # children and blocks on an interactive "has children ... Are you sure?" confirmation --
    # which hangs/kills the build.bat pipeline run; -Recurse is NOT a safe fix (on some
    # PowerShell versions it follows the junction and deletes the REAL CS2 game files it points
    # at). Directory.Delete(non-recursive) is the junction-safe, prompt-free way.
    [System.IO.Directory]::Delete($item.FullName, $false)
}

function Ensure-GeneratedJunction([string]$Path, [string]$Target) {
    if (Test-Path -LiteralPath $Path) {
        Remove-GeneratedJunction $Path
    }
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path

# Resolved here (not as param() defaults) because $PSScriptRoot reads empty during
# param-block default evaluation under `powershell -File` in this environment (repro'd
# with [CmdletBinding()] + a $PSScriptRoot-based default; harmless without CmdletBinding,
# but keeping CmdletBinding and resolving defaults in the body is the robust fix).
if ([string]::IsNullOrWhiteSpace($Source1ImportDir)) {
    $Source1ImportDir = if ($env:SOURCE1IMPORT_DIR) { $env:SOURCE1IMPORT_DIR } else { Join-Path $repoRoot 'misc\source1import' }
}
if ([string]::IsNullOrWhiteSpace($Source2ConverterDir)) {
    $Source2ConverterDir = if ($env:SOURCE2CONVERTER_DIR) { $env:SOURCE2CONVERTER_DIR } else { Join-Path $repoRoot 'misc\Source2Converter' }
}
if ([string]::IsNullOrWhiteSpace($LegacyCsgoDir)) {
    $LegacyCsgoDir = if ($env:LEGACY_CSGO_DIR) { $env:LEGACY_CSGO_DIR } else { Join-Path $repoRoot 'reference\csgo effect mod\legacy_csgo_deps' }
}
# The MW2019 "modern" pack Source 1 tree lives in-repo so builds don't need a GMod install.
if ([string]::IsNullOrWhiteSpace($ModernSourceDir)) {
    $ModernSourceDir = if ($env:MODERN_SOURCE_DIR) { $env:MODERN_SOURCE_DIR } else { Join-Path $repoRoot 'fx\sources\modern-warfare-gmod' }
}

$modRoot = Resolve-ExistingPath (Join-Path $repoRoot 'reference\csgo effect mod') 'Povarehok mod root'
# The ~1 GB compiled pack is a generated build artifact, so it lives under build\ (git-ignored),
# NOT under automation\ (which is test-only). build.bat stages the finished game dir into
# build\staging-release\fx\source_mvm_fx so a shipped build carries it.
$safeOutputRoot = Ensure-Directory (Join-Path $repoRoot 'build\fx')
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $safeOutputRoot 'povarehok-source1import'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
Assert-Under $OutputRoot $safeOutputRoot 'OutputRoot'

$source1ImportRoot = Resolve-ExistingPath $Source1ImportDir 'Source1ImportDir'
$source1ImportUtils = Resolve-ExistingPath (Join-Path $source1ImportRoot 'utils') 'source1import utils folder'
$materialsImport = Resolve-ExistingPath (Join-Path $source1ImportUtils 'materials_import.py') 'materials_import.py'
$particlesImport = Resolve-ExistingPath (Join-Path $source1ImportUtils 'particles_import.py') 'particles_import.py'
$textureExporter = Resolve-ExistingPath (Join-Path $PSScriptRoot 'export-source1-vtf.py') 'VTF texture exporter'
$particlePostprocess = Resolve-ExistingPath (Join-Path $PSScriptRoot 'postprocess_povarehok.py') 'Povarehok post-process tool'
$insurgencySmokeExtractor = Join-Path $PSScriptRoot 'extract-insurgency-smoke.py'
$insurgencySmokeStager = Join-Path $PSScriptRoot 'stage-insurgency-smoke.py'
$ragdollConverter = Resolve-ExistingPath (Join-Path $PSScriptRoot 'convert-improved-ragdolls.py') 'Improved ragdoll converter'
$insurgencySourceDir = Join-Path $repoRoot 'fx\sources\insurgency-sandstorm'
$source2ConverterRoot = Resolve-ExistingPath $Source2ConverterDir 'Source2ConverterDir'
$modelConverter = Resolve-ExistingPath (Join-Path $source2ConverterRoot 'convert_model.py') 'Source2Converter convert_model.py'

if (Test-Path -LiteralPath $OutputRoot) {
    Assert-Under (Resolve-Path -LiteralPath $OutputRoot).Path $safeOutputRoot 'OutputRoot'
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\csgo')
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\csgo_imported')
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\csgo_core')
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\core')
    Remove-Item -LiteralPath $OutputRoot -Recurse -Force
}

$src1Root = Ensure-Directory (Join-Path $OutputRoot 'source1_game')
$source2Root = Ensure-Directory (Join-Path $OutputRoot 'source2')
$contentDir = Ensure-Directory (Join-Path $source2Root 'content\source_mvm_fx')
$gameDir = Ensure-Directory (Join-Path $source2Root 'game\source_mvm_fx')
$csgoGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\csgo') 'CS2 game\csgo folder'
$csgoImportedGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\csgo_imported') 'CS2 game\csgo_imported folder'
$csgoCoreGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\csgo_core') 'CS2 game\csgo_core folder'
$coreGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\core') 'CS2 game\core folder'
$csgoJunction = Join-Path $source2Root 'game\csgo'
$csgoImportedJunction = Join-Path $source2Root 'game\csgo_imported'
$csgoCoreJunction = Join-Path $source2Root 'game\csgo_core'
$coreJunction = Join-Path $source2Root 'game\core'
Ensure-GeneratedJunction $csgoJunction $csgoGameDir
Ensure-GeneratedJunction $csgoImportedJunction $csgoImportedGameDir
Ensure-GeneratedJunction $csgoCoreJunction $csgoCoreGameDir
Ensure-GeneratedJunction $coreJunction $coreGameDir

# NOTE: the plain-classic folder (p_betterparticlesmod_classic_c057b) was removed 2026-07-02:
# it was byte-identical to "classic updated" (md5 over every file), so converting it doubled
# the particle-compile time for a mode nobody could tell apart. Runtime mapping (ParticleFxRules.cpp)
# now points On at regular; Less remains the per-file combination of the two folders
# below (their only real deltas vs regular are impact_fx.pcf and explosions_fx.pcf).
$variantSpecs = @(
    @{ Key = 'regular';      Dir = Join-Path $modRoot 'p_betterparticlesmod_classic updated_c057b\p_betterparticlesmod_classic' },
    @{ Key = 'less\impacts'; Dir = Join-Path $modRoot 'p_betterparticlesmod_lessimpacts\p_betterparticlesmod_lessimpacts' },
    @{ Key = 'less\smoke';   Dir = Join-Path $modRoot 'p_betterparticlesmod_lesssmoke_22ac2\p_betterparticlesmod_lesssmoke' }
)

$effectPcfs = @(
    'particles\blood_impact.pcf',
    'particles\impact_fx.pcf',
    'particles\impact_fxmoney.pcf',
    'particles\impact_fxsnow.pcf',
    'particles\impact_fx_smoke.pcf',
    'particles\explosions_fx.pcf',
    'particles\explosions_fx2.pcf',
    'particles\inferno_fx.pcf',
    'particles\inferno_fx3.pcf',
    'particles\weapons\cs_weapon_fx.pcf',
    'particles\polished_weapons_fx.pcf'
)

foreach ($variant in $variantSpecs) {
    $variantDir = Resolve-ExistingPath $variant.Dir "variant folder $($variant.Key)"
    Copy-TreeContents (Join-Path $variantDir 'materials') (Join-Path $src1Root 'materials')
    Copy-TreeContents (Join-Path $variantDir 'models') (Join-Path $src1Root 'models')
    foreach ($pcf in $effectPcfs) {
        Copy-EffectPcf $variantDir $variant.Key $pcf $src1Root
    }
}

# ---- MW2019 "modern" pack staging (particles/filmmaker/modern/...) ----
# The Modern pack's Source 1 tree (3 ARC9/MW2019 PCFs + their material closure) is
# COMMITTED in-repo under fx\sources\modern-warfare-gmod\, so a normal build reads it
# directly with no GMod install required. It gets merged into the same source1 conversion
# root so one materials/particles import pass covers both packs; Povarehok materials win
# any same-path collision (their converted look is already validated in-game).
#
# -RefreshModernFromGmod re-extracts that committed tree from a local GMod install first
# (for pulling in an updated addon); otherwise the GMod paths are never touched.
$modernAvailable = $false
if (-not $SkipModern) {
    if ($RefreshModernFromGmod) {
        $modernExtractor = Join-Path $PSScriptRoot 'extract-modern-particles-gmod.py'
        if ((Test-Path -LiteralPath $GmodWorkshopRoot) -and (Test-Path -LiteralPath $GmodRoot)) {
            Write-Host "Refreshing committed Modern source tree from GMod: $ModernSourceDir" -ForegroundColor Cyan
            & $Python $modernExtractor --workshop-root $GmodWorkshopRoot --gmod-root $GmodRoot --output $ModernSourceDir
            if ($LASTEXITCODE -ne 0) {
                throw "Modern pack re-extraction from GMod failed with exit code $LASTEXITCODE"
            }
        } else {
            throw "-RefreshModernFromGmod requires a GMod install; not found at $GmodWorkshopRoot / $GmodRoot"
        }
    }

    $modernPcfSrc = Join-Path $ModernSourceDir 'particles\filmmaker\modern'
    if (Test-Path -LiteralPath $modernPcfSrc) {
        Write-Host "Staging committed MW2019 modern particle sources from: $ModernSourceDir" -ForegroundColor Cyan
        $modernMaterials = Join-Path $ModernSourceDir 'materials'
        if (Test-Path -LiteralPath $modernMaterials) {
            Get-ChildItem -LiteralPath $modernMaterials -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($ModernSourceDir.Length + 1)
                $dst = Join-Path $src1Root $rel
                if (-not (Test-Path -LiteralPath $dst)) {
                    Ensure-Directory (Split-Path -Parent $dst) | Out-Null
                    Copy-Item -LiteralPath $_.FullName -Destination $dst
                }
            }
        }
        $modernPcfDst = Ensure-Directory (Join-Path $src1Root 'particles\filmmaker\modern')
        Get-ChildItem -LiteralPath $modernPcfSrc -Filter '*.pcf' | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $modernPcfDst -Force
        }
        $modernAvailable = $true
    } else {
        Write-Warning "Committed Modern source tree not found at $ModernSourceDir; skipping the MW2019 modern pack. (Run with -RefreshModernFromGmod, or restore fx\sources\modern-warfare-gmod\.)"
    }
}

$legacyModelDependencies = @(
    'models\props_debris\concrete_chunk07a.mdl',
    'models\gibs\wood_gib01b.mdl'
)
# The gib models' own materials, so the converted VMDLs are not textureless in CS2.
$legacyMaterialDependencies = @(
    'materials\models\gibs\woodgibs\woodgibs02.vmt',
    'materials\models\gibs\woodgibs\woodgibs02.vtf',
    'materials\models\props_debris\concretedebris_chunk01.vmt',
    'materials\models\props_debris\concretedebris_chunk01.vtf'
)
$legacyCsgoGameRoot = Resolve-LegacyCsgoGameRoot $LegacyCsgoDir
if ($legacyCsgoGameRoot) {
    Write-Host "Staging exact legacy CS:GO model dependencies from: $legacyCsgoGameRoot" -ForegroundColor Cyan
    foreach ($mdl in $legacyModelDependencies) {
        Copy-LegacyModelDependency $legacyCsgoGameRoot $mdl $src1Root
    }
    foreach ($rel in $legacyMaterialDependencies) {
        $source = Join-Path $legacyCsgoGameRoot $rel
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Required legacy CS:GO material dependency missing: $rel under $legacyCsgoGameRoot"
        }
        $destination = Join-Path $src1Root $rel
        Ensure-Directory (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
} else {
    Write-Warning "No LegacyCsgoDir supplied. Exact CS:GO stock model dependencies will remain unresolved: $($legacyModelDependencies -join ', ')"
}

@'
"GameInfo"
{
    game "SOURCE:MVM Povarehok Import"
    FileSystem
    {
        SearchPaths
        {
            Game "."
        }
    }
}
'@ | Set-Content -LiteralPath (Join-Path $src1Root 'gameinfo.txt') -Encoding ASCII

@"
"GameInfo"
{
    game "SOURCE:MVM FX"
    title "SOURCE:MVM FX"
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

Write-Host "Staged Povarehok variants under: $src1Root" -ForegroundColor Green
Write-Host "Exporting original VTF textures for source1import..." -ForegroundColor Cyan
$textureManifest = Join-Path $OutputRoot 'texture-export.json'
& $Python $textureExporter --input (Join-Path $src1Root 'materials') --output (Join-Path $contentDir 'materials') --manifest $textureManifest
if ($LASTEXITCODE -ne 0) { throw "VTF texture export failed with exit code $LASTEXITCODE" }

# Generated VMATs reference source1import's standard default textures. CS2 ships the
# authoritative copies in content/core; keep them in this isolated content root so
# ResourceCompiler can resolve associated VTEX inputs.
$defaultMaterialSource = Resolve-ExistingPath (Join-Path $Cs2Dir 'content\core\materials\default') 'CS2 default material sources'
$defaultMaterialDestination = Ensure-Directory (Join-Path $contentDir 'materials\default')
foreach ($name in @('default.tga', 'default_trans.tga', 'default_rough_s1import.tga')) {
    $source = Resolve-ExistingPath (Join-Path $defaultMaterialSource $name) "CS2 default texture $name"
    Copy-Item -LiteralPath $source -Destination (Join-Path $defaultMaterialDestination $name) -Force
}

Write-Host "Running source1import material conversion..." -ForegroundColor Cyan
Push-Location $source1ImportUtils
try {
    & $Python $materialsImport -i $src1Root -e $contentDir -b cs2
    if ($LASTEXITCODE -ne 0) { throw "materials_import.py failed with exit code $LASTEXITCODE" }

    Write-Host "Running source1import particle conversion..." -ForegroundColor Cyan
    & $Python $particlesImport -i $src1Root -e $contentDir -b cs2
    if ($LASTEXITCODE -ne 0) { throw "particles_import.py failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

# ---- Insurgency Sandstorm smoke staging (optional) ----
if ($IncludeInsurgencySmoke) {
    $insurgencyRoot = if ($env:INSURGENCY_DIR) { $env:INSURGENCY_DIR } else { 'F:\SteamLibrary\steamapps\common\insurgency2' }
    $insurgencyPicks = Join-Path $insurgencySourceDir 'picks.json'
    if (-not (Test-Path -LiteralPath $insurgencyPicks)) {
        if (Test-Path -LiteralPath $insurgencyRoot) {
            Write-Host "Extracting Insurgency smoke sources into $insurgencySourceDir..." -ForegroundColor Cyan
            & $Python $insurgencySmokeExtractor --insurgency-root $insurgencyRoot
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Insurgency smoke export failed; post-process will use in-pack fallbacks."
            }
        } else {
            Write-Warning "Insurgency install not found at $insurgencyRoot; skipping Insurgency smoke export."
        }
    }
    if (Test-Path -LiteralPath $insurgencyPicks) {
        Write-Host "Staging Insurgency smoke materials into content tree..." -ForegroundColor Cyan
        & $Python $insurgencySmokeStager --content-root $contentDir
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Insurgency smoke staging failed; post-process will use in-pack fallbacks."
        }
    }
}

Write-Host "Applying CS2-specific Povarehok VPCF post-process..." -ForegroundColor Cyan
Write-Host "  (scans ~7k converted .vpcf files; progress lines follow, usually ~30-90s)" -ForegroundColor DarkGray
$env:PYTHONUNBUFFERED = "1"
& $Python $particlePostprocess --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Povarehok post-process failed with exit code $LASTEXITCODE" }

# Current CS2 rejects source1import's legacy m_sMDLFilename wrapper. Convert the
# mod-bundled MDL/VVD/VTX files to ModelDoc 28 with REDxEYE/Source2Converter.
Write-Host "Running Source2Converter model conversion..." -ForegroundColor Cyan
& $Python $modelConverter -g CS2 -a $contentDir -m (Join-Path $src1Root 'models')
if ($LASTEXITCODE -ne 0) { throw "Source2Converter model conversion failed with exit code $LASTEXITCODE" }

# Several bundled files have an internal MDL name that differs from their actual Source 1
# file path. Particle definitions use the file path, so provide exact-path aliases to the
# same converted ModelDoc and DMX data.
$modelAliases = @{
    'models\shells\shell_12gauge.vmdl' = 'models\shells\mw2019\shell_12gauge.vmdl'
    'models\shells\shell_50cal.vmdl' = 'models\shells\mw2019\shell_50cal.vmdl'
    'models\shells\shell_pistol.vmdl' = 'models\shells\mw2019\shell_pistol.vmdl'
    'models\shells\shell_rifle.vmdl' = 'models\shells\mw2019\shell_rifle.vmdl'
    'models\usMoney\usmoney bills.vmdl' = 'models\usMoney\usmoney_bills.vmdl'
}
foreach ($alias in $modelAliases.GetEnumerator()) {
    $source = Resolve-ExistingPath (Join-Path $contentDir $alias.Value) "converted model $($alias.Value)"
    $destination = Join-Path $contentDir $alias.Key
    Ensure-Directory (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

# Rebuild the Source 1 Improved Ragdolls profile as native ModelDoc physics on every
# current CS2 CT/T player model. VRF preserves Valve's render meshes and skeletons;
# the converter changes physics only and intentionally ignores hostage/audio assets.
Write-Host "Preparing CS2 Improved Ragdolls model overrides..." -ForegroundColor Cyan
& $Python $ragdollConverter --content-root $contentDir `
    --vrf-cli $VrfCli `
    --cs2-vpk (Join-Path $csgoGameDir 'pak01_dir.vpk')
if ($LASTEXITCODE -ne 0) { throw "Improved Ragdolls conversion failed." }

if ($Compile) {
    $resourceCompilerPath = Resolve-ExistingPath $ResourceCompiler 'ResourceCompiler'
    Resolve-ExistingPath (Join-Path $gameDir 'gameinfo.gi') 'generated gameinfo.gi' | Out-Null

    # ---- Prune the content tree to the runtime closure BEFORE compiling ----
    # The mod ships far more than the DLL's swap tables ever reference (measured
    # 2026-07-03: 969 MB / 8,743 compiled files, of which only ~143 MB / ~930 files
    # are reachable from the DLL's swap tables -- the rest is bundled third-party material
    # packs and unswapped particle systems). The validator derives the exact keep-set
    # from the ParticleFx sources themselves (we pass ParticleFx.cpp; the validator scans
    # every sibling ParticleFx* file, since the FXRULE tables live in ParticleFxRules.cpp
    # after the 2026-07-04 split -- so a new FXRULE automatically keeps its assets);
    # everything else is deleted from this generated tree so resourcecompiler never
    # sees it. Cuts the shipped pack ~85% and the compile time by minutes.
    $validator = Resolve-ExistingPath (Join-Path $PSScriptRoot 'validate-povarehok-assets.py') 'Povarehok asset validator'
    $particleFxCpp = Resolve-ExistingPath (Join-Path $repoRoot 'AfxHookSource2\Filmmaker\Movie\ParticleFx.cpp') 'ParticleFx.cpp'
    $validationReport = Join-Path $OutputRoot 'validation.json'
    $runtimeRoots = Join-Path $OutputRoot 'runtime-roots.txt'
    $closureKeepList = Join-Path $OutputRoot 'closure-keep.txt'
    Write-Host "Computing runtime resource closure for pruning..." -ForegroundColor Cyan
    $closureArgs = @('--particle-fx-cpp', $particleFxCpp, '--content-root', $contentDir,
        '--game-root', $gameDir, '--file-list', $runtimeRoots, '--report', $validationReport,
        '--emit-closure', $closureKeepList)
    if ($modernAvailable) { $closureArgs += '--require-modern' }
    & $Python $validator @closureArgs
    if ($LASTEXITCODE -ne 0) { throw "Runtime closure computation failed (see $validationReport); refusing to prune/compile." }

    $keepSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in Get-Content -LiteralPath $closureKeepList) {
        if (-not [string]::IsNullOrWhiteSpace($line)) { [void]$keepSet.Add($line.Trim().Replace('\', '/')) }
    }
    if ($keepSet.Count -lt 100) { throw "Closure keep-set suspiciously small ($($keepSet.Count) entries); refusing to prune." }
    $contentPrefix = (Resolve-Path -LiteralPath $contentDir).Path
    $pruned = 0; $kept = 0
    # Only compile-group inputs are pruned (.vpcf/.vmat/.vtex); tga/mks/dmx inputs are
    # pulled on demand by whatever kept file references them, so they can stay.
    # materials\default\ holds the compiler's own fallback textures -- never prune it.
    foreach ($sub in @('materials', 'particles\filmmaker')) {
        $dir = Join-Path $contentDir $sub
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object {
            $_.Extension -in @('.vpcf', '.vmat', '.vtex')
        } | ForEach-Object {
            $rel = $_.FullName.Substring($contentPrefix.Length + 1).Replace('\', '/')
            if ($rel.StartsWith('materials/default/', [StringComparison]::OrdinalIgnoreCase)) { $kept++; return }
            if ($keepSet.Contains($rel)) { $kept++ } else { Remove-Item -LiteralPath $_.FullName -Force; $pruned++ }
        }
    }
    Write-Host ("Pruned content tree to runtime closure: kept {0}, removed {1} unreferenced vpcf/vmat/vtex sources." -f $kept, $pruned) -ForegroundColor Green

    $compileGroups = @(
        @{ Label = 'materials'; Input = Join-Path $contentDir 'materials\*.vmat'; AllowPartial = $true },
        @{ Label = 'direct particle textures'; Input = Join-Path $contentDir 'materials\*.vtex'; AllowPartial = $false },
        @{ Label = 'models'; Input = Join-Path $contentDir 'models\*.vmdl'; AllowPartial = $false },
        @{ Label = 'improved ragdoll player models'; Input = Join-Path $contentDir 'models\filmmaker\improved_ragdolls\*.vmdl'; AllowPartial = $false },
        @{ Label = 'particle systems'; Input = Join-Path $contentDir 'particles\filmmaker\povarehok\*.vpcf'; AllowPartial = $false }
    )
    if ($modernAvailable) {
        $compileGroups += @{ Label = 'modern particle systems'; Input = Join-Path $contentDir 'particles\filmmaker\modern\*.vpcf'; AllowPartial = $false }
    }
    foreach ($group in $compileGroups) {
        if (-not (Get-ChildItem (Split-Path -Parent $group.Input) -Recurse -File -ErrorAction SilentlyContinue)) {
            Write-Host "No converted $($group.Label) found; skipping." -ForegroundColor Yellow
            continue
        }
        Write-Host "Compiling converted Source 2 $($group.Label)..." -ForegroundColor Cyan
        & $resourceCompilerPath -i $group.Input -r -game $gameDir -nop4 -fshallow2
        if ($LASTEXITCODE -ne 0) {
            if ($group.AllowPartial) {
                Write-Warning "resourcecompiler.exe reported failures while compiling $($group.Label); continuing so runtime validation can check the actual particle closure."
            } else {
                throw "resourcecompiler.exe failed while compiling $($group.Label) with exit code $LASTEXITCODE"
            }
        }
    }

    $compiledCounts = @{
        Particles = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vpcf_c').Count
        Materials = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vmat_c').Count
        Textures = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vtex_c').Count
        Models = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vmdl_c').Count
    }
    $ragdollReport = Join-Path $contentDir 'improved-ragdolls-report.txt'
    $ragdollModelCount = if (Test-Path -LiteralPath $ragdollReport) {
        [int](Get-Content -LiteralPath $ragdollReport | Where-Object { $_ -like 'model_count=*' } | ForEach-Object { $_.Substring(12) })
    } else { 0 }
    if ($ragdollModelCount -le 0) {
        throw "Improved Ragdolls compile validation failed: conversion report contains no player models."
    }
    $reportedModels = @(Get-Content -LiteralPath $ragdollReport | Where-Object { $_ -like 'models/filmmaker/improved_ragdolls/*.vmdl' })
    if ($reportedModels.Count -ne $ragdollModelCount) {
        throw "Improved Ragdolls report mismatch: expected $ragdollModelCount paths, found $($reportedModels.Count)."
    }
    $missingCompiledModels = @($reportedModels | Where-Object {
        $compiledRelative = $_.Substring(0, $_.Length - 5) + '.vmdl_c'
        -not (Test-Path -LiteralPath (Join-Path $gameDir $compiledRelative))
    })
    if ($missingCompiledModels.Count -gt 0) {
        throw "Improved Ragdolls compile validation failed: $($missingCompiledModels.Count) reported player models are missing."
    }
    foreach ($required in @('Particles', 'Materials', 'Textures', 'Models')) {
        if ($compiledCounts[$required] -le 0) {
            throw "Compiled asset validation failed: no $required were produced under $gameDir"
        }
    }
    Write-Host ("Compiled assets: {0} particles, {1} materials, {2} textures, {3} models" -f `
        $compiledCounts.Particles, $compiledCounts.Materials, $compiledCounts.Textures, $compiledCounts.Models) -ForegroundColor Green

    Write-Host "Validating runtime Povarehok resource closure..." -ForegroundColor Cyan
    $validatorArgs = @('--particle-fx-cpp', $particleFxCpp, '--content-root', $contentDir,
        '--game-root', $gameDir, '--file-list', $runtimeRoots, '--report', $validationReport,
        '--validate-compiled')
    if ($modernAvailable) { $validatorArgs += '--require-modern' }
    & $Python $validator @validatorArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Povarehok runtime validation failed. See $validationReport"
    }
}

Write-Host ""
Write-Host "Converted content: $contentDir" -ForegroundColor Green
Write-Host "Compiled game dir: $gameDir" -ForegroundColor Green
Write-Host "Runtime resource namespaces: particles/filmmaker/povarehok/{regular,less/impacts,less/smoke}/..."
if ($modernAvailable) {
    Write-Host "                             particles/filmmaker/modern/{arc9_fas_muzzleflashes,arc9_fas_explosions,mw2019_tracer}/..."
}
