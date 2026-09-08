# onox_two.ps1 — 클라 **둘**을 내 서버에 붙이는 한 바퀴를 통째로 돈다.
#
#   서버(18591) → 프록시(18590) → 클라 A 접속 → 클라 B 접속 → 양쪽 화면 촬영
#
# ★★ **서버도 매 바퀴 새로 띄운다.** 죽은 클라의 세션이 남아 있으면
#   새 접속이 서버 목록으로 되돌아온다(2026-09-08 실측). 이걸 몰라 두 번 헤맸다.
#
#   .\tools\onox_two.ps1 -Tag two1 [-ServerArgs '--raw-snapshot']
param(
  [Parameter(Mandatory=$true)][string]$Tag,
  [string]$ServerArgs = '',
  # ★ 내 서버 대신 정품(opennox-server)을 뒤에 둔다 — **가르는 시험**용.
  #   「둘째가 붙으면 첫째가 죽는다」가 내 서버 탓인지 클라 탓인지 여기서 갈린다.
  [switch]$Genuine
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$dump = Join-Path $ev ("CAP_{0}.txt" -f $Tag)
$exe  = Join-Path $root 'third_party\opennox\opennox.exe'

function Spawn([string]$cmd, [string]$cwd) {
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $cmd; CurrentDirectory = $cwd }
  if ($r.ReturnValue -ne 0) { throw "못 띄움($($r.ReturnValue)): $cmd" }
  return $r.ProcessId
}

Get-Process nox_server, nox_proxy, opennox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
if (Test-Path $dump) { Remove-Item $dump -Force }

# 서두는 **오늘 뜬 정품 덤프**를 쓴다 — 09-07 덤프는 지금 클라와 값이 어긋난다
$prelude = Join-Path $ev 'SNAP_onox_ref.txt'
if ($Genuine) {
  $g = Join-Path $root 'third_party\opennox\opennox-server.exe'
  $spid = Spawn ('"' + $g + '" -serveronly -noDraw -port 18591') (Split-Path -Parent $g)
} else {
  # ★ 서버 로그를 파일로 받는다 — WMI 로 띄우면 콘솔이 없어 stdout 이 사라진다.
  #   틱 로그의 「사람 N(갱신에 M)」이 **서로 보이는지**를 가르는 자리다.
  $slog = Join-Path $ev ("srv_{0}.log" -f $Tag)
  if (Test-Path $slog) { Remove-Item $slog -Force }
  $spid = Spawn ('cmd /c ""' + (Join-Path $root 'build\Release\nox_server.exe') + '" --port 18591 --dump "' + $prelude + '" ' + $ServerArgs + ' > "' + $slog + '" 2>&1"') $root
}
Start-Sleep -Seconds 3
$ppid = Spawn ('"' + (Join-Path $root 'build\Release\nox_proxy.exe') + '" --listen 18590 --host 127.0.0.1 --host-port 18591 --dump "' + $dump + '"') $root
Write-Host ("서버 {0} · 프록시 {1}" -f $spid, $ppid)
Start-Sleep -Seconds 2

# 클라 둘. ★ 포트를 갈라 준다 — 같은 PC에서 둘을 돌리는 것이니
$aPid = Spawn ('"' + $exe + '" -window -logs "' + $ev + '\onox_A"') (Split-Path -Parent $exe)
Start-Sleep -Seconds 4
$bPid = Spawn ('"' + $exe + '" -window -clientport 18700 -logs "' + $ev + '\onox_B"') (Split-Path -Parent $exe)
Write-Host ("클라 A {0} · B {1}" -f $aPid, $bPid)
Start-Sleep -Seconds 24

$drive = Join-Path $PSScriptRoot 'onox_drive.ps1'
Write-Host "--- A 몰기 ---"
& $drive -Target $aPid -Shot (Join-Path $ev ("onox_{0}_A.png" -f $Tag)) | Out-Null
# ★ A 가 아직 살아 있나 — 「둘째가 들어오면 첫째가 죽는다」를 여기서 가른다
$alive1 = $null -ne (Get-Process -Id $aPid -ErrorAction SilentlyContinue)
Write-Host ("A 상태(B 몰기 전): {0}" -f $(if ($alive1) { '살아있음' } else { '죽음' }))

Write-Host "--- B 몰기 ---"
& $drive -Target $bPid -Shot (Join-Path $ev ("onox_{0}_B.png" -f $Tag)) | Out-Null

# ⚠ 여기서 기다리지 않는다 — 앞선 회차에 이 3초 사이에 A 가 죽어 못 찍었다(2026-09-08)
$alive2 = $null -ne (Get-Process -Id $aPid -ErrorAction SilentlyContinue)
Write-Host ("A 상태(B 몰기 후): {0}" -f $(if ($alive2) { '살아있음' } else { '죽음' }))
# A 를 다시 찍는다 — B 가 들어온 뒤 **A 화면에 B 가 보이는지**가 판정이다
if ($alive2) {
  & (Join-Path $PSScriptRoot 'winctl.ps1') -Target $aPid -Front -Shot (Join-Path $ev ("onox_{0}_A2.png" -f $Tag)) | Out-Null
}

$sc = @(Select-String -Path $dump -Pattern 'S->C' -ErrorAction SilentlyContinue).Count
Write-Host ""
Write-Host ("=== 서버→클라 {0}건 ===" -f $sc)
# ★ 서버가 스스로 말하는 사람 수 — 와이어를 뜯어 세는 것보다 이쪽이 믿을 만하다
$slog2 = Join-Path $ev ("srv_{0}.log" -f $Tag)
if (Test-Path $slog2) {
  Write-Host "--- 서버 틱 로그 마지막 3줄 ---"
  Get-Content $slog2 -Tail 3 -ErrorAction SilentlyContinue
}
Write-Host ("화면: onox_{0}_A.png · onox_{0}_B.png · onox_{0}_A2.png(B 접속 뒤 A 화면)" -f $Tag)
# 상대별 조각 수 — 둘 다 제대로 붙었으면 양쪽 다 수십 개가 나온다
$snap = Join-Path $ev ("SNAP_{0}.txt" -f $Tag)
& (Join-Path $PSScriptRoot 'cap2sniff.ps1') -In $dump -Out $snap | Out-Null
& (Join-Path $root 'build\Release\nox_opcount.exe') $snap --seq 300 |
  Select-String -Pattern '^\s+\[조각' |
  ForEach-Object { if ("$_" -match ':(\d{4,5})\s+len') { $matches[1] } } |
  Group-Object | Select-Object @{n='상대포트';e={$_.Name}}, Count | Format-Table -AutoSize
