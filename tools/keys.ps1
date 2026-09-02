# keys.ps1 — 게임 창에 키를 보낸다. 좌표가 필요 없어 게임 안 조작에 쓴다.
#
#   .\tools\keys.ps1 -Pid_ 1234 -Keys 'HOME' -Out shot.png
#   .\tools\keys.ps1 -Pid_ 1234 -Keys 'ENTER','안녕','ENTER'
#
# 이름 있는 키: ENTER ESC HOME END TAB SPACE F1..F12 UP DOWN LEFT RIGHT
# 그 밖의 문자열은 한 글자씩 타이핑한다.
param([int]$Pid_, [string[]]$Keys, [string]$Out, [int]$Delay = 400)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class Keys2 {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern short VkKeyScanA(char c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

  public static void ForceForeground(IntPtr h) {
    ShowWindow(h, 9);
    keybd_event(0x12, 0, 0, IntPtr.Zero);
    keybd_event(0x12, 0, 2, IntPtr.Zero);
    BringWindowToTop(h);
    SetForegroundWindow(h);
  }
  public static void Tap(byte vk) {
    keybd_event(vk, 0, 0, IntPtr.Zero);
    System.Threading.Thread.Sleep(60);
    keybd_event(vk, 0, 2, IntPtr.Zero);
  }
  public static void TypeChar(char c) {
    short v = VkKeyScanA(c);
    byte vk = (byte)(v & 0xFF);
    bool shift = ((v >> 8) & 1) != 0;
    if (shift) keybd_event(0x10, 0, 0, IntPtr.Zero);
    Tap(vk);
    if (shift) keybd_event(0x10, 0, 2, IntPtr.Zero);
  }
  public static string Shot(IntPtr h, string path) {
    RECT r; GetWindowRect(h, out r);
    int w = r.Right - r.Left, ht = r.Bottom - r.Top;
    using (Bitmap b = new Bitmap(w, ht)) using (Graphics g = Graphics.FromImage(b)) {
      IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 2); g.ReleaseHdc(hdc);
      b.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
    return "shot ok";
  }
}
'@ -ReferencedAssemblies System.Drawing

$named = @{
  'ENTER' = 0x0D; 'ESC' = 0x1B; 'HOME' = 0x24; 'END' = 0x23; 'TAB' = 0x09; 'SPACE' = 0x20;
  'UP' = 0x26; 'DOWN' = 0x28; 'LEFT' = 0x25; 'RIGHT' = 0x27; 'BACKSPACE' = 0x08;
  'F1' = 0x70; 'F2' = 0x71; 'F3' = 0x72; 'F4' = 0x73; 'F5' = 0x74; 'F6' = 0x75;
  'F7' = 0x76; 'F8' = 0x77; 'F9' = 0x78; 'F10' = 0x79; 'F11' = 0x7A; 'F12' = 0x7B
}

$h = (Get-Process -Id $Pid_ -ErrorAction Stop).MainWindowHandle
if ($h -eq [IntPtr]::Zero) { "ABORT: 창 없음"; exit 1 }
$fp = 0
for ($t = 1; $t -le 10; $t++) {
  [Keys2]::ForceForeground($h)
  Start-Sleep -Milliseconds 250
  [void][Keys2]::GetWindowThreadProcessId([Keys2]::GetForegroundWindow(), [ref]$fp)
  if ($fp -eq $Pid_) { break }
}
if ($fp -ne $Pid_) { "ABORT: focus=$fp"; exit 1 }
Start-Sleep -Milliseconds 300

foreach ($k in $Keys) {
  if ($named.ContainsKey($k.ToUpper())) {
    [Keys2]::Tap([byte]$named[$k.ToUpper()])
    Write-Host "  키: $k"
  } else {
    foreach ($c in $k.ToCharArray()) { [Keys2]::TypeChar($c); Start-Sleep -Milliseconds 40 }
    Write-Host "  타이핑: $k"
  }
  Start-Sleep -Milliseconds $Delay
}
Start-Sleep -Milliseconds 600
if ($Out) { [Keys2]::Shot($h, $Out) }
