# Sends console commands to a running CS2 over its -netconport TCP console and
# prints the streamed console output. Pure backend - no window focus needed.
#
#   pwsh automation\netcon\cs2-netcon.ps1 -Port 29010 -Commands "mirv_filmmaker ui_status","mirv_filmmaker list"
param(
    [int]$Port = 29010,
    [string[]]$Commands = @('mirv_filmmaker ui_status'),
    [double]$ReadSeconds = 1.5,   # how long to read output after each command
    [string]$LogPath
)
$ErrorActionPreference = 'Stop'

$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port)
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII
$sb = New-Object System.Text.StringBuilder

function Read-Available([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $buf = New-Object byte[] 8192
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                $text = $enc.GetString($buf, 0, $n)
                [void]$sb.Append($text)
                Write-Host -NoNewline $text
            }
        } else {
            Start-Sleep -Milliseconds 50
        }
    }
}

# Drain any banner/backlog first.
Read-Available 0.6
foreach ($cmd in $Commands) {
    Write-Host "`n>>> $cmd" -ForegroundColor Cyan
    $bytes = $enc.GetBytes($cmd + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Read-Available $ReadSeconds
}
$client.Close()

if ($LogPath) { [System.IO.File]::WriteAllText($LogPath, $sb.ToString()) }
