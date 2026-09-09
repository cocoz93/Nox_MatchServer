# two_visible.ps1 — 정품 서버에 정품 클라 둘을 **보이는 화면에** 띄워 인게임까지 몰아 둔다.
#
# 왜 따로 만드나 — `two_genuine.ps1` 은 숨은 데스크톱(NoxDesk1)에서 돈다.
#   사람이 눈으로 보려면 일반 데스크톱이라야 한다. 사람이 판정할 것 하나 —
#   **정품 서버에서 두 사람이 서로 보이는가.**
#
#   .\tools\two_visible.ps1              정품 서버(estate)
#   .\tools\two_visible.ps1 -Mine        내 서버로
param(
  [switch]$Mine,
  [string]$ServerArgs = '',
  [int]$WaitMenu = 40
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
$di   = Join-Path $PSScriptRoot 'di.ps1'

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing;
using System.Runtime.InteropServices;
public class PWX {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
  public static Bitmap Shot(IntPtr h) {
    RECT r; GetWindowRect(h, out r); int w=r.R-r.L, t=r.B-r.T;
    if (w<=0||t<=0) return null; Bitmap b=new Bitmap(w,t);
    using (Graphics g=Graphics.FromImage(b)) { IntPtr dc=g.GetHdc(); PrintWindow(h,dc,2); g.ReleaseHdc(dc); }
    return b; }
}
"@
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

function Spawn([string]$cmd, [string]$cwd) {
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $cmd; CurrentDirectory = $cwd }
  if ($r.ReturnValue -ne 0) { Write-Host "[FAIL] 못 띄움($($r.ReturnValue)): $cmd"; exit 1 }
  return $r.ProcessId
}

Get-Process nox_server, nox_proxy, opennox, opennox-server, deskhost -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
foreach ($try in 1..12) {
  $busy = @(netstat -ano -p UDP | Select-String ':1859[01]\s')
  if ($busy.Count -eq 0) { break }
  Start-Sleep -Seconds 1
}

if ($Mine) {
  $slog = Join-Path $ev 'srv_visible.log'
  if (Test-Path $slog) { [IO.File]::Delete($slog) }
  $spid = Spawn ('cmd /c ""' + (Join-Path $root 'build\Release\nox_server.exe') +
    '" --port 18591 ' + $ServerArgs + ' > "' + $slog + '" 2>&1"') $root
  Write-Host "내 서버 pid=$spid"
} else {
  $g = Join-Path $root 'third_party\opennox\opennox-server.exe'
  $spid = Spawn ('"' + $g + '" -serveronly -noDraw -port 18591 ' + $ServerArgs) (Split-Path -Parent $g)
  Write-Host "정품 서버 pid=$spid"
}
Start-Sleep -Seconds 3
$dump = Join-Path $ev 'CAP_visible.txt'
if (Test-Path $dump) { [IO.File]::Delete($dump) }
$ppid = Spawn ('"' + (Join-Path $root 'build\Release\nox_proxy.exe') +
  '" --listen 18590 --host 127.0.0.1 --host-port 18591 --dump "' + $dump + '"') $root
Write-Host "프록시 pid=$ppid"
Start-Sleep -Seconds 2

# 메뉴 좌표는 창 1280x960 기준을 2.04 로 나눈 값(join_my_server.ps1 과 같다).
$home3 = @('m -3000 -3000','m -3000 -3000','m -3000 -3000','sleep 300')
# 창을 찍어 색 가짓수를 센다. 화면이 넘어갔는지 가르는 값이다.
function ShotColors([int]$p, [string]$tag) {
  $f = Join-Path $ev ("step_{0}.png" -f $tag)
  & (Join-Path $PSScriptRoot 'winctl.ps1') -Target $p -Front -Shot $f | Out-Null
  if (-not (Test-Path $f)) { return -1 }
  Add-Type -AssemblyName System.Drawing
  $b = [System.Drawing.Bitmap]::FromFile($f)
  $set = New-Object 'System.Collections.Generic.HashSet[int]'
  for ($y = 0; $y -lt $b.Height; $y += 4) { for ($x = 0; $x -lt $b.Width; $x += 4) { [void]$set.Add($b.GetPixel($x,$y).ToArgb()) } }
  $b.Dispose()
  return $set.Count
}

