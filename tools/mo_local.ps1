# mo_local.ps1 — 이 PC 하나에서 정품 클라로 v2(로비 · 게임 서버 나눔)를 보는 판 (MO 6단계 v2 작업 6 · Docs/LOBBY_SPLIT_DESIGN.md 「사람(맨 끝 한 장)」).
#
#   -Mode Start [-NoClients] [-OneClient]
#       ① hosts 확인 — servserv.westwood.com 이 127.0.0.1 이어야 정품 인터넷 메뉴가 우리 라우터로 온다(아니면 tools\hosts_lobby.ps1 -Mode On 안내 후 멈춤)
#       ② 설정 config\nox_mo.ini(없으면 입장 토큰 키를 새로 — git 밖 · 비밀)
#       ③ 라우터 nox_router 127.0.0.1:4005 → ④ 로비 nox_lobby 127.0.0.1:4000(게임 서버 연결 127.0.0.1:4010)
#       ⑤ 게임 서버 nox_server UDP 18590 --lobby 127.0.0.1:4010(로비 기록에서 등록 확인 — 방 없이 시작 · 새 방은 로비가 묻는다)
#       ⑥ 클라1 Game.exe · 클라2 Game2.exe(-OneClient 면 클라1 만 · -NoClients 면 안 띄움). DLL 기록(NOX_HOOK_LOG_DIR)도 같은 기록 폴더에
#       기록 = _out\mo_local\<월일_시분초>\ — router.log · lobby.log · server.log · 판 결과는 게임 서버 실행 파일 옆 results\matches.jsonl
#   -Mode Stop
#       게임 서버 · 로비 = 닫기 이벤트(nox_server_stop_<pid> · nox_lobby_stop_<pid> — 끝 줄 셈을 찍고 나감 · 응답 없을 때만 강제)
#       라우터 = 끔(상태 · 셈이 없는 콘솔 — mo_host.ps1 과 같음). 클라는 사람이 게임 메뉴로 끈다
#
# 왜 — 10-20 v2 로 로비와 게임 서버가 다른 프로세스가 됐다. 옛 사람 판은 채널 하나(nox_server --lobby-port)만 띄웠다.
#   다른 PC 를 붙이는 판은 mo_host.ps1(서버_MO_열기.bat) — 이것은 같은 PC 루프백 전용(바깥에 안 엶)
param([ValidateSet('Start', 'Stop')][string]$Mode = 'Start', [switch]$NoClients, [switch]$OneClient)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\Release'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'

function Stop-Servers {
  foreach ($kind in @('nox_server', 'nox_lobby')) {
    foreach ($p in @(Get-Process $kind -ErrorAction SilentlyContinue)) {
      try { [void][Threading.EventWaitHandle]::OpenExisting("$($kind)_stop_$($p.Id)").Set() } catch { }
      if (-not $p.WaitForExit(8000)) { Stop-Process -Id $p.Id -Force; Write-Host "$kind pid $($p.Id) — 닫기 이벤트에 답이 없어 강제 종료" }
      else { Write-Host "$kind pid $($p.Id) — 닫기 이벤트로 끔" }
    }
  }
  Get-Process nox_router -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force; Write-Host "nox_router pid $($_.Id) 끔" }
}

