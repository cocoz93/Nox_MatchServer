# winctl.ps1 — 지금 쓰는 화면(입력 데스크톱)에 있는 창을 찍고 누른다.
#
# 왜 따로 만드나 — `deskhost` 는 **숨긴 데스크톱** 전용이고, `dinput.dll` 프록시는
# **DirectInput 전용**이다. 오픈녹스는 SDL2 라 둘 다 안 먹는다.
# SDL 은 보통 윈도 입력을 받으므로 `SetCursorPos` + `mouse_event` 로 조작된다.
#
#   .\tools\winctl.ps1 -Pid 1234 -Shot out.png
#   .\tools\winctl.ps1 -Pid 1234 -ClickX 500 -ClickY 300      (창 안쪽 좌표)
#   .\tools\winctl.ps1 -Pid 1234 -Key 0x1B                     (가상키 코드)
param(
  [Parameter(Mandatory=$true)][int]$Target,
  [string]$Shot = '',
  [int]$ClickX = -1,
  [int]$ClickY = -1,
  [int]$Key = -1,
  [switch]$Front
)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class WinCtl {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public const uint LDOWN = 0x0002, LUP = 0x0004, KEYUP = 0x0002;
}
'@

$p = Get-Process -Id $Target -ErrorAction Stop
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Host "[FAIL] 창이 없다"; exit 1 }

if ($Front) {
  [WinCtl]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
  [WinCtl]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 500
}

# 창 안쪽 좌표 → 화면 좌표
function ToScreen([int]$cx, [int]$cy) {
  $pt = New-Object WinCtl+POINT
  $pt.X = $cx; $pt.Y = $cy
  [WinCtl]::ClientToScreen($h, [ref]$pt) | Out-Null
  return @($pt.X, $pt.Y)
}

if ($ClickX -ge 0 -and $ClickY -ge 0) {
  $s = ToScreen $ClickX $ClickY
  [WinCtl]::SetCursorPos($s[0], $s[1]) | Out-Null
  Start-Sleep -Milliseconds 250
  [WinCtl]::mouse_event([WinCtl]::LDOWN, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 120
  [WinCtl]::mouse_event([WinCtl]::LUP, 0, 0, 0, [IntPtr]::Zero)
  Write-Host ("클릭 창({0},{1}) → 화면({2},{3})" -f $ClickX, $ClickY, $s[0], $s[1])
}

if ($Key -ge 0) {
  [WinCtl]::keybd_event([byte]$Key, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 100
  [WinCtl]::keybd_event([byte]$Key, 0, [WinCtl]::KEYUP, [IntPtr]::Zero)
  Write-Host ("키 0x{0:X2}" -f $Key)
}

if ($Shot -ne '') {
  $r = New-Object WinCtl+RECT
  [WinCtl]::GetClientRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  if ($w -le 0 -or $ht -le 0) { Write-Host "[FAIL] 창 크기 0"; exit 1 }
  # ⚠⚠ **PrintWindow 로는 SDL 창이 안 찍힌다** — 흰 화면이 나온다(2026-09-08 실측).
  #   SDL/OpenGL 은 창 DC 에 그리지 않는다. **화면에서 그 자리를 그대로 복사**한다.
  #   그래서 창이 **앞에 나와 있어야** 한다(-Front 를 같이 쓸 것).
  $o = ToScreen 0 0
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($o[0], $o[1], 0, 0, (New-Object System.Drawing.Size($w, $ht)))
  $g.Dispose()
  $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
  $n = ($bmp.Width * $bmp.Height)
  $bmp.Dispose()
  Write-Host ("찍음 {0}  ({1}x{2})" -f $Shot, $w, $ht)
}
