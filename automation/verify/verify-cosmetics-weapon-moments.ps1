#requires -Version 5
<#
Execute verification_run.json - apply legacy/modern paints, screenshots, flicker + crash.veh.

-Fast (default): one full skin/mesh test per unique weapon type (not per moment);
  single flicker check at end; fewer seeks; keeps CS2 session when chained after scan.

-Thorough: full test per moment + per-moment flicker + stress seeks.
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = "F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive",
    [string]$RunFile = "",
    [string]$OutDir = "",
    [int]$MaxMoments = 0,
    [int]$MaxWeaponsToTest = 6,
    [string]$WeaponCrop = "760,560,1500,1180",
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$NoLaunch,
    [switch]$PreviewOnly,
    [switch]$SkipFlicker
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\CosmeticsSpectate.ps1')
$dbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'

if (-not $Thorough -and -not $PSBoundParameters.ContainsKey('Fast')) { $Fast = $true }
if ($Fast -and $MaxMoments -le 0) { $MaxMoments = 20 }
if (-not $Fast -and $MaxMoments -le 0) { $MaxMoments = 12 }
if ($Fast -and -not $PSBoundParameters.ContainsKey('SkipFlicker')) { $SkipFlicker = $false }

$readDiag = if ($Fast) { 0.7 } else { 1.5 }
$readApply = if ($Fast) { 1.5 } else { 2.5 }
$sleepApply = if ($Fast) { 350 } else { 600 }

if ([string]::IsNullOrWhiteSpace($RunFile)) {
    $cand = Get-ChildItem (Join-Path $root 'automation\output\cosmetics_weapon_moments') -Recurse -Filter 'verification_run.json' -EA SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $cand) { throw "No verification_run.json - run scan first" }
    $RunFile = $cand.FullName
}
$runPath = Resolve-Path -LiteralPath $RunFile
$run = Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
$scanDir = if ($run.scanFile) { Split-Path -Parent $run.scanFile } else { Split-Path -Parent $runPath.Path }
$outAbs = if ([string]::IsNullOrWhiteSpace($OutDir)) { Join-Path $scanDir 'verify' } elseif ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'verify.log'
$proofFile = Join-Path $outAbs 'proof.json'
"=== verify $(Get-Date -Format o) fast=$Fast ===" | Set-Content -LiteralPath $logFile

$momentsJson = Join-Path $scanDir 'moments.json'
$demoPlay = if ($run.demo) { $run.demo } else { "replays/demo" }
if (Test-Path $momentsJson) {
    $mj = Get-Content $momentsJson -Raw | ConvertFrom-Json
    if ($mj.demo) { $demoPlay = $mj.demo }
}

function LastMatch([string]$t, [string]$p) {
    $m = [regex]::Matches($t, $p)
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function NC([string[]]$cmds, [double]$read = 1.0, [string]$tag = '') {
    if (-not (Wait-Cs2Netcon -Port $Port)) {
        $r = Export-CrashVehReport -Tag $tag -OutDir $outAbs -Port $Port
        if ($r.cs2ProcessAlive -and $r.crashVehCount -eq 0) {
            throw "netcon timeout ($tag) - CS2 still running (not a crash)"
        }
        throw "cs2/netcon down ($tag) cs2=$($r.cs2ProcessAlive) crash.veh=$($r.crashVehCount)"
    }
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -EA SilentlyContinue
    return $t
}

function Capture([string]$name) {
    $path = Join-Path $outAbs $name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    return $path
}

function Diff-Mean([string]$a, [string]$b) {
    $json = & python (Join-Path $automationRoot 'tools\image_diff.py') $a $b --crop $WeaponCrop
    return [double]((LastMatch $json '"mean":\s*([0-9.]+)'))
}

function Get-MvmTail([int]$after) {
    $log = Get-ChildItem $dbgDir -Filter 'mvm_debug_*.log' -EA SilentlyContinue | Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $log) { return @{ lines = @(); total = $after } }
    $all = Get-Content $log.FullName
    $slice = if ($after -lt $all.Count) { $all[$after..($all.Count - 1)] } else { @() }
    return @{ lines = $slice; total = $all.Count }
}

