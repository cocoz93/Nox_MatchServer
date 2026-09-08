# onox_round.ps1 — 「고치고 화면으로 확인한다」 한 바퀴를 통째로 돈다.
#
#   내 서버(18591) 새로 띄우기 → 프록시(18590) 새로 띄우기 → 클라 새로 띄우기
#   → 메뉴부터 인게임까지 몰기 → 화면 찍기 → 서버가 보낸 조각 순서 찍기
#
# 왜 통째로 도나 — 손으로 하면 한 바퀴에 대여섯 번 왕복이고, 중간에 클라가
# 「The server is not responding」 대화상자에서 굳으면(★그 버튼은 클릭이 안 먹는다)
# 그 다음 시험이 **옛 세션의 재전송**을 재고 있게 된다. 실제로 그렇게 한 번
# 「HUD 가 떴다」고 오판할 뻔했다 — S->C 가 0건인데 화면엔 직전 접속의 잔상이 남아 있었다.
#
#   .\tools\onox_round.ps1 -Tag snap6
#     → _evidence\CAP_<Tag>.txt · _evidence\onox_<Tag>.png
param(
  [Parameter(Mandatory=$true)][string]$Tag,
  [switch]$Genuine          # 내 서버 대신 정품(opennox-server)을 뒤에 둔다 — 대조군용
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$dump = Join-Path $ev ("CAP_{0}.txt" -f $Tag)
$shot = Join-Path $ev ("onox_{0}.png" -f $Tag)

function Spawn([string]$cmd, [string]$cwd) {
  # ★ 툴 세션이 끝나면 자식이 같이 죽는다 → WMI 로 띄운다
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $cmd; CurrentDirectory = $cwd }
  if ($r.ReturnValue -ne 0) { throw "못 띄움($($r.ReturnValue)): $cmd" }
  return $r.ProcessId
}

Get-Process nox_server, nox_proxy, opennox-server -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
if (Test-Path $dump) { Remove-Item $dump -Force }

if ($Genuine) {
  $exe = Join-Path $root 'third_party\opennox\opennox-server.exe'
  $bpid = Spawn ('"' + $exe + '" -serveronly -noDraw -port 18591') (Split-Path -Parent $exe)
  Write-Host "정품 서버 pid=$bpid"
} else {
  $exe = Join-Path $root 'build\Release\nox_server.exe'
  $bpid = Spawn ('"' + $exe + '" --port 18591 --quiet') $root
  Write-Host "내 서버 pid=$bpid"
}
Start-Sleep -Seconds 3

$px = Join-Path $root 'build\Release\nox_proxy.exe'
$ppid = Spawn ('"' + $px + '" --listen 18590 --host 127.0.0.1 --host-port 18591 --dump "' + $dump + '"') $root
Write-Host "프록시 pid=$ppid"
Start-Sleep -Seconds 2

# 클라는 늘 새로 띄운다 — 굳은 클라를 되살리는 것보다 싸고 확실하다
$out = & (Join-Path $PSScriptRoot 'onox_restart.ps1')
$cpid = [int]($out | Select-Object -Last 1)
Write-Host "클라 pid=$cpid"

# 인트로 넘기기
$w = Join-Path $PSScriptRoot 'winctl.ps1'
Start-Sleep -Seconds 14
foreach ($i in 1..4) { & $w -Target $cpid -Front -Key 0x1B | Out-Null; Start-Sleep -Seconds 2 }

& (Join-Path $PSScriptRoot 'onox_join.ps1') -Target $cpid -Wake -Trace | Out-Null
Start-Sleep -Seconds 6
& $w -Target $cpid -Shot $shot | Out-Null

$sc = @(Select-String -Path $dump -Pattern 'S->C' -ErrorAction SilentlyContinue).Count
Write-Host ""
Write-Host ("=== 서버→클라 {0}건 · 화면 {1} ===" -f $sc, $shot)
if ($sc -lt 50) { Write-Host "⚠ 접속이 안 닿았다 — 화면에 보이는 건 옛 상태다" }
else {
  $snap = Join-Path $ev ("SNAP_{0}.txt" -f $Tag)
  & (Join-Path $PSScriptRoot 'cap2sniff.ps1') -In $dump -Out $snap | Out-Null
  & (Join-Path $root 'build\Release\nox_opcount.exe') $snap --seq 14 |
    Select-String -Pattern '^\s+\[조각' | Select-Object -First 14
}
