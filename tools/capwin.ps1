# capwin.ps1 - capture a window by process id, without bringing it to front.
# Tries PrintWindow with PW_RENDERFULLCONTENT (0x2) first; falls back to plain PrintWindow.
param(
  [Parameter(Mandatory=$true)][int]$Pid_,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$Flags = 2
)
Add-Type -AssemblyName System.Drawing
$sig = @'
using System;
using System.Runtime.InteropServices;
public class Win32Cap {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
if (-not ("Win32Cap" -as [type])) { Add-Type -TypeDefinition $sig }

$p = Get-Process -Id $Pid_ -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "FAIL no-process"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq 0) { Write-Output "FAIL no-window"; exit 1 }

$r = New-Object Win32Cap+RECT
[void][Win32Cap]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Output "FAIL bad-rect"; exit 1 }

$bmp = New-Object Drawing.Bitmap($w, $ht)
$g = [Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Win32Cap]::PrintWindow($h, $hdc, [uint32]$Flags)
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)

# how many distinct colors? an all-black capture means PrintWindow did not work
$colors = @{}
for ($y = 0; $y -lt $ht; $y += 16) {
  for ($x = 0; $x -lt $w; $x += 16) {
    $colors[$bmp.GetPixel($x, $y).ToArgb()] = 1
  }
}
$bmp.Dispose()
Write-Output ("OK size={0}x{1} printwindow={2} colors={3} file={4}" -f $w, $ht, $ok, $colors.Count, $Out)
