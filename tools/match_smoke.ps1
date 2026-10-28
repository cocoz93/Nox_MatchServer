# match_smoke.ps1 — 판 하나를 끝까지 치러 판 결과가 로비 저장까지 가는지 본다 (MO 7단계 조각 8b · Docs/ACCOUNT_DB_DESIGN.md 「실물 판」).
#
# 로비(nox_lobby) + 게임 서버(nox_server --lobby · 점수 한도 1 · 진단 죽임) + 패킷 클라 둘(alice · bob — 정품 클라 없이):
#   ① 로비 로그인(아이디만 — ★ 로그인 암호 조각 4 · 토큰 판은 조각 6 -Auth) → alice 새 방(정품 Host Game 이 DLL 로 돌아오는 꼴 JOINGAME #alice's_game) · bob 그 방
#   ② UDP 접속 시도 + 토큰 꼬리 → JOIN_OK · CONNECT + 토큰 꼬리 → 수락 + 키 · 게임 진입(0x20) + 준비(0xc0) → 상태 갱신이 옴(대기 맵)
#   ③ alice(방장 — 먼저 들어옴) 채팅 `!start`(TEXT_MESSAGE 0xA8 · `Server.cpp` 「채팅」) → 로비 거울에 판 중(ROOM … 1)
#   ④ 진단 죽임(첫 사람이 둘째를 · `--debug-kill`) → 점수 한도 1 → 판 끝 → RESULT → 로비 저장 → SAVED ok → 게임 서버가 쥔 것을 지움
#   확인 — 게임 서버 결과 파일 줄 = 로비 [판 결과] 줄(바이트) · 끝 까닭 score_player · 승자 alice · 사람 alice · bob ·
#          로비 끝 줄 판 저장 요청 1(SAVED ok 1) · 게임 서버 끝 줄 SAVED(ok 1) · 쥔 채 끝 0
#   -Db — 로비를 설정 config\nox_mo.ini([token] · [db] — 사람 1회 config\db_setup.sql 뒤)로 띄우고 DB 의 판 · 사람 · 계정 행까지 본다
#   -Auth — ★ 로그인 암호 조각 6b — 로그인 토큰 판. 시험 DB([db] test_database)에 표를 새로 깔고(001 + 002) 로그인 서버 nox_auth --test-db 를 8443 에 띄워
#          alice · bob 이 HTTPS 로 가입 → /login 으로 토큰(NXL1…) → 로비 로그인 묶음 `NOXCLIENT 3` · `apgar <토큰> 0`(DLL 이 바꿔 보낸 꼴) — 로비는 --no-auth 없이 설정 [auth] key 로 확인.
#          더 보는 것 = 로비 끝 줄 토큰 통과 2 · 아이디만 0 · 시험 DB accounts 마지막 로그인 둘 · 세 기록(로비 · 게임 서버 · 로그인 서버)에 친 암호 · 토큰 글 0곳.
#          사람 1회(인증서 · nox_auth DB 사용자 — tools\auth_setup.ps1 · config\auth_setup.sql) 전이면 종료 코드 77 = ctest 「건너뜀」(auth_smoke 와 같음) · -Db 와 같이 못 씀(계정이 시험 DB)
# 패킷 꼴은 smoke.ps1(접속 · 게임 진입 · 클릭) · v2_split_smoke.ps1(정품 접속 시도 100바이트 + 토큰 꼬리)과 같다.
#   클라 머리 둘째 바이트 = 받은 서버 신뢰 순번(되받기) — smoke.ps1 은 짧아 고정값이지만 판은 수십 초라 따라간다 · 0.5초마다 핑
# 끝은 닫기 이벤트(게임 서버 · 로비 — 끝 줄 셈을 찍고 나감) · 강제 종료는 응답이 없을 때만
param([int]$LobbyPort = 14600, [int]$LinkPort = 14610, [int]$UdpPort = 18641, [switch]$Db, [switch]$Auth, [string]$Exe = '', [string]$LobbyExe = '', [string]$AuthExe = '', [switch]$KeepLogs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\Release'
if (-not $Exe) { $Exe = Join-Path $bin 'nox_server.exe' }
if (-not $LobbyExe) { $LobbyExe = Join-Path (Split-Path -Parent $Exe) 'nox_lobby.exe' }
if (-not $AuthExe) { $AuthExe = Join-Path (Split-Path -Parent $Exe) 'nox_auth.exe' }
if ($Auth -and $Db) { Write-Host '  [실패] -Auth 와 -Db 는 같이 못 씀(로그인 서버 계정은 시험 DB · 로비 판 기록은 운영 DB)'; exit 1 }
function Skip($why) { Write-Host "[건너뜀] $why"; if ($tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }; exit 77 }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}