try {
  if ($Mode -eq 'Stop') { Stop-Servers; Write-Host '[닫힘] 클라는 게임 메뉴로 끄세요'; exit 0 }

  $now = @([Net.Dns]::GetHostAddresses('servserv.westwood.com') | ForEach-Object { $_.IPAddressToString })
  if (-not ($now -contains '127.0.0.1')) {
    throw "servserv.westwood.com → $($now -join ',') — 정품 인터넷 메뉴가 우리 라우터로 안 온다. 먼저: powershell -ExecutionPolicy Bypass -File `"$PSScriptRoot\hosts_lobby.ps1`" -Mode On"
  }
  foreach ($exe in @('nox_router.exe', 'nox_lobby.exe', 'nox_server.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $exe))) { throw "실행 파일이 없다 — 빌드 먼저(tools\build.ps1): $exe" }
  }
  Stop-Servers   # 앞 판에 남은 것

  $cfgDir = Join-Path $repo 'config'
  $cfg = Join-Path $cfgDir 'nox_mo.ini'
  if (-not (Test-Path -LiteralPath $cfg)) {
    New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
    $key = New-Object byte[] 32; (New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($key)
    $hex = -join ($key | ForEach-Object { $_.ToString('x2') })
    [IO.File]::WriteAllText($cfg, "; Nox_Origin MO 설정 — 입장 토큰 키는 비밀(커밋 금지)`r`n[token]`r`nkey = $hex`r`n", (New-Object Text.UTF8Encoding($false)))
    Write-Host "설정: 새로 만듦 — $cfg"
  }
  $logDir = Join-Path $repo ('_out\mo_local\' + (Get-Date -Format 'MMdd_HHmmss'))
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null

  # 창은 작게 띄우고 출력은 기록 파일로(사람 확인 뒤 내가 읽는다)
  function Launch([string]$exe, [string]$argLine, [string]$log) {   # `$args` 는 PowerShell 자동 변수라 다른 이름
    Start-Process -FilePath cmd.exe -ArgumentList "/c `"`"$(Join-Path $bin $exe)`" $argLine > `"$log`" 2>&1`"" -WindowStyle Minimized
  }
  Launch 'nox_router.exe' "--bind 127.0.0.1" (Join-Path $logDir 'router.log')
  Launch 'nox_lobby.exe' "--config `"$cfg`" --bind 127.0.0.1 --lobby-port 4000 --link-bind 127.0.0.1 --link-port 4010" (Join-Path $logDir 'lobby.log')
  $ok = $false
  for ($i = 0; $i -lt 50 -and -not $ok; $i++) { Start-Sleep -Milliseconds 200; $ok = [bool](Get-NetTCPConnection -State Listen -LocalPort 4000 -ErrorAction SilentlyContinue) }
  if (-not $ok) { throw "로비가 TCP 4000 을 못 열었다 — $logDir\lobby.log" }
  Launch 'nox_server.exe' "--port 18590 --lobby 127.0.0.1:4010 --config `"$cfg`" --aoi --friends" (Join-Path $logDir 'server.log')
  $reg = $false
  for ($i = 0; $i -lt 60 -and -not $reg; $i++) {
    Start-Sleep -Milliseconds 250
    try {
      $fs = New-Object IO.FileStream((Join-Path $logDir 'lobby.log'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
      $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); $t = $sr.ReadToEnd(); $sr.Close()
      $reg = $t -match '등록 — 실행'
    } catch { }
  }
  if (-not $reg) { throw "게임 서버가 로비에 등록하지 않았다 — $logDir\server.log · lobby.log" }
  Write-Host "서버: 라우터 127.0.0.1:4005 · 로비 127.0.0.1:4000 · 게임 서버 UDP 18590(로비에 등록됨) — 기록 $logDir"

  if (-not $NoClients) {
    $env:NOX_HOOK_LOG_DIR = $logDir   # 클라 DLL 기록도 같은 폴더에
    $c1 = Start-Process -FilePath (Join-Path $game 'Game.exe') -WorkingDirectory $game -PassThru
    Write-Host "클라1 Game.exe pid $($c1.Id)"
    if (-not $OneClient) {
      Start-Sleep -Seconds 3
      $c2 = Start-Process -FilePath (Join-Path $game 'Game2.exe') -WorkingDirectory $game -PassThru
      Write-Host "클라2 Game2.exe pid $($c2.Id)"
    }
  }
  Write-Host '[열림] 정품 — 인터넷(Westwood Online) 로그인 → 채팅 채널 → 「게임」 → Host Game / 목록에서 방 · 끝나면 mo_local.ps1 -Mode Stop'
} catch {
  Write-Host "[실패] $($_.Exception.Message)"
  exit 1
}
