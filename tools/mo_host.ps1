# mo_host.ps1 — 다른 PC가 붙을 수 있게 이 PC의 MO 서버를 연다/닫는다 (리포 맨 위 서버_MO_열기.bat · 서버_MO_닫기.bat 이 부른다).
#
#   -Mode Open [-PublicIp a.b.c.d] [-AllowIp a.b.c.d] [-NoDb]
#       ★ 7단계 — 로비는 설정 [db](MySQL)에 붙어야 뜬다. -NoDb 면 메모리 저장소(--no-db — 계정 · 판 기록이 로비와 같이 사라짐)
#       ① 방화벽 열기(tools\mo_firewall.ps1 — 승인 창 1회, -AllowIp 를 주면 그 주소만)
#       ② ★ 10-20 v2 작업 6 — 로비(nox_lobby — 클라 TCP 4000 · 게임 서버 연결 127.0.0.1:4010)가 안 떠 있으면 띄운다
#          ★ 로그인 암호 조각 6 — 로비는 --no-auth(아이디만 · 설정에 [auth] 가 있어도). 로그인 서버(nox_auth)는 이 판에 안 띄운다
#          ★ 10-28 다른 PC 암호 로그인 — 설정에 [auth] 가 있고 -NoAuth 가 아니면 암호 판: 로비에 --no-auth 안 붙임 · ③.5 로그인 서버를 띄우고
#          방화벽에 8443 도 연다. 다른 PC 의 DLL 은 꾸러미의 인증서 지문(tools\pack_client.ps1)으로 이 PC 로그인 서버를 가른다(그 PC 믿는 루트 안 씀)
#       ③ ★ 10-20 v2 — 게임 서버(nox_server — UDP 18590 · --lobby 127.0.0.1:4010 · 방 없이 시작 · 새 방은 로비가 묻는다)가 안 떠 있으면 띄운다.
#          같은 PC 라 등록 주소 0 — 로비가 클라가 붙은 주소(공인 IP)를 게임 서버 주소로 알려 준다. 게임 서버를 다른 PC 에 두면 --register-ip 가 있어야 한다
#       ③.5 ★ 10-28 — 암호 판이면 로그인 서버(nox_auth — https://+:8443 · 가입 /signup)가 안 떠 있으면 띄운다(사람 1회 auth_setup 뒤라야 뜸)
#       ④ 라우터(4005)를 바깥용으로 다시 띄운다 — --bind 0.0.0.0
#          (★ 10-14 작업 8b — 4005 는 라우터 `nox_router` 가 받는다. 로비 주소 설정이 없어 「클라가 붙은 주소」:4000 을 알려 준다)
#          (★ 10-14 작업 8d — 로비 nox_lobby 를 없애고 채널이 4000 을 들었다 → ★ 10-20 v2 에서 로비를 되살림)
#   -Mode Close
#       ① 라우터 끄기 ② 방화벽 닫기(승인 창 1회). 로비 · 게임 서버는 그대로 둔다(창을 닫으면 꺼진다 — 끝 줄 셈을 찍고 나감)
#
# ⚠ 이 PC는 공유기 없이 공인 IP 가 직접 붙어 있어, 여는 동안 세 포트가 인터넷에 드러난다 — 시험이 끝나면 꼭 닫기.
#   로비 · 게임 서버는 시간 제한 · 접속 상한 · 낯선 패킷 시험이 없다(Docs/LOBBY_DESIGN.md 「다른 PC용 꾸러미」 절).
param([ValidateSet('Open', 'Close')][string]$Mode = 'Open', [string]$PublicIp = '', [string]$AllowIp = '', [switch]$NoDb, [switch]$NoAuth)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$routerExe = Join-Path $repo 'build\Release\nox_router.exe'
$authExe = Join-Path $repo 'build\Release\nox_auth.exe'   # ★ 10-28
$serverExe = Join-Path $repo 'build\Release\nox_server.exe'
$lobbyExe = Join-Path $repo 'build\Release\nox_lobby.exe'   # ★ 10-20 v2 작업 6
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

