# Launches CS2 with the hook + FX pack, then starts mvm_test offline bot practice.
# Press Insert in-game to open the FX test menu.
#
#   powershell.exe -ExecutionPolicy Bypass -File automation\launch\launch-cs2-test.ps1
#   powershell.exe -ExecutionPolicy Bypass -File automation\launch\launch-cs2-test.ps1 -Map de_mirage
param(
    [int]$Port = 29010,
    [string]$Map = 'de_dust2',
    [string]$Cs2Dir = 'F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive',
    [int]$Width = 1600,
    [int]$Height = 1200,
    [int]$ReadyTimeoutSeconds = 90,
    [int]$SettleSeconds = 12,
    [string]$FxAssetsGameDir = '',
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'launch-cs2-test'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'launch-cs2-netcon.ps1') `
    -Port $Port -Cs2Dir $Cs2Dir -Width $Width -Height $Height `
    -ReadyTimeoutSeconds $ReadyTimeoutSeconds -SettleSeconds $SettleSeconds `
    -FxAssetsGameDir $FxAssetsGameDir -OutDir $OutDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

function Send-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect('127.0.0.1', $Port)
    } catch {
        throw "Could not connect to netcon 127.0.0.1:$Port"
    }
    $client.NoDelay = $true
    $stream = $client.GetStream()
    $encoding = [Text.Encoding]::ASCII
    foreach ($cmd in $Commands) {
        $bytes = $encoding.GetBytes($cmd + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 120
    }
    $deadline = (Get-Date).AddSeconds($ReadSeconds)
    $buffer = New-Object byte[] 16384
    $builder = [Text.StringBuilder]::new()
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -gt 0) { [void]$builder.Append($encoding.GetString($buffer, 0, $read)) }
        } else {
            Start-Sleep -Milliseconds 25
        }
    }
    $client.Close()
    return $builder.ToString()
}

Write-Host "Starting mvm_test on map '$Map'..." -ForegroundColor Cyan
$out = Send-Netcon @("mvm_test start $Map") 3.5
if ($out) { Write-Host $out }
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'launch-cs2-test' -Additional @{
    port = $Port
    map = $Map
    width = $Width
    height = $Height
} | Out-Null

Write-Host ''
Write-Host 'mvm_test ready. In-game: press INSERT to open the FX test menu.' -ForegroundColor Green
Write-Host "Console: mvm_test menu toggle | mvm_test status | mvm_test stop" -ForegroundColor DarkGray
Write-Host "Artifacts: $OutDir"