$waitMap = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\So_LOD\So_LOD.map'
if (-not (Test-Path -LiteralPath $waitMap)) { Write-Host "  [건너뜀] 대기 맵(So_LOD)이 없다 — 새 방을 못 만들어 이 시험을 건너뜀: $waitMap"; exit 0 }

$latin1 = [Text.Encoding]::GetEncoding(28591)
function Open($port) {
  for ($i = 0; $i -lt 50; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient
      $c.Connect('127.0.0.1', $port)
      $s = $c.GetStream(); $s.ReadTimeout = 6000
      $w = New-Object System.IO.StreamWriter($s, $latin1); $w.NewLine = "`r`n"
      return @{ c = $c; r = (New-Object System.IO.StreamReader($s, $latin1)); w = $w }
    } catch { Start-Sleep -Milliseconds 100 }
  }
  throw "접속 실패: 127.0.0.1:$port"
}
function Send($k, [string[]]$lines) { foreach ($l in $lines) { $k.w.WriteLine($l) }; $k.w.Flush() }
function Kind([string]$l) { $t = $l.Split(' '); if ($t[0] -eq ':') { $t[1] } elseif ($t[0].StartsWith(':')) { 'user:' + $t[1] } else { '?' } }
function ReadUntil($k, [string]$stop) {
  $got = @()
  while ($true) {
    try { $l = $k.r.ReadLine() } catch { return ,$got }
    if ($null -eq $l) { return ,$got }
    $got += $l
    if ((Kind $l) -eq $stop) { return ,$got }
  }
}
function ReadLog([string]$path) {
  try {
    $fs = New-Object System.IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)
    $t = $sr.ReadToEnd(); $sr.Close(); return $t
  } catch { return '' }
}
function HexBytes([string]$h) { $b = New-Object byte[] ($h.Length / 2); for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($h.Substring($i * 2, 2), 16) }; return ,$b }

# 로그인 → 새 방 또는 남의 방 참가 → NOXTOKEN 줄의 토큰 16진(없으면 빈 것)
#   ★ 로그인 암호 조각 6b — $apgar(로그인 토큰 NXL1…)를 주면 DLL 판 3 꼴로 `apgar <토큰> 0` 을 USER 앞에
function LoginJoin([string]$nick, [string]$channel, [string]$apgar = '') {
  $k = Open $LobbyPort
  $hello = @("NOXCLIENT $(if ($apgar) { 3 } else { 2 })", "NICK $nick")
  if ($apgar) { $hello += "apgar $apgar 0" }
  Send $k ($hello + @('USER UserName HostName irc.westwood.com :RealName'))
  $null = ReadUntil $k '376'
  Send $k @("JOINGAME $channel 1 ")
  $g = ReadUntil $k '366'
  $tok = ''
  if ($g.Count -ge 1 -and $g[0].StartsWith('NOXTOKEN ')) { $tok = $g[0].Split(' ')[2] }
  return @{ k = $k; tok = $tok; lines = $g }
}

# ── 패킷 클라 ──────────────────────────────────────────────
$tryJoinHex = '00000e004a00610063006b00000000000000000000000000000000001c13616e01980309000000000000000049018501f07a' +
              '69000000020a3132333435363738393031323334353637383930000000009a030100000000007465737465720000d0af0000'   # 정품 접속 시도 100바이트(proto_test RecordedTryJoin)
