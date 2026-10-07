# hosts_lobby.ps1 — 이 PC의 정품 녹스 웨스트우드 온라인을 우리 로비(nox_lobby)로 돌리거나 되돌린다.
#
#   -Mode On  [-Ip 127.0.0.1] : hosts 에 주석 한 줄 + "<Ip> servserv.westwood.com" · "<Ip> irc.westwood.com" (표시 NOX_MO_LOBBY)
#   -Mode Off                 : 표시가 붙은 줄만 지운다 → 녹스 인터넷 메뉴가 진짜 XWIS 로 간다
#
# 켜 두는 동안: 이 PC의 웨스트우드 온라인(녹스 · 같은 모듈을 쓰는 다른 웨스트우드 게임)은 늘 이 주소로 간다.
#   우리 로비를 안 띄운 날 인터넷 메뉴를 누르면 연결 실패가 난다(10-07 사용자 결정 — 3·4단계 동안 켜 둠).
# 1단계 녹음 때 쓴 표시 NOX_WOL_RELAY 줄도 같이 지운다. 관리자 권한이 없으면 스스로 승인 창을 띄운다.
# 바꾸기 전 hosts 사본 = _evidence\_hosts_backup\ (git 제외) · 결과 = %TEMP%\nox_hosts_lobby_result.txt
param([ValidateSet('On', 'Off')][string]$Mode = 'On', [string]$Ip = '127.0.0.1')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$result = Join-Path $env:TEMP 'nox_hosts_lobby_result.txt'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Remove-Item -LiteralPath $result -ErrorAction SilentlyContinue
  try {
    Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList @(
      '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"", '-Mode', $Mode, '-Ip', $Ip)
  } catch { Write-Host '[실패] 관리자 승인을 받지 못했다'; exit 1 }
  if (Test-Path -LiteralPath $result) { Get-Content -LiteralPath $result -Encoding UTF8 } else { Write-Host '[실패] 결과 파일 없음' }
  exit 0
}

$log = New-Object System.Collections.Generic.List[string]
function Say($s) { $log.Add($s) }
$latin1 = [Text.Encoding]::GetEncoding(28591)   # hosts 를 바이트 그대로 읽고 쓴다 — 다른 줄을 깨뜨리지 않게
try {
  if ($Ip -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw "IP 꼴이 아니다: $Ip" }
  $h = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
  $bak = Join-Path $repo '_evidence\_hosts_backup'
  New-Item -ItemType Directory -Force -Path $bak | Out-Null
  Copy-Item -LiteralPath $h -Destination (Join-Path $bak ('hosts.bak_{0:yyyyMMdd_HHmmss}_{1}' -f (Get-Date), $Mode))
  $lines = @($latin1.GetString([IO.File]::ReadAllBytes($h)) -split "`r?`n" | Where-Object { $_ -notmatch 'NOX_MO_LOBBY|NOX_WOL_RELAY' })
  while ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = @($lines | Select-Object -First ($lines.Count - 1)) }
  if ($Mode -eq 'On') {
    # hosts 엔 ASCII 만 — 주석도 영어로 둔다
    $lines += "# NOX_MO_LOBBY Nox_Origin MO lobby test ({0:yyyy-MM-dd}) - revert: powershell -ExecutionPolicy Bypass -File `"{1}`" -Mode Off" -f (Get-Date), (Join-Path $PSScriptRoot 'hosts_lobby.ps1')
    $lines += "$Ip servserv.westwood.com # NOX_MO_LOBBY"
    $lines += "$Ip irc.westwood.com # NOX_MO_LOBBY"
  }
  [IO.File]::WriteAllBytes($h, $latin1.GetBytes(($lines -join "`r`n") + "`r`n"))
  ipconfig /flushdns | Out-Null
  $now = ([Net.Dns]::GetHostAddresses('servserv.westwood.com') | ForEach-Object { $_.IPAddressToString }) -join ','
  Say "[$Mode] servserv.westwood.com → $now"
  Get-Content -LiteralPath $h | Where-Object { $_ -match 'NOX_MO_LOBBY' } | ForEach-Object { Say "  hosts: $_" }
} catch {
  Say "[실패] $($_.Exception.Message)"
} finally {
  [IO.File]::WriteAllLines($result, $log, (New-Object Text.UTF8Encoding($true)))
}
