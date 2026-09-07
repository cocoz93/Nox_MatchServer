# gamenav.ps1 — 게임이 그리는 커서를 **화면에서 찾아** 목표로 몰고 간 뒤 누른다.
#
# 왜 nav.ps1 로 안 되나 — nav.ps1 은 **OS 커서**를 되짚는다(GetCursorPos).
# 그런데 녹스는 OS 커서를 쓰지 않는다. raw 마우스 델타를 자기 감도로 곱해
# **자기 커서를 따로** 굴린다. 그래서 OS 커서를 정확히 맞춰도 게임 커서는 딴 데 있다.
#
# 실측 (2026-09-07, 마우스 가속 0,0,0 인 상태) —
#   nav.ps1 로 클라이언트 (592,335) 를 겨눴는데 게임 커서는 (약 1197,684) 에 있었다.
#   가로 2.02배 · 세로 2.04배. 그래서 메뉴 클릭이 통째로 헛돌았다.
#
# 어떻게 찾나 — 게임 커서 말고는 메뉴 화면이 정지해 있다. 그러니
#   ① 커서를 좌상단 구석으로 몰아 **깨끗한 기준 사진**을 찍고
#   ② 옮긴 뒤 사진을 찍어 기준과 **다른 화소**를 모으면 그게 커서다.
# 좌상단 구석(HomeBox)은 기준 사진에도 커서가 있던 자리라 계산에서 뺀다.
#
# 쓰는 법
#   .\tools\gamenav.ps1 -Pid_ <PID> -Tx 592 -Ty 335 -Click
#   .\tools\gamenav.ps1 -Pid_ <PID> -Tx 592 -Ty 335 -Click -Right -Hold 800
# Tx,Ty 는 **클라이언트 픽셀**이다 (창 테두리·제목줄을 뺀 게임 그림 안 좌표).
param(
  [Parameter(Mandatory)][int]$Pid_,
  [Parameter(Mandatory)][int]$Tx,
  [Parameter(Mandatory)][int]$Ty,
  [switch]$Click, [switch]$Right, [int]$Hold = 0,
  [string]$Out = '',
  [int]$Tries = 6,          # 되짚기 횟수
  [int]$Tol = 6,            # 이만큼 안에 들면 됐다고 본다 (픽셀)
  [double]$Scale = 0        # 0 이면 그 자리에서 잰다. 알면 넣어서 한 걸음 아낀다
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class GDrv {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static void ForceForeground(IntPtr h) {
    ShowWindow(h, 9);
    keybd_event(0x12, 0, 0, IntPtr.Zero);
    keybd_event(0x12, 0, 2, IntPtr.Zero);
    BringWindowToTop(h);
    SetForegroundWindow(h);
  }
  // 창을 통째로 떠서 32bpp 바이트 배열로 돌려준다. 창 픽셀 기준이다.
  public static byte[] Grab(IntPtr h, out int w, out int ht, out int stride) {
    RECT r; GetWindowRect(h, out r);
    w = r.Right - r.Left; ht = r.Bottom - r.Top;
    using (Bitmap b = new Bitmap(w, ht)) {
      using (Graphics g = Graphics.FromImage(b)) {
        IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 2); g.ReleaseHdc(hdc);
      }
      var rect = new Rectangle(0, 0, w, ht);
      var bd = b.LockBits(rect, System.Drawing.Imaging.ImageLockMode.ReadOnly,
                          System.Drawing.Imaging.PixelFormat.Format32bppArgb);
      stride = bd.Stride;
      byte[] buf = new byte[stride * ht];
      Marshal.Copy(bd.Scan0, buf, 0, buf.Length);
      b.UnlockBits(bd);
      return buf;
    }
  }
  public static void Save(IntPtr h, string path) {
    RECT r; GetWindowRect(h, out r);
    int w = r.Right - r.Left, ht = r.Bottom - r.Top;
    using (Bitmap b = new Bitmap(w, ht)) using (Graphics g = Graphics.FromImage(b)) {
      IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 2); g.ReleaseHdc(hdc);
      b.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
  }
}
'@ -ReferencedAssemblies System.Drawing

$h = (Get-Process -Id $Pid_ -ErrorAction Stop).MainWindowHandle
if ($h -eq [IntPtr]::Zero) { "ABORT: 창 없음"; exit 1 }

# 포그라운드로 올린다 — 뒤에 있으면 입력을 안 받는다
$fp = 0
for ($t = 1; $t -le 8; $t++) {
  [GDrv]::ForceForeground($h); Start-Sleep -Milliseconds 250
  [void][GDrv]::GetWindowThreadProcessId([GDrv]::GetForegroundWindow(), [ref]$fp)
  if ($fp -eq $Pid_) { break }
}
if ($fp -ne $Pid_) { "ABORT: focus=$fp"; exit 1 }
[void][GDrv]::SetWindowPos($h, [IntPtr](-1), 200, 15, 0, 0, 0x0001)
Start-Sleep -Milliseconds 300

# 창 픽셀 -> 클라이언트 픽셀로 옮기는 값 (테두리·제목줄)
$wr = New-Object GDrv+RECT; [void][GDrv]::GetWindowRect($h, [ref]$wr)
$cp = New-Object GDrv+POINT; [void][GDrv]::ClientToScreen($h, [ref]$cp)
$offX = $cp.X - $wr.Left; $offY = $cp.Y - $wr.Top
$cr = New-Object GDrv+RECT; [void][GDrv]::GetClientRect($h, [ref]$cr)
$cw = $cr.Right - $cr.Left; $ch = $cr.Bottom - $cr.Top

