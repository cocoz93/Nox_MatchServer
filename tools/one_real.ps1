# one_real.ps1 — **진짜 클라 하나 + 가짜 하나**로 한 바퀴. 숨은 데스크톱이라 화면을 안 뺏는다.
#
# 왜 이렇게 하나 — 「둘이 서로 보이나」를 재는 데 진짜 클라가 둘 필요했는데,
#   메뉴 클릭이 자주 빗나가 하루의 절반이 판정이 아니라 클릭 고치는 데 갔다(2026-09-09).
#   한쪽을 화면 없는 가짜(`ghost.ps1`)로 바꾸면 **클릭이 절반**이고 그쪽은 어긋날 일이 없다.
#
#   .\tools\one_real.ps1 -Tag m1 [-ServerArgs '...'] [-Seconds 180]
param(
  [Parameter(Mandatory=$true)][string]$Tag,
  [string]$ServerArgs = '',
  [int]$Seconds = 180,
  [int]$WaitMenu = 40,
  # 싸움에 쓸 시간(초). 0 이면 안 싸운다.
  [int]$FightSeconds = 0
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$ctrl = Join-Path $ev 'desk1'
$dump = Join-Path $ev ("CAP_{0}.txt" -f $Tag)
$slog = Join-Path $ev ("srv_{0}.log" -f $Tag)

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Runtime.InteropServices;
public class PWA {
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

function Spawn([string]$cmd, [string]$cwd) {
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $cmd; CurrentDirectory = $cwd }
  if ($r.ReturnValue -ne 0) { Write-Host "[FAIL] 못 띄움($($r.ReturnValue)): $cmd"; exit 1 }
  return $r.ProcessId
}
function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
  $r = New-Object IO.StreamReader($f,[Text.Encoding]::UTF8); $t = $r.ReadToEnd(); $r.Close(); $f.Close(); return $t
}

Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
foreach ($try in 1..12) {
  $busy = @(netstat -ano -p UDP | Select-String ':1859[01]\s')
  if ($busy.Count -eq 0) { break }
  Start-Sleep -Seconds 1
}
foreach ($f in @($dump, $slog)) { if (Test-Path $f) { [IO.File]::Delete($f) } }

$spid = Spawn ('cmd /c ""' + (Join-Path $root 'build\Release\nox_server.exe') +
  '" --port 18591 ' + $ServerArgs + ' > "' + $slog + '" 2>&1"') $root
Write-Host ("서버 pid={0} {1}" -f $spid, $ServerArgs)
Start-Sleep -Seconds 3
$ppid = Spawn ('"' + (Join-Path $root 'build\Release\nox_proxy.exe') +
  '" --listen 18590 --host 127.0.0.1 --host-port 18591 --dump "' + $dump + '"') $root
Write-Host "프록시 pid=$ppid"
Start-Sleep -Seconds 2

# ★ **진짜를 먼저 넣는다.** 가짜를 먼저 넣었더니 진짜 클라 화면이 검었다(2026-09-09).
#   서버가 「이미 사람이 있는」 상태로 서두를 만들면 뭔가 어긋난다 — 그건 따로 볼 것.
#   지금 재려는 것은 **먼저 있던 사람에게 새 사람이 그려지나**이므로 이 순서가 맞다.
& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag ("{0}R" -f $Tag) -WaitMenu $WaitMenu

$gpid = Spawn ('powershell -NoProfile -ExecutionPolicy Bypass -File "' +
  (Join-Path $PSScriptRoot 'ghost.ps1') + '" -Port 18590 -Seconds ' + ($Seconds + 60) +
  ' -Tag ghost_' + $Tag) $root
Write-Host "가짜 클라 pid=$gpid"
Start-Sleep -Seconds 8
$rpid = 0
$f = Join-Path $ctrl 'lastpid.txt'
if (Test-Path $f) { [void][int]::TryParse(([IO.File]::ReadAllText($f)).Trim(), [ref]$rpid) }
Write-Host "진짜 클라 pid=$rpid"

# ★ 싸움 — 진짜 클라가 가짜를 때려 판을 끝낸다.
#   왼버튼은 **누르는 순간 한 번만** 오므로 눌렀다 떼기를 되풀이한다(2026-09-09 실측).
#   주문키(A~G = 스캔코드 0x1E~0x22)도 섞는다.
if ($FightSeconds -gt 0) {
  $di = Join-Path $PSScriptRoot 'di.ps1'
  Write-Host ("--- 싸움 {0}초 ---" -f $FightSeconds)
  $pulse = @('inject 1')
  foreach ($i in 1..20) { $pulse += @('mb 0 1','sleep 80','mb 0 0','sleep 420') }
  foreach ($k in @(0x1E,0x22)) { $pulse += @(("k 0x{0:x2} 1" -f $k),'sleep 120',("k 0x{0:x2} 0" -f $k),'sleep 600') }
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt $FightSeconds) {
    & $di -Target $rpid -Cmd $pulse | Out-Null
    $tk = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
    if ($tk.Count) { Write-Host ("  {0}" -f $tk[0]) }
    if ($tk.Count -and $tk[0] -match '판끝 예') { Write-Host '  ★ 판이 끝났다'; break }
    if (-not (Get-Process -Id $rpid -ErrorAction SilentlyContinue)) { Write-Host '  ⚠ 진짜 클라가 죽었다'; break }
  }
}

# 판정 — 서버가 세는 사람 수 · 진짜 클라 화면
Start-Sleep -Seconds 3
$tick = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' })
if ($tick.Count) { Write-Host ("마지막 틱: {0}" -f $tick[-1]) }
# ★ 진짜 클라는 **숨은 데스크톱**에 있다. 이쪽에서 `MainWindowHandle` 은 0 이고
#   PrintWindow 도 못 닿는다 — 그 데스크톱 안에서 도는 `deskhost` 에게 찍게 해야 한다.
if ($rpid -gt 4) {
  $shot = Join-Path $ctrl ("see_{0}.png" -f $Tag)
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'),
    ("target {0}`nshotfull {1}`n" -f $rpid, $shot), (New-Object Text.UTF8Encoding($false)))
  Start-Sleep -Seconds 3
  Write-Host ("화면: {0}" -f $shot)
} else { Write-Host '[FAIL] 진짜 클라 pid 를 못 잡았다' }