function Read-Diag {
    $t = NC @('mirv_filmmaker cosmetics visualdiag') $readDiag 'diag'
    $health = LastMatch $t 'spectateHealth=(\d+)'
    $isDead = ($t -match 'is dead \(health=') -or ($health -and [int]$health -le 0) -or ($t -match 'no spectated pawn')
    $paint = LastMatch $t 'paint\(def6\)=(\d+)'
    if (-not $paint -or $paint -eq 'absent') {
        $paint = LastMatch $t 'fallback: paint=(\d+)'
    }
    return @{
        defIndex = LastMatch $t 'defIndex=(\d+)'
        steam = LastMatch $t 'spectateSteam=(\d+)'
        paint = $paint
        health = $health
        raw = $t
        text = $t
        isDead = [bool]$isDead
    }
}

function Assert-Cs2Alive([string]$tag) {
    if (Wait-Cs2Netcon -Port $Port) { return }
    $r = Export-CrashVehReport -Tag $tag -OutDir $outAbs -Port $Port
    if ($r.cs2ProcessAlive -and $r.crashVehCount -eq 0) {
        throw "netcon timeout ($tag) - CS2 still running (not a crash)"
    }
    throw "cs2/netcon down ($tag) cs2=$($r.cs2ProcessAlive) crash.veh=$($r.crashVehCount)"
}

function Seek-Tick([int]$tick, [ref]$loaded) {
    if ($tick -eq $loaded.Value) { return }
    $seekRead = if ($Fast) { 3.0 } else { 6.0 }
    $delta = [Math]::Abs($tick - $loaded.Value)
    if ($delta -gt 20000) { $seekRead = if ($Fast) { 8.0 } else { 12.0 } }
    NC @("demo_gototick $tick", 'demo_pause') $seekRead "seek$tick" | Out-Null
    Start-Sleep -Milliseconds 200
    $loaded.Value = $tick
}

# Build test plan: Fast = one row per unique defIndex; Thorough = every moment
$sourceMoments = @($run.moments | Select-Object -First $MaxMoments)
if ($Fast) {
    $byDef = @{}
    foreach ($m in ($sourceMoments | Sort-Object { [int]$_.tick })) {
        $d = [string]$m.weaponDefIndex
        if (-not $byDef.ContainsKey($d)) { $byDef[$d] = $m }
        if ($byDef.Count -ge $MaxWeaponsToTest) { break }
    }
    $testPlan = @($byDef.Values | Sort-Object { if ([int]$_.weaponDefIndex -eq 4) { 1 } else { 0 } }, { [int]$_.tick })
    Write-Host "Fast verify: $($testPlan.Count) weapons (Glock/def4 last if present)" -ForegroundColor Cyan
} else {
    $testPlan = $sourceMoments
}

# Batch-load test paints (one Python call)
$defList = @($testPlan | ForEach-Object { [int]$_.weaponDefIndex } | Select-Object -Unique)
$paintsJson = & python (Join-Path $automationRoot 'tools\export_test_paints.py') @($defList | ForEach-Object { "$_" })
$paintsMap = $paintsJson | ConvertFrom-Json

$proof = [ordered]@{ runAt = (Get-Date -Format o); fast = [bool]$Fast; demo = $demoPlay; weapons = @(); crashVeh = 0 }

