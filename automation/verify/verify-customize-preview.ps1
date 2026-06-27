#requires -Version 5
<#
Live verifier for the Customize Player modal's 3D preview + current-loadout display.

Opens the modal the SAME way the in-game button does (openCustomize only -- deliberately NO
previewModel/previewRebuild "pokes"), so it proves the 3D agent preview renders standalone, not just
when external automation re-pokes it. Produces:
  automation/output/customize_player/03_customize_modal_open.png
  automation/output/customize_player/04_customize_modal_agent_picker.png
  automation/output/customize_player/08_preview_after_agent_change.png
  automation/output/customize_player/09_preview_after_skin_change.png
  automation/output/customize_player/verify_preview_state.json
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = "F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive",
    [string]$Demo = "poop1",
    [int]$Tick = 26000,
    [string]$OutDir = "automation\output\customize_player",
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false }
    finally { $t.Dispose() }
}

if (-not $NoLaunch -or -not (Test-Netcon $Port)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 `
        -OutDir (Join-Path $outAbs 'verify_preview_launch')
}

function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('verify_preview_netcon_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Add-Content -LiteralPath (Join-Path $outAbs 'verify_preview.log') -Value $text
    return $text
}

function EditorEval([string]$Js) { return 'mirv_filmmaker editor eval ' + $Js }

function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path
    return $path
}

function Get-CustomizeState {
    $cmd = EditorEval '$.Msg($.CamEditor.customizeState()+String.fromCharCode(10))'
    $text = Invoke-Netcon -Commands @($cmd) -ReadSeconds 2.0
    $jsonLine = [regex]::Split($text, "\r?\n") | ForEach-Object {
        $idx = $_.IndexOf('{"visible":'); if ($idx -ge 0) { $_.Substring($idx).Trim() }
    } | Where-Object { $_ } | Select-Object -Last 1
    if (-not $jsonLine) { return $null }
    return $jsonLine | ConvertFrom-Json
}

Write-Host "=== Customize preview verifier (demo=$Demo tick=$Tick) ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 8.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @("demo_gototick $Tick", 'demo_pause') -ReadSeconds 4.0 | Out-Null
Start-Sleep -Seconds 1
Invoke-Netcon -Commands @('mirv_filmmaker editor on') -ReadSeconds 2.0 | Out-Null
Start-Sleep -Milliseconds 800

# Open the modal exactly like the button does -- no preview pokes.
Invoke-Netcon -Commands @((EditorEval '$.Msg($.CamEditor.openCustomize()+String.fromCharCode(10))')) -ReadSeconds 1.5 | Out-Null
Start-Sleep -Seconds 3   # let render()'s per-frame maintainPreview() composite the scene

$state = Get-CustomizeState
if (-not $state) { throw "Customize state unavailable after openCustomize." }
$state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outAbs 'verify_preview_state.json') -Encoding UTF8
Write-Host ("target={0} team={1} preview={2}" -f $state.name, $state.team, $state.preview) -ForegroundColor Green
Write-Host ("loadout: primary={0} secondary={1} knife={2} gloves={3}" -f `
    ($state.loadout.primary.defIndex), ($state.loadout.secondary.defIndex), `
    ($state.loadout.knife.defIndex), ($state.loadout.gloves.defIndex)) -ForegroundColor Green

Capture '03_customize_modal_open.png' | Write-Host

# Open the agent picker (screenshot the styled dropdown).
Invoke-Netcon -Commands @((EditorEval '$.CamEditor.customizeOpenPicker(String.fromCharCode(97,103,101,110,116))')) -ReadSeconds 1.0 | Out-Null
Start-Sleep -Milliseconds 600
Capture '04_customize_modal_agent_picker.png' | Write-Host

# Pick the first real agent option for this player's team and re-screenshot the preview.
$optsText = Invoke-Netcon -Commands @((EditorEval '$.Msg(JSON.stringify($.CamEditor.customizeOptions(String.fromCharCode(97,103,101,110,116)))+String.fromCharCode(10))')) -ReadSeconds 2.0
$optsLine = [regex]::Split($optsText, "\r?\n") | ForEach-Object { $i = $_.IndexOf('[['); if ($i -ge 0) { $_.Substring($i).Trim() } } | Where-Object { $_ } | Select-Object -Last 1
$agentVal = $null
if ($optsLine) {
    try { $opts = $optsLine | ConvertFrom-Json; foreach ($o in $opts) { if ($o[1] -and $o[1] -ne 'default') { $agentVal = [string]$o[1]; break } } } catch {}
}
if ($agentVal) {
    Write-Host "Picking agent: $agentVal" -ForegroundColor Cyan
    $agentChars = (($agentVal.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')
    Invoke-Netcon -Commands @((EditorEval ('$.CamEditor.customizePick(String.fromCharCode(97,103,101,110,116),String.fromCharCode(' + $agentChars + '))'))) -ReadSeconds 1.5 | Out-Null
    Start-Sleep -Seconds 2
    Capture '08_preview_after_agent_change.png' | Write-Host
} else {
    Write-Host "No agent option resolved; skipping agent-change shot." -ForegroundColor Yellow
}

# If the player has a weapon, pick a skin and screenshot.
$skinSlot = if ($state.loadout.primary.defIndex -gt 0) { 'primary' } elseif ($state.loadout.secondary.defIndex -gt 0) { 'secondary' } else { '' }
if ($skinSlot) {
    $slotChars = (($skinSlot.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')
    $skinOptsText = Invoke-Netcon -Commands @((EditorEval ('$.Msg(JSON.stringify($.CamEditor.customizeOptions(String.fromCharCode(' + $slotChars + ')))+String.fromCharCode(10))'))) -ReadSeconds 2.0
    $skinLine = [regex]::Split($skinOptsText, "\r?\n") | ForEach-Object { $i = $_.IndexOf('[['); if ($i -ge 0) { $_.Substring($i).Trim() } } | Where-Object { $_ } | Select-Object -Last 1
    $skinVal = $null
    if ($skinLine) { try { $so = $skinLine | ConvertFrom-Json; foreach ($o in $so) { if ($o[1] -and $o[1] -ne '0' -and $o[1] -notmatch ':0$') { $skinVal = [string]$o[1]; break } } } catch {} }
    if ($skinVal) {
        Write-Host "Picking $skinSlot skin: $skinVal" -ForegroundColor Cyan
        $skinChars = (($skinVal.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')
        Invoke-Netcon -Commands @((EditorEval ('$.CamEditor.customizePick(String.fromCharCode(' + $slotChars + '),String.fromCharCode(' + $skinChars + '))'))) -ReadSeconds 1.5 | Out-Null
        Start-Sleep -Seconds 2
        Capture '09_preview_after_skin_change.png' | Write-Host
    }
}

Write-Host "Artifacts written to $outAbs" -ForegroundColor Green




