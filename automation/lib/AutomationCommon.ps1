Set-StrictMode -Version Latest

function Get-AutomationRoot {
    return (Split-Path -Parent $PSScriptRoot)
}

function Get-AutomationRepositoryRoot {
    return (Split-Path -Parent (Get-AutomationRoot))
}

function New-AutomationRunFolder {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[A-Za-z0-9._-]+$')]
        [string]$Name,
        [string]$BaseDirectory
    )

    if ([string]::IsNullOrWhiteSpace($BaseDirectory)) {
        $BaseDirectory = Join-Path (Get-AutomationRoot) 'runs'
    }

    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $path = Join-Path (Join-Path $BaseDirectory $Name) $timestamp
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return (Resolve-Path -LiteralPath $path).Path
}

function Save-AutomationRunMetadata {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunDirectory,
        [Parameter(Mandatory = $true)]
        [string]$AutomationName,
        [hashtable]$Additional = @{}
    )

    $metadata = [ordered]@{
        automation = $AutomationName
        startedAt = (Get-Date).ToString('o')
        machine = $env:COMPUTERNAME
        user = $env:USERNAME
        primaryMonitor = $null
    }

    Add-Type -AssemblyName System.Windows.Forms
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $metadata.primaryMonitor = [ordered]@{
        x = $bounds.X
        y = $bounds.Y
        width = $bounds.Width
        height = $bounds.Height
    }

    foreach ($key in $Additional.Keys) {
        $metadata[$key] = $Additional[$key]
    }

    $path = Join-Path $RunDirectory 'run.json'
    $metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}