$nxt1 = [Text.Encoding]::ASCII.GetBytes('NXT1')
function NewClient([string]$name, [string]$tokHex) {
  $u = New-Object System.Net.Sockets.UdpClient (New-Object System.Net.IPEndPoint ([Net.IPAddress]::Parse('127.0.0.1')), 0)
  $u.Client.ReceiveTimeout = 2000
  return @{ name = $name; u = $u; tok = (HexBytes $tokHex); key = 0; ack = 0; upd = 0 }
}
$gsEp = New-Object System.Net.IPEndPoint ([Net.IPAddress]::Parse('127.0.0.1')), $UdpPort
function Raw($cl, [byte[]]$pkt) { [void]$cl.u.Send($pkt, $pkt.Length, $gsEp) }
function RecvRaw($cl) { $ep = New-Object System.Net.IPEndPoint ([Net.IPAddress]::Any), 0; try { return ,$cl.u.Receive([ref]$ep) } catch { return $null } }
# 키를 받은 뒤 보내는 것은 뒤집어 보낸다 — 머리 [스트림 1][받은 서버 신뢰 순번] + 메시지
function SendGame($cl, [byte[]]$msg) {
  $p = New-Object byte[] ($msg.Length + 2)
  $p[0] = 0x01; $p[1] = [byte]$cl.ack
  [Array]::Copy($msg, 0, $p, 2, $msg.Length)
  for ($i = 0; $i -lt $p.Length; $i++) { $p[$i] = $p[$i] -bxor $cl.key }
  Raw $cl $p
}
# 쌓인 것을 다 읽는다 — 신뢰 순번을 따라가고 상태 갱신(0xa4)을 센다
function Drain($cl) {
  while ($cl.u.Available -gt 0) {
    $g = RecvRaw $cl
    if ($null -eq $g) { break }
    $d = New-Object byte[] $g.Length
    for ($i = 0; $i -lt $g.Length; $i++) { $d[$i] = $g[$i] -bxor $cl.key }
    if ($d.Length -ge 2 -and ($d[0] -band 0x80) -ne 0 -and ($d[0] -band 0x7f) -eq 0) { $cl.ack = $d[1] }   # 스트림 0 신뢰 조각의 순번
    for ($p = 2; $p -lt $d.Length; $p++) {
      if ($d[$p] -eq 0xa4) { $cl.upd++; break }
      if ($d[$p] -eq 0x27) { $p += 2; continue }   # TIMESTAMP 1 + 2 — 앞의 시각만 건너뛴다(smoke.ps1 Find-Upd 와 같음)
      if ($d[$p] -eq 0x28) { $p += 4; continue }
      break
    }
  }
}
function Ping($cl) { SendGame $cl ([byte[]](0x06, 0x11, 0x22, 0x33, 0x44)) }
# 접속 시도 → JOIN_OK · CONNECT → 수락(키) · 게임 진입 · 준비 · 왼클릭(관전 풀기 — smoke.ps1)
function Enter($cl) {
  Raw $cl ((HexBytes $tryJoinHex) + $cl.tok + $nxt1)
  $ok = RecvRaw $cl
  $joined = $null -ne $ok -and $ok.Length -eq 3 -and $ok[2] -eq 0x14
  Raw $cl (([byte[]](0xff, 0x00, 0x00)) + $cl.tok + $nxt1)
  $acc = RecvRaw $cl
  $accepted = $null -ne $acc -and $acc.Length -eq 10 -and $acc[2] -eq 0x1f
  if ($accepted) { $cl.key = $acc[9] }
  $info = New-Object byte[] 154; $info[0] = 0x20   # 0x20 CLIENT_ACCEPT — 플레이어 정보 자리만(smoke.ps1)
  SendGame $cl $info
  SendGame $cl ([byte[]](0xc0))                    # CLIENT_READY
  Start-Sleep -Milliseconds 150
  SendGame $cl ([byte[]](0x3f, 4, 6, 0, 0, 0))     # PLAYER_INPUT 왼클릭(코드 6) — 접속 직후 관전을 푼다
  return @{ joined = $joined; accepted = $accepted }
}
# 채팅 한 줄 — [0xA8][번호 2][깃발 0x02 = UTF-8][x 2][y 2][글자 수][미상 2][글자] (`Wire.h` MessageSize · 번호 · 좌표는 서버가 새로 짓는다)
function Chat($cl, [string]$text) {
  $t = [Text.Encoding]::ASCII.GetBytes($text)
  $m = New-Object byte[] (11 + $t.Length)
  $m[0] = 0xA8; $m[3] = 0x02; $m[8] = [byte]$t.Length
  [Array]::Copy($t, 0, $m, 11, $t.Length)
  SendGame $cl $m
}
# 조건이 될 때까지 두 클라를 비우며 0.5초마다 핑(초 한도)
function Pump([scriptblock]$until, [int]$sec) {
  $sw = [Diagnostics.Stopwatch]::StartNew(); $last = -1000
  while ($sw.Elapsed.TotalSeconds -lt $sec) {
    foreach ($cl in $clients) { Drain $cl }
    if ($sw.ElapsedMilliseconds - $last -ge 500) { foreach ($cl in $clients) { Ping $cl }; $last = $sw.ElapsedMilliseconds }
    if (& $until) { return $true }
    Start-Sleep -Milliseconds 30
  }
  return $false
}

