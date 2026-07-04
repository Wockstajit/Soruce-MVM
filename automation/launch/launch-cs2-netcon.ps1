# Headlessly launches CS2 with the AfxHookSource2 hook injected and a TCP
# console (-netconport) open, using HLAE's CustomLoader CLI (-noGui).
# No HLAE GUI, no mouse/keyboard takeover.
#
#   powershell.exe -ExecutionPolicy Bypass -File automation\launch\launch-cs2-netcon.ps1 -Port 29010
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = "F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive",
    [int]$Width = 1600,
    [int]$Height = 1200,
    [int]$ReadyTimeoutSeconds = 75,
    [int]$SettleSeconds = 10,
    [switch]$NoWait,
    [string]$FxAssetsGameDir = '',
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$agentDebugLog = Join-Path $root 'debug-af2ef9.log'
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'launch-cs2'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$bin     = Join-Path $root 'build\staging-release\bin'
$hlae    = Join-Path $bin 'HLAE.exe'
$dll     = Join-Path $bin 'x64\AfxHookSource2.dll'
$cs2exe  = Join-Path $Cs2Dir 'game\bin\win64\cs2.exe'
if ([string]::IsNullOrWhiteSpace($FxAssetsGameDir)) {
    # Prefer the pack staged into the shipped build (build\staging-release\fx\...) so a
    # released/copied build works standalone; fall back to the converter's working output
    # (build\fx\...) for a dev machine that built the pack but hasn't staged it yet.
    $fxCandidates = @(
        (Join-Path $root 'build\staging-release\fx\source_mvm_fx'),
        (Join-Path $root 'build\fx\povarehok-source1import\source2\game\source_mvm_fx')
    )
    $FxAssetsGameDir = $fxCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $FxAssetsGameDir) { $FxAssetsGameDir = $fxCandidates[0] }
}
$fxAssetsAvailable = Test-Path -LiteralPath $FxAssetsGameDir

foreach ($f in @($hlae,$dll,$cs2exe)) { if (-not (Test-Path $f)) { Write-Error "Missing: $f"; exit 1 } }

# Close any running CS2 (only one Steam instance allowed; staging DLL re-injects fresh).
Get-Process -Name cs2 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
if (-not $steam) { $steam = 'C:\Program Files (x86)\Steam' }

# Mirror hlae/LaunchCs2.cs cmdline, plus the net console flags.
$cmdLine = "-steam -insecure -windowed -sw -w $Width -h $Height -console +sv_lan 1 -netconport $Port -afxFixNetCon"
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'launch-cs2' -Additional @{
    port = $Port
    width = $Width
    height = $Height
    windowed = $true
    commandLine = $cmdLine
    fxAssetsGameDir = if ($fxAssetsAvailable) { (Resolve-Path -LiteralPath $FxAssetsGameDir).Path } else { '' }
} | Out-Null

$argList = @(
    '-customLoader','-autoStart','-noGui',
    '-hookDllPath', $dll,
    '-programPath', $cs2exe,
    '-cmdLine', $cmdLine,
    '-addEnv', "SteamPath=$steam",
    '-addEnv', 'SteamClientLaunch=1',
    '-addEnv', 'SteamGameId=730',
    '-addEnv', 'SteamAppId=730',
    '-addEnv', 'SteamOverlayGameId=730',
    '-addEnv', "MVM_AGENT_DEBUG_LOG=$agentDebugLog"
)
if ($fxAssetsAvailable) {
    $argList += @('-addEnv', ('USRLOCALCSGO=' + (Resolve-Path -LiteralPath $FxAssetsGameDir).Path))
    Write-Host "Mounting converted FX assets via USRLOCALCSGO: $FxAssetsGameDir" -ForegroundColor Green
} else {
    Write-Host "Converted FX asset dir not found; Povarehok swaps will fail open: $FxAssetsGameDir" -ForegroundColor Yellow
}

Write-Host "Launching CS2 (hook + netconport $Port, ${Width}x${Height} windowed)..."
# Use ProcessStartInfo.ArgumentList so the multi-word -cmdLine stays a single argument.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $hlae
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $bin
# PowerShell 7 (.NET Core) exposes ProcessStartInfo.ArgumentList for robust quoting.
# Windows PowerShell 5.1 (.NET Framework) does not. Do not read $psi.ArgumentList
# directly under StrictMode: on 5.1 that throws before the fallback can run.
$argumentListProperty = [System.Diagnostics.ProcessStartInfo].GetProperty('ArgumentList')
if ($null -ne $argumentListProperty) {
    foreach ($a in $argList) { $psi.ArgumentList.Add([string]$a) }
} else {
    $quote = { param($s) if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }
    $psi.Arguments = (($argList | ForEach-Object { & $quote ([string]$_) }) -join ' ')
}
[void][System.Diagnostics.Process]::Start($psi)
if ($NoWait) {
    Write-Host "Launch started without readiness wait. Artifacts: $OutDir"
    exit 0
}

Write-Host "Waiting for CS2 window and netcon readiness..."
$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$ready = $false
while ((Get-Date) -lt $deadline) {
    $cs2 = Get-Process -Name cs2 -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 -and $_.Responding } |
        Select-Object -First 1
    if ($cs2) {
        try {
            $probe = [System.Net.Sockets.TcpClient]::new()
            $connect = $probe.BeginConnect('127.0.0.1', $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(500)) {
                $probe.EndConnect($connect)
                $ready = $true
            }
            $probe.Dispose()
        } catch {
            $ready = $false
        }
    }
    if ($ready) { break }
    Start-Sleep -Seconds 1
}
if (-not $ready) {
    throw "CS2 did not expose a responsive window and netcon port within $ReadyTimeoutSeconds seconds."
}

if ($SettleSeconds -gt 0) {
    Write-Host "CS2 is responsive; allowing $SettleSeconds seconds for the main menu/game loop to settle..."
    Start-Sleep -Seconds $SettleSeconds
}
Write-Host "CS2 automation-ready at ${Width}x${Height} windowed on netcon $Port. Artifacts: $OutDir"
