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
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public const uint LDOWN = 0x0002, LUP = 0x0004, KEYUP = 0x0002;
  // 절대 좌표 이동 — 0..65535 로 정규화해 보낸다.
  // ⚠ VIRTUALDESK 를 같이 세우지 않으면 **주 모니터 기준**이라 둘째 모니터가 안 닿는다.
  public const uint MOVE = 0x0001, ABSOLUTE = 0x8000, VIRTUALDESK = 0x4000;
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
  # ⚠⚠ **`SetCursorPos` 만으로는 녹스 커서가 안 따라온다.** 메뉴가 뜬 뒤에는 게임이
  #   커서를 잡고 있어 그 호출을 무시한다. 지문 = **직전 단계의 커서 자리에 그대로
  #   남아 있고 클릭이 헛돈다**(2026-09-08: 메인 메뉴에서 Multiplay 를 눌렀는데
  #   커서가 Play Intro 위에 남아 그 항목이 강조돼 있었다. 두 걸음으로 밀어도 같았다).
  #   답 = **입력 이벤트로 옮긴다**. `mouse_event` 의 절대 이동은 시스템이 진짜
  #   마우스 움직임으로 만들어 주므로 SDL 이 읽는 경로를 그대로 탄다.
  # ⚠⚠ **모니터가 둘이면 주 모니터 기준으로 재면 안 된다.** SM_CXSCREEN(1920)으로
  #   정규화했더니 커서가 화면 왼쪽 위로 튀었다(2026-09-08). 가상 화면 전체
  #   (여기선 3840x1080)를 기준으로 재고 VIRTUALDESK 를 같이 세운다.
  $vx = [WinCtl]::GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
  $vy = [WinCtl]::GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
  $vw = [WinCtl]::GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
  $vh = [WinCtl]::GetSystemMetrics(79)   # SM_CYVIRTUALSCREEN
  $flags = [WinCtl]::MOVE -bor [WinCtl]::ABSOLUTE -bor [WinCtl]::VIRTUALDESK
  # ★ **한 번에 건너뛰지 말고 여러 걸음으로 간다.** 큰 점프 하나는 게임이 통째로
  #   건너뛴다 — 두 걸음으로도 안 됐다(2026-09-08: 메인 메뉴에서 Multiplay 를 눌렀는데
  #   커서가 직전 자리인 Play Intro 위에 남아 그 항목이 강조된 채였다).
  #   실제 마우스처럼 잘게 움직이면 매 걸음이 움직임 이벤트가 되어 확실히 따라온다.
  $cur = New-Object WinCtl+POINT
  [WinCtl]::GetCursorPos([ref]$cur) | Out-Null
  $steps = 10
  foreach ($i in 1..$steps) {
    $px = [int]($cur.X + ($s[0] - $cur.X) * $i / $steps)
    $py = [int]($cur.Y + ($s[1] - $cur.Y) * $i / $steps)
    $nx = [int]([math]::Round(($px - $vx) * 65535.0 / ($vw - 1)))
    $ny = [int]([math]::Round(($py - $vy) * 65535.0 / ($vh - 1)))
    [WinCtl]::mouse_event($flags, $nx, $ny, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 30
  }
  Start-Sleep -Milliseconds 300
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
