# mo_firewall.ps1 — 바깥 PC 접속 시험 때만 방화벽을 연다 (MO 바깥 접속, Docs/LOBBY_DESIGN.md).
#
#   -Mode Open  : 들어오는 허용 규칙 둘 — nox_lobby.exe TCP 4005·4000 · nox_server.exe UDP 18590 (규칙 이름 앞 "NoxMO ")
#   -Mode Close : 이름이 "NoxMO " 로 시작하는 규칙만 지운다
#   -Mode Show  : 지금 있는 녹스 관련 규칙을 적기만 한다
#
# ⚠ 이 PC는 공유기 없이 공인 IP 가 직접 붙어 있어 열면 인터넷 전체에 열린다. 로비는 인증이 없고 게임 서버는
#   바깥 패킷을 그대로 받으므로 시험이 끝나면 꼭 Close.
# 관리자 권한이 없으면 스스로 승인 창을 띄운다. 결과는 %TEMP%\nox_mo_firewall_result.txt (리포를 더럽히지 않게).
param([ValidateSet('Open', 'Close', 'Show')][string]$Mode = 'Show')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$result = Join-Path $env:TEMP 'nox_mo_firewall_result.txt'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Remove-Item -LiteralPath $result -ErrorAction SilentlyContinue
  try {
    Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList @(
      '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"", '-Mode', $Mode)
  } catch { Write-Host '[실패] 관리자 승인을 받지 못했다'; exit 1 }
  if (Test-Path -LiteralPath $result) { Get-Content -LiteralPath $result -Encoding UTF8 } else { Write-Host '[실패] 결과 파일 없음' }
  exit 0
}

$log = New-Object System.Collections.Generic.List[string]
function Say($s) { $log.Add($s) }
try {
  $lobby = Join-Path $repo 'build\Release\nox_lobby.exe'
  $server = Join-Path $repo 'build\Release\nox_server.exe'
  if ($Mode -eq 'Open') {
    Get-NetFirewallRule -DisplayName 'NoxMO *' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName 'NoxMO lobby TCP 4005,4000' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 4005, 4000 -Program $lobby -Profile Any | Out-Null
    New-NetFirewallRule -DisplayName 'NoxMO server UDP 18590' -Direction Inbound -Action Allow -Protocol UDP -LocalPort 18590 -Program $server -Profile Any | Out-Null
    Say '[열림] NoxMO 규칙 둘'
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