# 라우터는 창도 저장할 상태도 없는 콘솔 프로그램이라 바로 끈다(서버클라_실행.bat 이 nox_server 를 끄는 것과 같다).
#   ★ 10-20 v2 — 로비(nox_lobby)는 다시 쓰는 프로세스라 여기서 끄지 않는다(옛 꼴은 작업 8d 에 없앤 옛 로비를 같이 껐다)
function Stop-Router {
  Get-Process nox_router -ErrorAction SilentlyContinue | ForEach-Object {
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

    # ★ 10-28 — 암호 판인가(설정에 [auth] · -NoAuth 아님). 방화벽 8443 · 로비 --no-auth · 로그인 서버 띄우기가 이것을 따른다
    $cfgPath = Join-Path $repo 'config\nox_mo.ini'
    $authOn = -not $NoAuth -and (Test-Path -LiteralPath $cfgPath) -and ([IO.File]::ReadAllText($cfgPath) -match '(?m)^\[auth\]')
    Write-Host "로그인: $(if ($authOn) { '암호(로그인 서버 8443)' } else { '아이디만' })"

    $extra = @(); if ($AllowIp) { $extra = @('-AllowIp', $AllowIp) }
    if ($authOn) { $extra += '-Auth' }
    $fw = Invoke-Firewall 'Open' $extra
    if (-not ($fw -match '\[열림\]')) { throw '방화벽을 열지 못했다 — 여기서 멈춤(로비 · 게임 서버 · 라우터를 바깥용으로 띄우지 않음)' }

    # ★ 10-10 토큰 작업 3 — 로비 · 게임 서버가 설정 파일의 입장 토큰 키를 쓴다(★ 10-20 v2 — 연결 확인 키 · 게임 서버별 키도 이것에서). 없으면 새로 만든다(★ 10-14 작업 8d — 옛 [rooms] 는 없앴다).
    #   키는 비밀이라 git 밖 `config\`(.gitignore). 다른 PC 클라는 꾸러미의 wsock32.dll(작업 4)이 있어야 로비에 로그인된다
    $cfgDir = Join-Path $repo 'config'
    $cfg = Join-Path $cfgDir 'nox_mo.ini'
    if (-not (Test-Path -LiteralPath $cfg)) {
      New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
      $key = New-Object byte[] 32; (New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($key)
      $hex = -join ($key | ForEach-Object { $_.ToString('x2') })
      [IO.File]::WriteAllText($cfg, "; Nox_Origin MO 설정 — 입장 토큰 키는 비밀(커밋 금지)`r`n[token]`r`nkey = $hex`r`n", (New-Object Text.UTF8Encoding($false)))
      Write-Host "설정: 새로 만듦 — $cfg"
    } else {
      Write-Host "설정: $cfg"
    }

    # ★ 10-20 v2 작업 6 — 로비 — 클라 TCP 4000 을 모든 주소로 듣는 nox_lobby 가 있으면 그것을 쓴다. 다른 것이 4000 을 들면(옛 채널) 멈춤
    $l4000 = @(Get-NetTCPConnection -State Listen -LocalPort 4000 -ErrorAction SilentlyContinue)
    $lobbyUp = @($l4000 | Where-Object { $_.LocalAddress -eq '0.0.0.0' -and (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName -eq 'nox_lobby' })
    if ($lobbyUp.Count -ge 1) {
      Write-Host '로비: 이미 떠 있음 (TCP 4000 · 게임 서버 연결 127.0.0.1:4010) — 같은 설정 파일로 띄운 것이어야 토큰이 맞는다 · 암호/아이디만도 띄울 때 정해진 대로'
    } elseif ($l4000.Count -ge 1) {
      throw "TCP 4000 을 다른 프로그램이 듣는다(옛 채널 nox_server --lobby-port 이거나 루프백 로비) — 그 창을 닫고 다시 실행하세요"
    } else {
      $lobbyArgs = @('--config', "`"$cfg`"", '--bind', '0.0.0.0', '--lobby-port', '4000', '--link-bind', '127.0.0.1', '--link-port', '4010', '--quiet')
      if ($NoDb) { $lobbyArgs += '--no-db' }   # ★ 7단계
      # ★ 로그인 암호 조각 6 — 다른 PC 판은 아이디만(정할 것 c · 로그인 서버 인증서 이름이 127.0.0.1 · localhost 뿐이라 바깥 PC 의 DLL 이 못 믿음)
      #   ★ 10-28 — DLL 지문 고정으로 풀림 → 아이디만은 -NoAuth 이거나 설정에 [auth] 가 없을 때만
      if (-not $authOn) { $lobbyArgs += '--no-auth' }
      Start-Process -FilePath $lobbyExe -ArgumentList $lobbyArgs
      Start-Sleep -Seconds 2
      if (-not (Get-NetTCPConnection -State Listen -LocalPort 4000 -ErrorAction SilentlyContinue)) { throw '로비가 뜨지 않았다 — 로비 창의 글을 보세요(★ 7단계 — [멈춤] DB 면 사람 1회 config\db_setup.sql 또는 -NoDb)' }
      Write-Host '로비: 띄움 (클라 TCP 0.0.0.0:4000 · 게임 서버 연결 127.0.0.1:4010)'
    }
    # 게임 서버 — UDP 18590 을 든 nox_server 가 있으면 그것을 쓴다(로비에 붙어 있는지는 로비 창의 「등록」 줄로 본다)
    if (Get-NetUDPEndpoint -LocalPort 18590 -ErrorAction SilentlyContinue) {
      Write-Host '게임 서버: 이미 떠 있음 (UDP 18590) — --lobby 127.0.0.1:4010 으로 띄운 것이어야 로비 목록에 방이 생긴다'
    } else {
      Start-Process -FilePath $serverExe -ArgumentList @('--port', '18590', '--lobby', '127.0.0.1:4010', '--config', "`"$cfg`"", '--quiet', '--aoi', '--friends')
      Start-Sleep -Seconds 3
      if (-not (Get-NetUDPEndpoint -LocalPort 18590 -ErrorAction SilentlyContinue)) { throw '게임 서버가 뜨지 않았다 — 게임 서버 창의 글을 보세요' }
      Write-Host '게임 서버: 띄움 (UDP 18590 · 로비에 붙음 · 방 없이 시작)'
    }

    # ★ 10-28 — 로그인 서버. http.sys(System)가 대신 들어 포트 주인으로는 못 보니 가입 페이지가 답하나로 본다(이 PC 는 자작 인증서를 믿는 루트에 둠)
    if ($authOn) {
      $authUp = { try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 -Uri 'https://127.0.0.1:8443/signup').StatusCode -eq 200 } catch { $false } }
      if (Get-Process nox_auth -ErrorAction SilentlyContinue) {
        Write-Host '로그인 서버: 이미 떠 있음 (https://+:8443)'
      } else {
        Start-Process -FilePath $authExe -ArgumentList @('--config', "`"$cfg`"")
        $up = $false
        for ($i = 0; $i -lt 15 -and -not $up; $i++) { Start-Sleep -Seconds 1; $up = & $authUp }
        if (-not $up) { throw '로그인 서버가 뜨지 않았다 — 로그인 서버 창의 글을 보세요(사람 1회 config\auth_setup.sql · tools\auth_setup.ps1 전이면 -NoAuth)' }
        Write-Host "로그인 서버: 띄움 (https://+:8443 · 가입 https://$PublicIp`:8443/signup)"
      }
    }

    Stop-Router   # 루프백(127.0.0.1)용으로 떠 있던 라우터는 끄고 바깥용으로 다시 띄운다
    Start-Process -FilePath $routerExe -ArgumentList @('--config', "`"$cfg`"", '--bind', '0.0.0.0')
    Start-Sleep -Seconds 1
    $listen = @(Get-NetTCPConnection -State Listen -LocalPort 4005 -ErrorAction SilentlyContinue | Where-Object LocalAddress -eq '0.0.0.0')
    if ($listen.Count -lt 1) { throw '라우터가 0.0.0.0:4005 로 뜨지 않았다 — 라우터 창의 글을 보세요' }
    Write-Host "라우터: 띄움 — 0.0.0.0:4005 → 로비 $PublicIp`:4000(클라가 붙은 주소)"
    if ($authOn) { Write-Host "다른 PC 꾸러미 = tools\pack_client.ps1 -ServerIp $PublicIp (인증서 지문을 싣는다 · 가입 https://$PublicIp`:8443/signup)" }
    Write-Host '[열림] 이제 다른 PC 에서 2_PLAY.bat — 시험이 끝나면 꼭 서버_MO_닫기.bat'
  } else {
    Stop-Router
    $fw = Invoke-Firewall 'Close' @()
    if (-not ($fw -match '\[닫힘\]')) { throw '방화벽을 닫지 못했다 — 다시 실행하거나 mo_firewall.ps1 -Mode Close 를 직접' }
    Write-Host '[닫힘] 라우터 끔 · 방화벽 규칙 지움 — 로비 · 게임 서버 창은 필요 없으면 닫으세요'
  }
} catch {
  Write-Host "[실패] $($_.Exception.Message)"
  exit 1
}