# ── 띄우기 ────────────────────────────────────────────────
$tag = $PID
$tmp = Join-Path $env:TEMP "match_smoke_$tag"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
if ($Db -or $Auth) {   # ★ 조각 6b — -Auth 도 설정 파일의 [auth] key(로비 · 로그인 서버가 같은 값) · [auth_db] 가 있어야 한다
  $ini = Join-Path $repo 'config\nox_mo.ini'
  if (-not (Test-Path -LiteralPath $ini)) { if ($Auth) { Skip "설정 파일 없음 $ini" }; Write-Host "  [실패] -Db 인데 설정이 없다: $ini"; exit 1 }
} else {
  $ini = Join-Path $tmp 'mo.ini'
  [IO.File]::WriteAllText($ini, "[token]`r`nkey = $('ef' * 32)`r`n")
}
$llog = Join-Path $tmp 'lobby.txt'; $glog = Join-Path $tmp 'server.txt'; $res = Join-Path $tmp 'matches.jsonl'; $alog = Join-Path $tmp 'auth.txt'

# ★ 로그인 암호 조각 6b — 로그인 서버 판 준비(로비 · 게임 서버보다 먼저 — 사람 1회 전이면 아무것도 안 띄운 채 77)
$ap = $null
$pwA = 'Al1ce!pw'; $pwB = 'B0bpass9'   # 정품 암호 칸 8자 안(4219c90) · 글자 + 숫자
$tokA = ''; $tokB = ''
$authStart = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
if ($Auth) {
  function IniGet($section, $key) {
    $cur = ''
    foreach ($l in [IO.File]::ReadAllLines($ini)) {
      $t = $l.Trim()
      if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
      if ($cur -eq $section -and $t -match "^$key\s*=\s*(.*)$") { return $Matches[1].Trim() }
    }
    return ''
  }
  if (-not (IniGet 'auth' 'key')) { Skip '[auth] key 없음' }
  if (-not (IniGet 'auth_db' 'user')) { Skip '[auth_db] user 없음(config/auth_setup.sql — 사람 1회)' }
  $mysql = 'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
  if (-not (Test-Path -LiteralPath $mysql)) { Skip "mysql.exe 없음 $mysql" }
  if (-not (Test-Path -LiteralPath $AuthExe)) { Write-Host "  [실패] 로그인 서버 실행 파일이 없다: $AuthExe"; exit 1 }
  $dbHost = if (IniGet 'db' 'host') { IniGet 'db' 'host' } else { '127.0.0.1' }
  $testDb = if (IniGet 'db' 'test_database') { IniGet 'db' 'test_database' } else { 'nox_mo_test' }
  # 시험 DB 에 SQL 을 로비 사용자로(auth_smoke 와 같음 — 암호는 환경 변수로만)
  function Sql([string]$text) {
    $env:MYSQL_PWD = IniGet 'db' 'password'
    try { $out = $text | & $mysql -u (IniGet 'db' 'user') -h $dbHost --default-character-set=utf8mb4 -N -B $testDb 2>&1 }
    finally { Remove-Item Env:\MYSQL_PWD -ErrorAction SilentlyContinue }
    if ($LASTEXITCODE -ne 0) { throw "SQL 실패: $out" }
    return $out
  }
  $schema = 'DROP TABLE IF EXISTS match_players, matches, accounts, schema_version;' + "`n" +
    [IO.File]::ReadAllText((Join-Path $repo 'tools\db\001_init.sql')) + "`n" + [IO.File]::ReadAllText((Join-Path $repo 'tools\db\002_auth.sql'))
  try { Sql $schema | Out-Null } catch { Skip "시험 DB $testDb 표 깔기 못 함 — $_" }
  $ap = Start-Process -FilePath $AuthExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $alog -ArgumentList @(
    '--config', "`"$ini`"", '--test-db', '--workers', '2', '--seconds', '150')
  $null = $ap.Handle
  $listening = $false
  for ($i = 0; $i -lt 150 -and -not $ap.HasExited; $i++) {
    Start-Sleep -Milliseconds 100
    if ((ReadLog $alog) -match '듣기 https') { $listening = $true; break }   # 쓰는 중인 기록은 공유 읽기로(ReadLog) — 10-27 첫 판 IOException 1회(재현 안 됨)
  }
  if (-not $listening) {
    if (-not $ap.HasExited) { Stop-Process -Id $ap.Id -Force; $null = $ap.WaitForExit(3000) }
    $text = ReadLog $alog
    Write-Host $text
    if ($text -match '사람 1회|1045') { Skip 'nox_auth 가 사람 1회 전 까닭으로 안 뜸(위 줄)' }
    Write-Host '  [실패] nox_auth 가 안 뜸'; exit 1
  }
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  function AuthReq([string]$path, [string]$body) {   # 인증서 검사를 켠 채로(브라우저 · DLL 과 같음)
    try {
      $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri ('https://127.0.0.1:8443' + $path) -Body $body -ContentType 'application/x-www-form-urlencoded'
      return @{ status = [int]$r.StatusCode; body = [string]$r.Content }
    } catch [System.Net.WebException] {
      $resp = $_.Exception.Response
      if (-not $resp) { return @{ status = 0; body = $_.Exception.Message } }
      # PowerShell 5.1 은 오류 응답 본문을 ErrorDetails 로 먼저 읽어 가 스트림이 비어 있음
      $text = if ($_.ErrorDetails) { $_.ErrorDetails.Message } else { (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
      return @{ status = [int]$resp.StatusCode; body = $text }
    }
  }
  $authOk = $true
  foreach ($u in @(@{ n = 'alice'; p = $pwA }, @{ n = 'bob'; p = $pwB })) {
    $e = [uri]::EscapeDataString($u.p)
    $s = AuthReq '/signup' "name=$($u.n)&password=$e&password2=$e"
    $l = AuthReq '/login' "name=$($u.n)&password=$e"
    $ok = $s.status -eq 201 -and $l.status -eq 200 -and $l.body -cmatch '^NXL1[0-9a-f]{114}$'
    if ($u.n -eq 'alice') { $tokA = $l.body } else { $tokB = $l.body }
    if (-not $ok) { $authOk = $false; Write-Host "  [실패] $($u.n) — 가입 $($s.status) · 로그인 $($l.status) $(if ($l.body.StartsWith('NXL!')) { $l.body })" }
  }
  Check $authOk '⓪ 로그인 서버 — alice · bob 가입 201 · /login 200 NXL1 + 16진 114자(인증서 검사 켬)'
}

$lobbyArgs = @('--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort, '--seconds', '120')   # 연결 줄을 찍게 --quiet 없이
if (-not $Db) { $lobbyArgs += '--no-db' }
if (-not $Auth) { $lobbyArgs += '--no-auth' }   # ★ 로그인 암호 조각 4 — 패킷 클라는 토큰이 없다(설정에 [auth] 가 있어도 아이디만) · ★ 조각 6b -Auth 면 토큰 확인
$lp = Start-Process -FilePath $LobbyExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $llog -ArgumentList $lobbyArgs
$null = $lp.Handle
Start-Sleep -Milliseconds 500
$gp = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $glog -ArgumentList @(
  '--port', $UdpPort, '--bind', '127.0.0.1', '--config', "`"$ini`"", '--lobby', "127.0.0.1:$LinkPort", '--quiet',
  '--battle-score-limit', '1', '--debug-kill', '300', '--results', "`"$res`"")
$null = $gp.Handle
$clients = @()
$started = (Get-Date)
try {
  Write-Host "로비 :$LobbyPort · 연결 :$LinkPort · 게임 서버 UDP $UdpPort · 저장소 $(if ($Db) { 'DB' } else { '메모리(--no-db)' })"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt 10 -and (ReadLog $llog) -notmatch '등록 — 실행') { Start-Sleep -Milliseconds 100 }
  Check ((ReadLog $llog) -match '등록 — 실행') '게임 서버가 로비에 등록'

  # ①
  $a = LoginJoin 'alice' "#alice's_game" $tokA   # ★ 조각 6b — -Auth 가 아니면 토큰이 빈 것 = 아이디만 옛 꼴
  $b = LoginJoin 'bob' "#alice's_game" $tokB
  Check ($a.tok.Length -eq 102 -and $b.tok.Length -eq 102) "① 로그인 → alice 새 방 · bob 그 방 — 토큰 둘($($a.tok.Length) · $($b.tok.Length))"

  # ② — alice 먼저(방장 = 먼저 들어온 사람)
  $ca = NewClient 'alice' $a.tok; $cb = NewClient 'bob' $b.tok
  $clients = @($ca, $cb)
  $ea = Enter $ca
  $eb = Enter $cb
  Check ($ea.joined -and $ea.accepted -and $eb.joined -and $eb.accepted) "② 접속 시도 → JOIN_OK · CONNECT → 수락 + 키 — alice $($ea.joined)/$($ea.accepted) · bob $($eb.joined)/$($eb.accepted)"
  $inGame = Pump { $ca.upd -ge 3 -and $cb.upd -ge 3 } 6
  Check $inGame "② 둘 다 게임 안 — 상태 갱신 alice $($ca.upd) · bob $($cb.upd)"

  # ③
  Chat $ca '!start'
  $playing = Pump { (ReadLog $llog) -match '>> ROOM \d+ 2 \d+ 1 ' } 8
  Check $playing '③ 방장 alice `!start` → 로비 거울에 판 중(ROOM … 사람 2 · 판 중 1)'

  # ④ — 진단 죽임은 첫 사람이 게임에 든 뒤 300틱(약 10초)
  $saved = Pump { (ReadLog $llog) -match '\[판 결과\] \{"match"' } 25
  Check $saved '④ 판 끝 → 로비가 판 결과를 받음([판 결과] 줄)'
  $null = Pump { (ReadLog $llog) -match '<< SAVED \S+ ' } 5   # 로비가 SAVED 를 보낸 것을 보고(저장소 답은 다음 바퀴)
  $null = Pump { $false } 1   # SAVED 가 게임 서버에 닿을 틈
  $a.k.c.Close(); $b.k.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  foreach ($cl in $clients) { try { $cl.u.Close() } catch { } }
  foreach ($x in @(@{ p = $gp; k = 'nox_server' }, @{ p = $lp; k = 'nox_lobby' }, @{ p = $ap; k = 'nox_auth' })) {   # ★ 조각 6b — 로그인 서버도 닫기 이벤트로
    if ($x.p -and -not $x.p.HasExited) {
      try { [void][Threading.EventWaitHandle]::OpenExisting("$($x.k)_stop_$($x.p.Id)").Set() } catch { }
      if (-not $x.p.WaitForExit(15000)) { Check $false "$($x.k) 이 닫기 이벤트 뒤에 끝나지 않음 — 강제 종료"; Stop-Process -Id $x.p.Id -Force }
    }
  }
}

$tl = ReadLog $llog; $tg = ReadLog $glog
$fileLines = @(if (Test-Path -LiteralPath $res) { Get-Content -LiteralPath $res -Encoding UTF8 } else { @() })
$lobbyJson = @(([regex]::Matches($tl, '\[판 결과\] (\{[^\r\n]*)')) | ForEach-Object { $_.Groups[1].Value })   # 줄 끝까지 — `\{.*\}` 는 꼬리 바이트를 못 봄(돌연변이 M4)
Check ($fileLines.Count -eq 1 -and $lobbyJson.Count -eq 1 -and $lobbyJson[0] -eq $fileLines[0]) "게임 서버 결과 파일 줄 = 로비 [판 결과] 줄(파일 $($fileLines.Count) · 로비 $($lobbyJson.Count))"
$j = $null; try { $j = $fileLines[0] | ConvertFrom-Json } catch { }
$accs = @(); if ($j) { $accs = @($j.players | ForEach-Object { $_.account }) }
Check ($j -and $j.endReason -eq 'score_player' -and $j.winner.kind -eq 'player' -and $j.winner.account -eq 'alice' -and ($accs -contains 'alice') -and ($accs -contains 'bob')) `
  "판 결과 — 끝 까닭 $($j.endReason) · 승자 $($j.winner.account) · 사람 $($accs -join ',')"
Check ($tl -match '판 저장 요청 1\(SAVED ok 1 · dup 0 · 실패 0 · 끊긴 뒤 답 0 · 모르는 답 0 · 끝날 때 남음 0\)') '로비 끝 줄 — 판 저장 요청 1(SAVED ok 1)'
Check ($tg -match '판 결과 SAVED\(ok 1 · dup 0 · bad 0 · 모르는 판 0\)' -and $tg -match '쥔 채 끝 0') '게임 서버 끝 줄 — SAVED ok 1 · 쥔 채 끝 0'
if ($Auth) {
  # ★ 로그인 암호 조각 6b — 토큰으로 둘 다 통과 · 아이디만 0 · 시험 DB 마지막 로그인 · 기록 셋에 친 암호 · 토큰 글 0곳
  Check ($tl -match '끝 — 로그인 2\(토큰 \[통과 2 · 크기 0 · 판 0 · 서명 0 · 만료 0 · 만료가 너무 멂 0 · IP 다름 0 · 이름 다름 0\] · 다시 쓴 토큰 0 .* 아이디만 0\) · 옛 접속 끊음 0') `
    '로비 끝 줄 — 로그인 2(토큰 통과 2 · 다른 토큰 갈래 0 · 다시 쓴 토큰 0 · 아이디만 0)'
  $ll = @(Sql "SELECT COUNT(*) FROM accounts WHERE name IN ('alice','bob') AND last_login_unix >= $authStart;")
  Check ($ll.Count -ge 1 -and "$($ll[0])".Trim() -eq '2') "시험 DB accounts — alice · bob 마지막 로그인이 이번 판 시각 뒤($($ll -join ' '))"
  $ta = ReadLog $alog
  $all = $tl + $tg + $ta
  Check ($ta -match '듣기 https' -and $tl.Length -gt 0 -and $tg.Length -gt 0) "기록 셋을 읽음(로비 $($tl.Length) · 게임 서버 $($tg.Length) · 로그인 서버 $($ta.Length) 글자 — 아래 0곳 검사가 빈 것을 보지 않게)"
  Check (-not $all.Contains($pwA) -and -not $all.Contains($pwB) -and -not $all.Contains([uri]::EscapeDataString($pwA))) '기록 셋에 친 암호 글자 0곳'
  Check (-not ($all -match 'NXL1[0-9a-f]{20}')) '기록 셋에 로그인 토큰 글 0곳'
} else {
  Check ($tl -match '끝 — 로그인 2\(토큰 \[[^\]1-9]*\] .* 아이디만 2\) · 옛 접속 끊음 0') '로비 끝 줄 — 로그인 2(아이디만 2 · 토큰 갈래 0 — ★ 로그인 암호 조각 4)'
}

if ($Db) {
  # DB 행 — 판 번호로 찾음(시험마다 실행 번호가 달라 겹치지 않는다)
  # [db] 칸을 로비와 같은 설정 파일에서(비밀번호는 찍지 않는다 — 환경 변수로만 넘김)
  $dbc = @{ host = '127.0.0.1'; port = '3306'; database = 'nox_mo' }; $sec = ''
  foreach ($ln in [IO.File]::ReadAllLines($ini, (New-Object Text.UTF8Encoding($false)))) {
    $tx = $ln.Trim()
    if ($tx -match '^\[(.+)\]$') { $sec = $Matches[1]; continue }
    if ($sec -eq 'db' -and $tx -match '^([^=;#]+?)\s*=\s*(.*)$') { $dbc[$Matches[1]] = $Matches[2].Trim() }
  }
  $env:MYSQL_PWD = $dbc['password']
  $mysql = 'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
  $mk = if ($j) { $j.match } else { '' }
  $dn = $dbc['database']
  $rows = & $mysql -u $dbc['user'] -h $dbc['host'] -P $dbc['port'] --protocol=TCP -N -B -e "SELECT COUNT(*) FROM $dn.matches WHERE match_key = '$mk'; SELECT COUNT(*), SUM(account_id IS NOT NULL) FROM $dn.match_players p JOIN $dn.matches m ON m.id = p.match_id WHERE m.match_key = '$mk'; SELECT COUNT(*) FROM $dn.accounts WHERE name IN ('alice','bob')" 2>&1
  Remove-Item Env:MYSQL_PWD
  $rows = @($rows | ForEach-Object { $_.ToString() })
  Check ($rows.Count -ge 3 -and $rows[0] -eq '1' -and $rows[1] -match '^2\s+2$' -and $rows[2] -eq '2') "DB — 판 한 행 · 사람 두 행(계정 번호 둘) · 계정 둘($($rows -join ' | '))"
}

Write-Host ("걸린 시간 {0:N0}초" -f ((Get-Date) - $started).TotalSeconds)
if (-not $KeepLogs -and -not $script:fails) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue } else { Write-Host "기록: $tmp" }   # 실패하면 기록을 남긴다(까닭을 볼 수 있게)
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
