# Captures ONLY the CS2 game window to PNG (not the whole desktop).
#
# Why: a full-monitor grab makes the game a small slice of a big image, so fine
# UI detail (tile colors, navbar icons, text) is hard to read. Cropping to the
# window -- by default the client area, i.e. the game's render surface with no
# title bar / borders / desktop -- yields a tight, full-resolution image.
#
#   powershell -ExecutionPolicy Bypass -File automation\capture\capture-game-window.ps1 -Out shot.png
#   powershell -ExecutionPolicy Bypass -File automation\capture\capture-game-window.ps1 -Out shot.png -Mode window
#   powershell -ExecutionPolicy Bypass -File automation\capture\capture-game-window.ps1 -Out shot.png -Process cs2
#
# -Mode client (default) : just the game viewport (recommended for reading UI).
# -Mode window           : the whole OS window including title bar + borders.
param(
    [Parameter(Mandatory = $true)]
    [string]$Out,
    [string]$Process = 'cs2',
    [ValidateSet('client', 'window')]
    [string]$Mode = 'client'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win32WindowCapture {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
  // DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS=9) gives the true visible
  // frame (GetWindowRect includes invisible drop-shadow margins on Win10/11).
  [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr hWnd, int attr, out RECT r, int size);

  public static void EnablePerMonitorV2() { SetThreadDpiAwarenessContext(new IntPtr(-4)); }

  public static RECT ExtendedFrame(IntPtr hWnd) {
    RECT r;
    if (DwmGetWindowAttribute(hWnd, 9, out r, Marshal.SizeOf(typeof(RECT))) == 0) return r;
    GetWindowRect(hWnd, out r); return r;
  }
}
'@

[Win32WindowCapture]::EnablePerMonitorV2()

# Locate the target window (first process of -Process with a real top-level window).
$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No '$Process' process with a visible window. Is the game running?" }
$h = $proc.MainWindowHandle

# Un-minimize if needed (a minimized window has no capturable pixels). SW_RESTORE = 9.
if ([Win32WindowCapture]::IsIconic($h)) { [void][Win32WindowCapture]::ShowWindow($h, 9); Start-Sleep -Milliseconds 250 }

$x = 0; $y = 0; $w = 0; $hgt = 0
if ($Mode -eq 'client') {
    $rc = New-Object Win32WindowCapture+RECT
    [void][Win32WindowCapture]::GetClientRect($h, [ref]$rc)
    $tl = New-Object Win32WindowCapture+POINT; $tl.X = 0; $tl.Y = 0
    [void][Win32WindowCapture]::ClientToScreen($h, [ref]$tl)
    $x = $tl.X; $y = $tl.Y; $w = $rc.Right - $rc.Left; $hgt = $rc.Bottom - $rc.Top
} else {
    $rc = [Win32WindowCapture]::ExtendedFrame($h)
    $x = $rc.Left; $y = $rc.Top; $w = $rc.Right - $rc.Left; $hgt = $rc.Bottom - $rc.Top
}
if ($w -le 0 -or $hgt -le 0) { throw "Window '$Process' has invalid $Mode bounds (${w}x${hgt}); it may be minimized or hidden." }

$parent = Split-Path -Parent $Out
if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

$bitmap = [System.Drawing.Bitmap]::new($w, $hgt, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $hgt)), [System.Drawing.CopyPixelOperation]::SourceCopy)
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $graphics.Dispose(); $bitmap.Dispose()
}

$file = Get-Item -LiteralPath $Out
$kb = [math]::Round($file.Length / 1kb)
Write-Host "Captured $Process $Mode (${w}x${hgt}, $kb KB) -> $($file.FullName)"
