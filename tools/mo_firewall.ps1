# mo_firewall.ps1 — 바깥 PC 접속 시험 때만 방화벽을 연다 (MO 바깥 접속, Docs/LOBBY_DESIGN.md).
#
#   -Mode Open [-AllowIp a.b.c.d] : 들어오는 허용 규칙 셋 — nox_router.exe TCP 4005 · nox_lobby.exe TCP 4000 · nox_server.exe UDP 18590 (규칙 이름 앞 "NoxMO ")
#                                   ★ 10-14 작업 8b — 4005 는 라우터가 받는다(옛 규칙 = nox_lobby.exe 4005·4000 하나)
#                                   -AllowIp 를 주면 그 주소에서 오는 것만 받는다(없으면 아무 주소 — 10-07 「잠깐 켤 동안은 열어도」)
#   -Mode Close : 이름이 "NoxMO " 로 시작하는 규칙만 지운다
#   -Mode Show  : 지금 있는 녹스 관련 규칙을 적기만 한다
#
# ⚠ 이 PC는 공유기 없이 공인 IP 가 직접 붙어 있어 열면 인터넷 전체에 열린다. 로비는 인증이 없고 게임 서버는
#   바깥 패킷을 그대로 받으므로 시험이 끝나면 꼭 Close.
# 관리자 권한이 없으면 스스로 승인 창을 띄운다. 결과는 %TEMP%\nox_mo_firewall_result.txt (리포를 더럽히지 않게).
param([ValidateSet('Open', 'Close', 'Show')][string]$Mode = 'Show', [string]$AllowIp = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$result = Join-Path $env:TEMP 'nox_mo_firewall_result.txt'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Remove-Item -LiteralPath $result -ErrorAction SilentlyContinue
  try {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"", '-Mode', $Mode)
    if ($AllowIp) { $a += @('-AllowIp', $AllowIp) }
    Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList $a
  } catch { Write-Host '[실패] 관리자 승인을 받지 못했다'; exit 1 }
  if (Test-Path -LiteralPath $result) { Get-Content -LiteralPath $result -Encoding UTF8 } else { Write-Host '[실패] 결과 파일 없음' }
  exit 0
}

$log = New-Object System.Collections.Generic.List[string]
function Say($s) { $log.Add($s) }
try {
  $router = Join-Path $repo 'build\Release\nox_router.exe'
  $lobby = Join-Path $repo 'build\Release\nox_lobby.exe'
  $server = Join-Path $repo 'build\Release\nox_server.exe'
  if ($Mode -eq 'Open') {
    if ($AllowIp -and $AllowIp -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw "-AllowIp 꼴이 아니다: $AllowIp" }
    $remote = if ($AllowIp) { $AllowIp } else { 'Any' }
    Get-NetFirewallRule -DisplayName 'NoxMO *' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName 'NoxMO router TCP 4005' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 4005 -Program $router -RemoteAddress $remote -Profile Any | Out-Null
    New-NetFirewallRule -DisplayName 'NoxMO lobby TCP 4000' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 4000 -Program $lobby -RemoteAddress $remote -Profile Any | Out-Null
    New-NetFirewallRule -DisplayName 'NoxMO server UDP 18590' -Direction Inbound -Action Allow -Protocol UDP -LocalPort 18590 -Program $server -RemoteAddress $remote -Profile Any | Out-Null
    Say "[열림] NoxMO 규칙 셋 — 받는 주소 $remote"
  } elseif ($Mode -eq 'Close') {
    $n = @(Get-NetFirewallRule -DisplayName 'NoxMO *' -ErrorAction SilentlyContinue).Count
    Get-NetFirewallRule -DisplayName 'NoxMO *' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    Say "[닫힘] NoxMO 규칙 $n 개 지움"
  }
  # 지금 상태 — 프로그램 경로나 이름에 nox 가 든 들어오는 규칙
  $rules = Get-NetFirewallRule -Direction Inbound | Where-Object {
    $_.DisplayName -match '(?i)nox' -or (($_ | Get-NetFirewallApplicationFilter).Program -match '(?i)nox') }
  Say "녹스 관련 들어오는 규칙 $(@($rules).Count) 개"
  foreach ($r in $rules) {
    $pf = $r | Get-NetFirewallPortFilter
    Say ("  {0} | {1} | 켜짐 {2} | {3} {4} | {5}" -f $r.DisplayName, $r.Action, $r.Enabled, $pf.Protocol, $pf.LocalPort, ($r | Get-NetFirewallApplicationFilter).Program)
  }
} catch {
  Say "[실패] $($_.Exception.Message)"
} finally {
  [IO.File]::WriteAllLines($result, $log, (New-Object Text.UTF8Encoding($true)))
}