function Move-Raw([int]$dx, [int]$dy) {
  # 잘게 나눠 보낸다 — 한 번에 크게 보내면 게임이 흘린다
  $n = [Math]::Max([Math]::Ceiling([Math]::Abs($dx) / 40.0), [Math]::Ceiling([Math]::Abs($dy) / 40.0))
  if ($n -lt 1) { $n = 1 }
  $ax = 0; $ay = 0
  for ($i = 1; $i -le $n; $i++) {
    $tx = [int]([Math]::Round($dx * $i / $n)); $ty = [int]([Math]::Round($dy * $i / $n))
    [GDrv]::mouse_event(0x0001, ($tx - $ax), ($ty - $ay), 0, [IntPtr]::Zero)
    $ax = $tx; $ay = $ty
    Start-Sleep -Milliseconds 10
  }
  Start-Sleep -Milliseconds 180
}
function Home-Cursor {
  for ($i = 0; $i -lt 6; $i++) { [GDrv]::mouse_event(0x0001, -4000, -4000, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60 }
  Start-Sleep -Milliseconds 250
}

$w = 0; $ht = 0; $st = 0
Home-Cursor
$base = [GDrv]::Grab($h, [ref]$w, [ref]$ht, [ref]$st)

# 커서를 좌상단으로 몰았을 때 그것이 차지하는 자리. 차분에서 뺀다.
$HomeBox = 64

# Find-Cursor — 기준 사진과 다른 화소를 모아 **가장 큰 덩어리의 좌상단**을 돌려준다.
#   커서 그림은 화살촉이 좌상단이라 그 모서리가 실제로 눌리는 자리다.
function Find-Cursor {
  $cur = [GDrv]::Grab($h, [ref]$w, [ref]$ht, [ref]$st)
  $minX = [int]::MaxValue; $minY = [int]::MaxValue; $n = 0
  $sumX = 0.0; $sumY = 0.0
  for ($y = $offY; $y -lt $ht; $y++) {
    $row = $y * $st
    for ($x = $offX; $x -lt $w; $x++) {
      $lx = $x - $offX; $ly = $y - $offY
      if ($lx -lt $HomeBox -and $ly -lt $HomeBox) { continue }   # 집(좌상단)은 뺀다
      $o = $row + $x * 4
      $d = [Math]::Abs([int]$cur[$o] - [int]$base[$o]) +
           [Math]::Abs([int]$cur[$o+1] - [int]$base[$o+1]) +
           [Math]::Abs([int]$cur[$o+2] - [int]$base[$o+2])
      if ($d -gt 60) {
        $n++; $sumX += $lx; $sumY += $ly
        if ($lx -lt $minX) { $minX = $lx }
        if ($ly -lt $minY) { $minY = $ly }
      }
    }
  }
  if ($n -lt 12) { return $null }
  return [pscustomobject]@{ N = $n; X = $minX; Y = $minY; Cx = [int]($sumX / $n); Cy = [int]($sumY / $n) }
}

# ── 배율 재기 ── 알려주지 않았으면 한 번 움직여 잰다
$probe = 120
if ($Scale -le 0) {
  Move-Raw $probe $probe
  $p1 = Find-Cursor
  if (-not $p1) { "ABORT: 커서를 못 찾았다 (배율 재기 실패)"; exit 1 }
  $Scale = (($p1.X / [double]$probe) + ($p1.Y / [double]$probe)) / 2.0
  "배율 = {0:N3} px/raw  (raw {1} -> 커서 {2},{3})" -f $Scale, $probe, $p1.X, $p1.Y
} else {
  Move-Raw $probe $probe
}

# ── 목표로 몰기 ──
$ok = $false
for ($k = 0; $k -lt $Tries; $k++) {
  $p = Find-Cursor
  if (-not $p) { "  커서 실종 — 집으로 되돌린다"; Home-Cursor; Move-Raw 60 60; continue }
  $ex = $Tx - $p.X; $ey = $Ty - $p.Y
  if ([Math]::Abs($ex) -le $Tol -and [Math]::Abs($ey) -le $Tol) {
    "커서 도착 ($($p.X),$($p.Y)) 목표 ($Tx,$Ty)"; $ok = $true; break
  }
  Move-Raw ([int]([Math]::Round($ex / $Scale))) ([int]([Math]::Round($ey / $Scale)))
}
if (-not $ok) {
  $p = Find-Cursor
  if ($p) { "커서 근사 ($($p.X),$($p.Y)) 목표 ($Tx,$Ty) — 오차 $([Math]::Abs($Tx-$p.X)),$([Math]::Abs($Ty-$p.Y))" }
  else { "ABORT: 커서를 못 찾았다"; exit 1 }
}

if ($Click) {
  $down = if ($Hold -gt 0) { $Hold } else { 90 }
  if ($Right) { $dn = 0x0008; $up = 0x0010 } else { $dn = 0x0002; $up = 0x0004 }
  [GDrv]::mouse_event($dn, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds $down
  [GDrv]::mouse_event($up, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 400
}
if ($Out) { [GDrv]::Save($h, $Out); "shot ok -> $Out" }
"scale=$Scale"
