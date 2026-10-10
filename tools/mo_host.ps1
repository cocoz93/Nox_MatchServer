# mo_host.ps1 — 다른 PC가 붙을 수 있게 이 PC의 MO 서버를 연다/닫는다 (리포 맨 위 서버_MO_열기.bat · 서버_MO_닫기.bat 이 부른다).
#
#   -Mode Open [-PublicIp a.b.c.d] [-AllowIp a.b.c.d]
#       ① 방화벽 열기(tools\mo_firewall.ps1 — 승인 창 1회, -AllowIp 를 주면 그 주소만)
#       ② 게임 서버(UDP 18590)가 안 떠 있으면 띄운다 — 서버클라_실행.bat 과 같은 실행값(Estate 아레나)
#       ③ 로비를 바깥용으로 다시 띄운다 — --bind 0.0.0.0 · 게임 서버 주소 = 이 PC 공인 IP
#   -Mode Close
#       ① 로비 끄기 ② 방화벽 닫기(승인 창 1회). 게임 서버는 그대로 둔다(서버 창을 닫으면 꺼진다)
#
# ⚠ 이 PC는 공유기 없이 공인 IP 가 직접 붙어 있어, 여는 동안 세 포트가 인터넷에 드러난다 — 시험이 끝나면 꼭 닫기.
#   로비 · 게임 서버는 시간 제한 · 접속 상한 · 낯선 패킷 시험이 없다(Docs/LOBBY_DESIGN.md 「다른 PC용 꾸러미」 절).
param([ValidateSet('Open', 'Close')][string]$Mode = 'Open', [string]$PublicIp = '', [string]$AllowIp = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lobbyExe = Join-Path $repo 'build\Release\nox_lobby.exe'
$serverExe = Join-Path $repo 'build\Release\nox_server.exe'
$map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\Estate\Estate.map'
$firewall = Join-Path $PSScriptRoot 'mo_firewall.ps1'
$fwResult = Join-Path $env:TEMP 'nox_mo_firewall_result.txt'   # mo_firewall.ps1 이 승인 창 뒤에서 남기는 결과(UTF-8)

# 방화벽 스크립트를 부르고 결과 파일로 판정한다 — 다른 프로세스의 화면 글자를 읽으면 한글 인코딩이 어긋날 수 있다
function Invoke-Firewall([string]$m, [string[]]$extra) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File $firewall -Mode $m @extra | Out-Null
  if (-not (Test-Path -LiteralPath $fwResult)) { return @() }
  $r = @(Get-Content -LiteralPath $fwResult -Encoding UTF8)
  $r | ForEach-Object { Write-Host "  방화벽: $_" }
  return $r
}

# 사설 · 루프백 · 자동 할당 주소를 뺀 IPv4 — 이 PC는 이더넷에 공인 IP 가 직접 붙어 있다(공유기 뒤면 후보가 0)
function Get-PublicIpCandidates {
  @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
      $_.IPAddress -notmatch '^(10\.|127\.|169\.254\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)' } | ForEach-Object IPAddress)
}

# 로비는 창도 저장할 상태도 없는 콘솔 프로그램이라 바로 끈다(서버클라_실행.bat 이 nox_server 를 끄는 것과 같다)
function Stop-Lobby {
  Get-Process nox_lobby -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-Process -Id $_.Id -Force
    Write-Host "로비: 떠 있던 것 끔 (pid $($_.Id))"
  }
}

try {
  if ($Mode -eq 'Open') {
    if (-not $PublicIp) {
      $c = @(Get-PublicIpCandidates)   # 후보가 하나면 함수가 문자열로 풀어 돌려줘 $c[0] 이 첫 글자가 된다(10-07 「공인 IP: 2」)
      if ($c.Count -ne 1) { throw "공인 IP 를 정하지 못했다(후보: $($c -join ', ')) — -PublicIp 로 주세요. 공유기 뒤라면 공유기 포트 포워딩도 필요" }
      $PublicIp = $c[0]
    }
    Write-Host "공인 IP: $PublicIp"

    $extra = @(); if ($AllowIp) { $extra = @('-AllowIp', $AllowIp) }
    $fw = Invoke-Firewall 'Open' $extra
    if (-not ($fw -match '\[열림\]')) { throw '방화벽을 열지 못했다 — 여기서 멈춤(로비를 바깥용으로 띄우지 않음)' }

    if (Get-NetUDPEndpoint -LocalPort 18590 -ErrorAction SilentlyContinue) {
      Write-Host '게임 서버: 이미 떠 있음 (UDP 18590)'
    } else {
      # 10-10 토큰 작업 2 — 로비가 토큰을 내고(작업 3) 클라 DLL 이 싣기(작업 4) 전까지는 토큰 검사 끄기로 띄운다. 작업 4 에서 설정 파일로 바꾼다
      Start-Process -FilePath $serverExe -ArgumentList @('--port', '18590', '--standalone', '--map', "`"$map`"", '--mode', 'arena', '--quiet', '--aoi', '--friends', '--no-join-token')
      Start-Sleep -Seconds 3
      if (-not (Get-NetUDPEndpoint -LocalPort 18590 -ErrorAction SilentlyContinue)) { throw '게임 서버가 뜨지 않았다 — 서버 창의 글을 보세요' }
      Write-Host '게임 서버: 띄움 (Estate 아레나, UDP 18590)'
    }

    Stop-Lobby   # 루프백(127.0.0.1)용으로 떠 있던 로비는 끄고 바깥용으로 다시 띄운다
    Start-Process -FilePath $lobbyExe -ArgumentList @('--bind', '0.0.0.0', '--game-ip', $PublicIp, '--room-name', 'Nox_Origin', '--map', 'estate', '--mode', 'arena')
    Start-Sleep -Seconds 1
    $listen = @(Get-NetTCPConnection -State Listen -LocalPort 4005, 4000 -ErrorAction SilentlyContinue | Where-Object LocalAddress -eq '0.0.0.0')
    if ($listen.Count -lt 2) { throw '로비가 0.0.0.0:4005 · 4000 으로 뜨지 않았다 — 로비 창의 글을 보세요' }
    Write-Host "로비: 띄움 — 0.0.0.0:4005 · 4000 · 방 Nox_Origin → 게임 서버 $PublicIp`:18590"
    Write-Host '[열림] 이제 다른 PC 에서 2_PLAY.bat — 시험이 끝나면 꼭 서버_MO_닫기.bat'
  } else {
    Stop-Lobby
    $fw = Invoke-Firewall 'Close' @()
    if (-not ($fw -match '\[닫힘\]')) { throw '방화벽을 닫지 못했다 — 다시 실행하거나 mo_firewall.ps1 -Mode Close 를 직접' }
    Write-Host '[닫힘] 로비 끔 · 방화벽 규칙 지움 — 게임 서버 창은 필요 없으면 닫으세요'
  }
} catch {
  Write-Host "[실패] $($_.Exception.Message)"
  exit 1
}
