# mo_host.ps1 — 다른 PC가 붙을 수 있게 이 PC의 MO 서버를 연다/닫는다 (리포 맨 위 서버_MO_열기.bat · 서버_MO_닫기.bat 이 부른다).
#
#   -Mode Open [-PublicIp a.b.c.d] [-AllowIp a.b.c.d]
#       ① 방화벽 열기(tools\mo_firewall.ps1 — 승인 창 1회, -AllowIp 를 주면 그 주소만)
#       ② 게임 서버(UDP 18590)가 안 떠 있으면 띄운다 — 서버클라_실행.bat 과 같은 실행값(Estate 아레나)
#       ③ 라우터(4005)와 로비(4000)를 바깥용으로 다시 띄운다 — --bind 0.0.0.0 · 게임 서버 주소 = 이 PC 공인 IP
#          (★ 10-14 작업 8b — 4005 는 라우터 `nox_router` 가 받는다. 채널 주소 설정이 없어 「클라가 붙은 주소」:4000 을 알려 준다)
#   -Mode Close
#       ① 라우터 · 로비 끄기 ② 방화벽 닫기(승인 창 1회). 게임 서버는 그대로 둔다(서버 창을 닫으면 꺼진다)
#
# ⚠ 이 PC는 공유기 없이 공인 IP 가 직접 붙어 있어, 여는 동안 세 포트가 인터넷에 드러난다 — 시험이 끝나면 꼭 닫기.
#   로비 · 게임 서버는 시간 제한 · 접속 상한 · 낯선 패킷 시험이 없다(Docs/LOBBY_DESIGN.md 「다른 PC용 꾸러미」 절).
param([ValidateSet('Open', 'Close')][string]$Mode = 'Open', [string]$PublicIp = '', [string]$AllowIp = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lobbyExe = Join-Path $repo 'build\Release\nox_lobby.exe'
$routerExe = Join-Path $repo 'build\Release\nox_router.exe'
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

# 로비 · 라우터는 창도 저장할 상태도 없는 콘솔 프로그램이라 바로 끈다(서버클라_실행.bat 이 nox_server 를 끄는 것과 같다)
function Stop-Lobby {
  Get-Process nox_lobby, nox_router -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-Process -Id $_.Id -Force
    Write-Host "$($_.ProcessName): 떠 있던 것 끔 (pid $($_.Id))"
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

    # ★ 10-10 토큰 작업 3 — 로비와 게임 서버가 같은 설정 파일(입장 토큰 키 · 방 목록)을 쓴다. 없으면 키를 새로 만든다.
    #   키는 비밀이라 git 밖 `config\`(.gitignore). 다른 PC 클라는 꾸러미의 wsock32.dll(작업 4)이 있어야 로비에 로그인된다
    $cfgDir = Join-Path $repo 'config'
    $cfg = Join-Path $cfgDir 'nox_mo.ini'
    if (-not (Test-Path -LiteralPath $cfg)) {
      New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
      $key = New-Object byte[] 32; (New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($key)
      $hex = -join ($key | ForEach-Object { $_.ToString('x2') })
      [IO.File]::WriteAllText($cfg, "; Nox_Origin MO 설정 — 입장 토큰 키는 비밀(커밋 금지)`r`n[token]`r`nkey = $hex`r`n[rooms]`r`nNoxOrigin = 0`r`n", (New-Object Text.UTF8Encoding($false)))
      Write-Host "설정: 새로 만듦 — $cfg"
    } else {
      Write-Host "설정: $cfg"
    }

    if (Get-NetUDPEndpoint -LocalPort 18590 -ErrorAction SilentlyContinue) {
      Write-Host '게임 서버: 이미 떠 있음 (UDP 18590) — 같은 설정 파일로 띄운 것이어야 토큰이 맞는다'
    } else {
      Start-Process -FilePath $serverExe -ArgumentList @('--port', '18590', '--standalone', '--map', "`"$map`"", '--mode', 'arena', '--quiet', '--aoi', '--friends', '--config', "`"$cfg`"")
      Start-Sleep -Seconds 3
      if (-not (Get-NetUDPEndpoint -LocalPort 18590 -ErrorAction SilentlyContinue)) { throw '게임 서버가 뜨지 않았다 — 서버 창의 글을 보세요' }
      Write-Host '게임 서버: 띄움 (Estate 아레나, UDP 18590)'
    }

    Stop-Lobby   # 루프백(127.0.0.1)용으로 떠 있던 로비 · 라우터는 끄고 바깥용으로 다시 띄운다
    Start-Process -FilePath $routerExe -ArgumentList @('--config', "`"$cfg`"", '--bind', '0.0.0.0')
    Start-Process -FilePath $lobbyExe -ArgumentList @('--config', "`"$cfg`"", '--bind', '0.0.0.0', '--game-ip', $PublicIp, '--map', 'estate', '--mode', 'arena')
    Start-Sleep -Seconds 1
    $listen = @(Get-NetTCPConnection -State Listen -LocalPort 4005, 4000 -ErrorAction SilentlyContinue | Where-Object LocalAddress -eq '0.0.0.0')
    if ($listen.Count -lt 2) { throw '라우터 · 로비가 0.0.0.0:4005 · 4000 으로 뜨지 않았다 — 두 창의 글을 보세요' }
    Write-Host "라우터: 띄움 — 0.0.0.0:4005 · 로비: 띄움 — 0.0.0.0:4000 · 방은 설정 파일 [rooms] → 게임 서버 $PublicIp`:18590"
    Write-Host '[열림] 이제 다른 PC 에서 2_PLAY.bat — 시험이 끝나면 꼭 서버_MO_닫기.bat'
  } else {
    Stop-Lobby
    $fw = Invoke-Firewall 'Close' @()
    if (-not ($fw -match '\[닫힘\]')) { throw '방화벽을 닫지 못했다 — 다시 실행하거나 mo_firewall.ps1 -Mode Close 를 직접' }
    Write-Host '[닫힘] 라우터 · 로비 끔 · 방화벽 규칙 지움 — 게임 서버 창은 필요 없으면 닫으세요'
  }
} catch {
  Write-Host "[실패] $($_.Exception.Message)"
  exit 1
}
