# 게임 창을 잡고 커서를 목표로 옮긴 뒤(선택적으로) 클릭하고 화면을 찍는다.
# Cx,Cy 는 게임 화면 1280x960 기준 좌표. Gain 은 실측 계수(화면좌표 = 보낸값 x Gain).
param([int]$Pid_, [int]$Cx, [int]$Cy, [string]$Out, [switch]$Click, [double]$Gain = 3.06, [switch]$NoActivate, [switch]$NoHome, [int]$Hold = 0, [switch]$Right)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class Drv {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
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
  // ALT 를 살짝 눌러 이 스레드를 "마지막 입력 주체"로 만든 뒤 포그라운드를 가져온다.
  // 윈도우의 포커스 탈취 방지를 통과하는 표준 우회.
  public static void ForceForeground(IntPtr h) {
    ShowWindow(h, 9);
    keybd_event(0x12, 0, 0, IntPtr.Zero);
    keybd_event(0x12, 0, 2, IntPtr.Zero);
    BringWindowToTop(h);
    SetForegroundWindow(h);
  }
  public static string Shot(IntPtr h, string path) {
    RECT r; GetWindowRect(h, out r);
    int w = r.Right - r.Left, ht = r.Bottom - r.Top;
    using (Bitmap b = new Bitmap(w, ht)) using (Graphics g = Graphics.FromImage(b)) {
      IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 2); g.ReleaseHdc(hdc);
      b.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
    int fp; GetWindowThreadProcessId(GetForegroundWindow(), out fp);
    return "shot ok, foreground pid=" + fp;
  }
}
'@ -ReferencedAssemblies System.Drawing
$h = (Get-Process -Id $Pid_ -ErrorAction Stop).MainWindowHandle
if ($h -eq [IntPtr]::Zero) { "ABORT: 창 없음"; exit 1 }
$ws = New-Object -ComObject WScript.Shell
$fp = 0
for ($t = 1; $t -le 10; $t++) {
  [Drv]::ForceForeground($h)
  Start-Sleep -Milliseconds 250
  [void][Drv]::GetWindowThreadProcessId([Drv]::GetForegroundWindow(), [ref]$fp)
  if ($fp -eq $Pid_) { break }
  [void]$ws.AppActivate($Pid_)
  Start-Sleep -Milliseconds 250
  [void][Drv]::GetWindowThreadProcessId([Drv]::GetForegroundWindow(), [ref]$fp)
  if ($fp -eq $Pid_) { break }
}
if ($fp -ne $Pid_) { "ABORT: focus=$fp"; exit 1 }
[void][Drv]::SetWindowPos($h, [IntPtr](-1), 200, 15, 0, 0, 0x0001)
Start-Sleep -Milliseconds 300
$pt = New-Object Drv+POINT
[void][Drv]::ClientToScreen($h, [ref]$pt)
$cr = New-Object Drv+RECT
[void][Drv]::GetClientRect($h, [ref]$cr)
$cw = $cr.Right - $cr.Left; $ch = $cr.Bottom - $cr.Top
# 1) 창 안 빈 곳을 실제로 눌러 게임이 마우스를 다시 잡게 한다 (이걸 빼면 이후 입력이 전부 무시됨)
#    ⚠ 이 클릭은 팝업을 닫는다. 팝업 위를 눌러야 할 때는 -NoActivate 로 건너뛴다
#      (앞선 호출로 이미 마우스를 잡고 있을 때만 안전하다)
if (-not $NoActivate) {
  [void][Drv]::SetCursorPos(($pt.X + 100), ($pt.Y + 900))
  Start-Sleep -Milliseconds 250
  [Drv]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 90
  [Drv]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 900
}
# 2) 커서를 좌상단에 붙여 기준점을 만든다
#    ⚠ 마우스가 팝업 밖으로 나가면 팝업이 닫힌다. 그럴 때는 -NoHome 으로 건너뛰고
#      Cx,Cy 를 "지금 자리에서의 이동량"으로 준다
if (-not $NoHome) {
  for ($i = 0; $i -lt 5; $i++) { [Drv]::mouse_event(0x0001, -3000, -3000, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 100 }
  Start-Sleep -Milliseconds 300
}
# 3) 목표까지 잘게 나눠 이동
#    ★ 2026-09-07 고침 — 예전에는 좌상단(0,0)에서 Cx,Cy 만큼만 옮겼다.
#      그런데 바로 위에서 창을 (200,15)로 옮겨 두므로 실제로 눌린 자리는
#      클라이언트 좌표 (Cx-208, Cy-46) 였다. **늘 왼쪽 위로 208x46 px 밀렸다.**
#      화면 한가운데(640,480)를 달라고 해도 (432,434)를 눌렀으니, 캐릭터에서 커서로
#      가는 방향이 통째로 틀어졌다 — walkto 가 두 자리를 오가며 진동한 원인이다.
#      이제 창 원점($pt)과 실제 클라이언트 크기($cw,$ch)를 반영한다.
$sx = [int]([Math]::Round($pt.X + $Cx * $cw / 1280.0))
$sy = [int]([Math]::Round($pt.Y + $Cy * $ch / 960.0))
$rx = [int]([Math]::Round($sx / $Gain)); $ry = [int]([Math]::Round($sy / $Gain))
$n = [Math]::Max([Math]::Ceiling($rx / 4.0), [Math]::Ceiling($ry / 4.0)); if ($n -lt 1) { $n = 1 }
$ax = 0; $ay = 0
for ($i = 1; $i -le $n; $i++) {
  $tx = [int]([Math]::Round($rx * $i / $n)); $ty = [int]([Math]::Round($ry * $i / $n))
  [Drv]::mouse_event(0x0001, ($tx - $ax), ($ty - $ay), 0, [IntPtr]::Zero)
  $ax = $tx; $ay = $ty
  Start-Sleep -Milliseconds 12
}
# 3-2) 커서를 읽어 남은 만큼 되짚는다.
#      Gain 은 실측 계수라 마우스 가속 설정이 바뀌면 어긋난다. 읽어서 맞추면 그 의존이 사라진다.
$cur = New-Object Drv+POINT
for ($k = 0; $k -lt 6; $k++) {
  [void][Drv]::GetCursorPos([ref]$cur)
  $ex = $sx - $cur.X; $ey = $sy - $cur.Y
  if ([Math]::Abs($ex) -le 2 -and [Math]::Abs($ey) -le 2) { break }
  [Drv]::mouse_event(0x0001, [int]([Math]::Round($ex / $Gain)), [int]([Math]::Round($ey / $Gain)), 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 60
}
[void][Drv]::GetCursorPos([ref]$cur)
"커서 목표 ($sx,$sy) 실제 ($($cur.X),$($cur.Y))"
Start-Sleep -Milliseconds 500
if ($Click) {
  # -Hold 을 주면 그만큼 누른 채로 있는다. 녹스는 버튼을 누르고 있는 동안 커서 쪽으로 걷는다.
  $down = if ($Hold -gt 0) { $Hold } else { 90 }
  # nox.cfg 실측: MOUSE_BUTTON_RIGHT = MoveForward (이동), MOUSE_BUTTON_LEFT = Action
  # 걷게 하려면 -Right 로 오른쪽 버튼을 누른 채 유지한다.
  $bDown = if ($Right) { 0x0008 } else { 0x0002 }
  $bUp   = if ($Right) { 0x0010 } else { 0x0004 }
  [Drv]::mouse_event($bDown, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds $down
  [Drv]::mouse_event($bUp, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 1600
}
[Drv]::Shot($h, $Out)
