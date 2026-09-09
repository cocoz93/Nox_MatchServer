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
function Drive([int]$p) {
  & $di -Target $p -Cmd @('inject 1','mb 0 1','sleep 200','mb 0 0','sleep 1200','mb 0 1','sleep 200','mb 0 0') | Out-Null
  Write-Host "  메뉴 대기 ${WaitMenu}초..."
  Start-Sleep -Seconds $WaitMenu
  & $di -Target $p -Cmd ($home3 + @('m 280 167','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2000')) | Out-Null
  & $di -Target $p -Cmd ($home3 + @('m 314 267','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2500')) | Out-Null
  & $di -Target $p -Cmd ($home3 + @('m 259 57','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2000')) | Out-Null
  & $di -Target $p -Cmd @('m 14 100','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 3000') | Out-Null
  & $di -Target $p -Cmd ($home3 + @('m 313 372','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 8000')) | Out-Null
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

# 창을 나란히 — 왼쪽/오른쪽
Start-Sleep -Seconds 2
$i = 0
foreach ($p in $pids) {
  $h = (Get-Process -Id $p -ErrorAction SilentlyContinue).MainWindowHandle
  if ($h -and $h -ne 0) { [void][W]::MoveWindow($h, ($i * 960), 0, 950, 730, $true) }
  $i++
}
Write-Host ""
Write-Host ("클라 pid: {0}" -f ($pids -join ', '))
Write-Host "두 창을 나란히 놓았다. **상대가 보이는지** 눈으로 볼 것."