function IsMenu([int]$q) {
  $h=(Get-Process -Id $q -ErrorAction SilentlyContinue).MainWindowHandle
  if (-not $h) { return $true }
  $b=[PWX]::Shot($h); if (-not $b) { return $true }
  if ($b.Width -lt 300 -or $b.Height -lt 300) { $b.Dispose(); return $true }
  $c=$b.GetPixel(1060,148); $b.Dispose()
  # ★ 지문은 **목록 줄 오른쪽 끝의 흰 「Open」 글자**다(R235 G235 B235 실측).
  #   붉은 「Host Game」 단추를 쓰면 메인 메뉴에도 있어서 못 가른다.
  return ($c.R -gt 200 -and $c.G -gt 200 -and $c.B -gt 200)
}

function Drive([int]$p) {
  & $di -Target $p -Cmd @('inject 1','mb 0 1','sleep 200','mb 0 0','sleep 1200','mb 0 1','sleep 200','mb 0 0') | Out-Null
  Write-Host "  메뉴 대기 ${WaitMenu}초..."
  Start-Sleep -Seconds $WaitMenu
  & $di -Target $p -Cmd ($home3 + @('m 280 167','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2000')) | Out-Null
  & $di -Target $p -Cmd ($home3 + @('m 314 267','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2500')) | Out-Null
  # ★ 창을 찍어 **화면이 넘어갔는지 보고** 다음을 누른다. 좌표는 2026-09-09 에
  #   보이는 화면에서 한 걸음씩 확인한 값이다(창 1296x999 · 클라 1280x960 · 주입은 /2.04):
  #     서버 줄  창(546,148) → 주입(264,57)
  #     Join     창(565,350) → 주입(273,156)   ← ⚠ 팝업 위에서는 홈 금지, **상대 이동만**
  #     Load     창(648,790) → 주입(314,372)
  foreach ($try in 1..4) {
    & $di -Target $p -Cmd ($home3 + @('m 264 57','sleep 900','mb 0 1','sleep 250','mb 0 0','sleep 3000')) | Out-Null
    & $di -Target $p -Cmd @('m 9 99','sleep 900','mb 0 1','sleep 250','mb 0 0','sleep 4000') | Out-Null
    & $di -Target $p -Cmd ($home3 + @('m 314 372','sleep 900','mb 0 1','sleep 250','mb 0 0','sleep 11000')) | Out-Null
    if (-not (IsMenu $p)) { Write-Host ("    시도 {0} — 들어감" -f $try); break }
    Write-Host ("    시도 {0} — 아직 목록, 다시" -f $try)
  }
  # 관전 해제 — SPACE → ENTER → 왼클릭 → ESC (마지막 ESC 를 빼면 채팅줄이 키를 삼킨다)
  & $di -Target $p -Cmd @('k 0x39 1','sleep 150','k 0x39 0','sleep 900','k 0x1C 1','sleep 150','k 0x1C 0','sleep 900','mb 0 1','sleep 150','mb 0 0','sleep 2000','k 0x01 1','sleep 150','k 0x01 0','sleep 800') | Out-Null
}

$pids = @()
foreach ($exe in @('Game3.exe','Game4.exe')) {
  Write-Host "--- $exe 띄우고 몰기 ---"
  [void](Spawn ('"' + (Join-Path $game $exe) + '"') $game)
  Start-Sleep -Seconds 9
  $nm = [IO.Path]::GetFileName($exe)
  $p = [int]((Get-CimInstance Win32_Process -Filter "Name='$nm'" | Select-Object -Last 1).ProcessId)
  if ($p -le 0) { Write-Host "[FAIL] $exe 안 떴다"; continue }
  $pids += $p
  Write-Host "  pid=$p"
  Drive $p
}

# ⚠ **창 크기를 바꾸지 않는다.** 940x720 으로 줄였더니 그 뒤 몰기가 어긋나
#   서버 목록에서 멈추는 일이 세 번 났다(2026-09-09). 자리만 옮긴다.
Start-Sleep -Seconds 2
$i = 0
foreach ($q in $pids) {
  $h = (Get-Process -Id $q -ErrorAction SilentlyContinue).MainWindowHandle
  if ($h -and $h -ne 0) { [void][W]::MoveWindow($h, ($i * 300), 0, 1296, 999, $true) }
  $i++
}
Write-Host ""
Write-Host ("클라 pid: {0}" -f ($pids -join ', '))
Write-Host "두 창을 나란히 놓았다. **상대가 보이는지** 눈으로 볼 것."