try {
    if (-not $NoLaunch -and -not (Wait-Cs2Netcon -Port $Port -Retries 3)) {
        Get-Process -Name cs2,hlae -EA SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 -OutDir (Join-Path $outAbs 'launch')
    }

    NC @('mvm_debug start') 1.2 'mvm' | Out-Null
    NC @("playdemo `"$demoPlay`"") 7.0 'playdemo' | Out-Null
    Start-Sleep -Seconds 1
    NC @('mirv_filmmaker follow stop', 'mirv_filmmaker follow clear', 'mirv_filmmaker cosmetics ticknudge off',
        'mirv_filmmaker cosmetics mesh auto', 'mirv_filmmaker cosmetics enabled 1') 1.2 'setup' | Out-Null

    $loadedTick = -1
    $flickerTick = 0
    $flickerDef = 0

    foreach ($m in $testPlan) {
        $mid = $m.id
        $tick = [int]$m.tick
        $def = [int]$m.weaponDefIndex
        $steam = "$($m.playerSteamId)"
        $key = "$def"
        Write-Host "`n[$mid] $($m.weaponName) tick=$tick" -ForegroundColor Yellow

        Seek-Tick $tick ([ref]$loadedTick)
        Assert-Cs2Alive "pre_lock_$mid"
        $spec = Invoke-CosmeticsSpectate -SteamId $steam -Netcon ${function:NC}
        if ($spec.dead) {
            $proof.weapons += [ordered]@{ id = $mid; def = $def; pass = $false; error = 'player dead at tick' }
            continue
        }
        if (-not $spec.ok) {
            $proof.weapons += [ordered]@{ id = $mid; def = $def; pass = $false; error = 'cosmetics spectate failed' }
            continue
        }
        Start-Sleep -Milliseconds 150
        if (Test-PlayerDeadFromDiag (Read-Diag)) {
            $proof.weapons += [ordered]@{ id = $mid; def = $def; pass = $false; error = 'player dead at tick' }
            continue
        }

        NC @('mirv_filmmaker follow stop', 'demo_pause') 0.8 'fp' | Out-Null
        Assert-Cs2Alive "pre_baseline_$mid"
        Capture "${mid}_baseline.png" | Out-Null

        if ($PreviewOnly) {
            $proof.weapons += [ordered]@{ id = $mid; def = $def; pass = $true; previewOnly = $true }
            continue
        }

        $wp = $paintsMap.$key
        if (-not $wp) {
            $proof.weapons += [ordered]@{ id = $mid; def = $def; pass = $true; skipped = 'no test paints' }
            continue
        }

        $legId = [int]$wp.legacyPaint.id
        $modId = [int]$wp.modernPaint.id
        $toggleId = [int]$wp.meshTogglePaint
        $flickerTick = $tick
        $flickerDef = $def

        NC @("mirv_filmmaker cosmetics player $steam weapon $def paint $legId wear 0.05 seed 0") $readApply "leg" | Out-Null
        Start-Sleep -Milliseconds $sleepApply
        Assert-Cs2Alive "after_legacy_$mid"
        $paintLeg = (Read-Diag).paint
        $shotLeg = Capture "${mid}_legacy.png"

        NC @("mirv_filmmaker cosmetics player $steam weapon $def paint $modId wear 0.05 seed 0") $readApply "mod" | Out-Null
        Start-Sleep -Milliseconds $sleepApply
        Assert-Cs2Alive "after_modern_$mid"
        $paintMod = (Read-Diag).paint
        $shotMod = Capture "${mid}_modern.png"
        $paintDiff = Diff-Mean $shotLeg $shotMod

        NC @('mirv_filmmaker cosmetics mesh modern', "mirv_filmmaker cosmetics player $steam weapon $def paint $toggleId wear 0.05 seed 0") 1.5 'mm' | Out-Null
        Start-Sleep -Milliseconds 300
        Assert-Cs2Alive "after_mesh_mod_$mid"
        $shotMm = Capture "${mid}_mesh_mod.png"
        NC @('mirv_filmmaker cosmetics mesh legacy') 1.0 'ml' | Out-Null
        Start-Sleep -Milliseconds 300
        Assert-Cs2Alive "after_mesh_leg_$mid"
        $shotMl = Capture "${mid}_mesh_leg.png"
        $meshDiff = Diff-Mean $shotMm $shotMl
        NC @('mirv_filmmaker cosmetics mesh auto') 0.6 'ma' | Out-Null

        $crashSnap = Export-CrashVehReport -Tag "weapon_$mid" -OutDir $outAbs -Port $Port

        $paintOk = ($paintLeg -eq "$legId") -and ($paintMod -eq "$modId")
        $pass = $paintOk -and (Test-Cs2Netcon $Port) -and ($crashSnap.crashVehCount -eq 0)
        $proof.weapons += [ordered]@{
            id = $mid; def = $def; weapon = $m.weaponName; tick = $tick; steam = $steam
            legacy = @{ want = $legId; got = $paintLeg }
            modern = @{ want = $modId; got = $paintMod; diff = $paintDiff }
            meshDiff = $meshDiff
            crashVeh = $crashSnap.crashVehCount
            paintOk = $paintOk; pass = $pass
        }
        Write-Host "  legacy=$paintLeg modern=$paintMod mesh=$([math]::Round($meshDiff,2)) crash.veh=$($crashSnap.crashVehCount) pass=$pass" -ForegroundColor $(if ($pass) { 'Green' } else { 'Red' })
        if ($crashSnap.crashVehCount -gt 0 -or -not (Test-Cs2Netcon $Port)) { break }
    }

    # One flicker check (not per weapon)
    if (-not $SkipFlicker -and $flickerTick -gt 0 -and $flickerDef -gt 0 -and (Test-Cs2Netcon $Port)) {
        $fKey = "$flickerDef"
        $fWp = $paintsMap.$fKey
        $fModId = if ($fWp) { [int]$fWp.modernPaint.id } else { 0 }
        if ($fModId -gt 0) {
        Write-Host "`nFlicker smoke (once)..." -ForegroundColor Cyan
        $mark = (Get-MvmTail 0).total
        Seek-Tick $flickerTick ([ref]$loadedTick)
        NC @("mirv_filmmaker cosmetics player $($testPlan[-1].playerSteamId) weapon $flickerDef paint $fModId wear 0.05 seed 0") $readApply 'fprep' | Out-Null
        $back = [Math]::Max(1, $flickerTick - 32)
        NC @("demo_gototick $back", 'demo_pause') 2.0 'fback' | Out-Null
        $loadedTick = $back
        NC @('demo_resume') 0.4 'fres' | Out-Null
        Start-Sleep -Seconds $(if ($Fast) { 1 } else { 2 })
        NC @('demo_pause') 0.6 'fpau' | Out-Null
        $tail = Get-MvmTail $mark
        $nudge = @($tail.lines | Where-Object { $_ -match 'cosmetics\.nudge.*START' }).Count
        $proof.flicker = @{ nudgeStarts = $nudge; pass = ($nudge -eq 0) }
        Write-Host "  nudgeStarts=$nudge" -ForegroundColor $(if ($nudge -eq 0) { 'Green' } else { 'Red' })
        }
    }

    if (-not $Fast) {
        foreach ($st in @(4000, 16000, 32000)) {
            if (-not (Wait-Cs2Netcon -Port $Port -Retries 3)) { break }
            NC @("demo_gototick $st", 'demo_pause') 2.5 "stress$st" | Out-Null
            NC @('mirv_filmmaker cosmetics skinlog') 1.0 "log$st" | Out-Null
        }
    }

    $finalCrash = Export-CrashVehReport -Tag 'verify_end' -OutDir $outAbs -Port $Port
    $proof.crashVeh = $finalCrash.crashVehCount
    $proof.crashVehLines = @($finalCrash.crashVehLines)
    $proof.cs2Alive = Test-Cs2Netcon $Port
    $failed = @($proof.weapons | Where-Object { -not $_.pass })
    $flickOk = (-not $proof.flicker) -or $proof.flicker.pass
    $proof.overall_pass = ($failed.Count -eq 0) -and $proof.cs2Alive -and ($proof.crashVeh -eq 0) -and $flickOk -and ($proof.weapons.Count -gt 0)

    $proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    & python (Join-Path $automationRoot 'tools\make_moment_preview.py') $outAbs --pattern '*_baseline.png' 2>$null

    if (-not $proof.overall_pass) { exit 1 }
    Write-Host "`nPASS $proofFile" -ForegroundColor Green
    exit 0
}
catch {
    $proof.error = $_.Exception.Message
    try { Export-CrashVehReport -Tag 'verify_catch' -OutDir $outAbs -Port $Port | Out-Null } catch {}
    $proof | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
