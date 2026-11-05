# ban_smoke.ps1 — 즉시 차단 소켓 왕복(플랜1 조각 4 · Docs/MO_ACCOUNT_RANK_MAP_PLAN_1105.md 6절).
#
# 시험 DB · 로그인 서버 · 로비(DB 저장소) · 게임 서버 둘(A 127.0.0.1 · B 127.0.0.2 — 같은 UDP 포트)
#   ① alice · bob 토큰 로그인 · 게임 서버 둘 등록
#   ② 운영 명령 --ban alice(다른 프로세스) → 7초 안에 alice 접속에 끊는 줄 + 끊김 · bob 접속은 그대로
#   ③ 게임 서버 둘 다 KICK 을 받음(그 이름이 없는 서버라 내보낸 세션 0개 — 판 안 내보내기는 proto_test ⑤ · 실물 판)
#   ④ 정지 전에 받아 둔 토큰 → 로비가 거절(378 · 일회용 번호 소비) · 새 토큰 요청은 로그인 서버가 403
#   ⑤ --unban → 다음 읽기 뒤 새 토큰으로 들어옴
#   ⑥ 끝 줄 — 못 읽음 0 · 내보낸 이름 1 · 끊음 1 · KICK 2 · 거절 1
# 끝은 모두 닫기 이벤트 — 강제 종료는 응답이 없을 때만
param([int]$LobbyPort = 14420, [int]$LinkPort = 14430, [int]$UdpPort = 18621, [string]$Exe = '', [string]$LobbyExe = '', [string]$AuthExe = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\Release'
if (-not $Exe) { $Exe = Join-Path $bin 'nox_server.exe' }
if (-not $LobbyExe) { $LobbyExe = Join-Path $bin 'nox_lobby.exe' }
if (-not $AuthExe) { $AuthExe = Join-Path $bin 'nox_auth.exe' }
$ini = Join-Path $repo 'config\nox_mo.ini'
function Skip($why) { Write-Host "[건너뜀] $why"; exit 77 }
if (-not (Test-Path -LiteralPath $ini)) { Skip "설정 파일 없음 $ini" }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}
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
if (-not (IniGet 'auth_db' 'user')) { Skip '[auth_db] user 없음' }
$mysql = 'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
if (-not (Test-Path -LiteralPath $mysql)) { Skip "mysql.exe 없음 $mysql" }
$dbHost = if (IniGet 'db' 'host') { IniGet 'db' 'host' } else { '127.0.0.1' }
$testDb = if (IniGet 'db' 'test_database') { IniGet 'db' 'test_database' } else { 'nox_mo_test' }
function Sql([string]$text) {   # 시험 DB 에 로비 사용자로(암호는 환경 변수로만)
  $env:MYSQL_PWD = IniGet 'db' 'password'
  try { $out = $text | & $mysql -u (IniGet 'db' 'user') -h $dbHost --default-character-set=utf8mb4 -N -B $testDb 2>&1 }
  finally { Remove-Item Env:\MYSQL_PWD -ErrorAction SilentlyContinue }
  if ($LASTEXITCODE -ne 0) { throw "SQL 실패: $out" }
  return $out
}
$schema = 'DROP TABLE IF EXISTS account_bans, match_totals, login_keys, ratings, match_players, matches, accounts, schema_version;'
foreach ($f in @('001_init', '002_auth', '003_rating', '004_launcher_keys', '005_match_kills', '006_match_totals', '007_account_bans')) {
  $schema += "`n" + [IO.File]::ReadAllText((Join-Path $repo "tools\db\$f.sql"))
}
try { Sql $schema | Out-Null } catch { Skip "시험 DB $testDb 표 깔기 못 함 — $_" }

$latin1 = [Text.Encoding]::GetEncoding(28591)
function Open($port) {
  for ($i = 0; $i -lt 50; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient
      $c.Connect('127.0.0.1', $port)
      $s = $c.GetStream(); $s.ReadTimeout = 3000
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
function WaitLog([string]$path, [string]$pattern, [int]$n, [int]$sec) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $sec) {
    if (([regex]::Matches((ReadLog $path), $pattern)).Count -ge $n) { return $true }
    Start-Sleep -Milliseconds 100
  }
  return $false
}
function StopProc($p, [string]$kind) {
  if ($p.HasExited) { return }
  try { [void][Threading.EventWaitHandle]::OpenExisting("$($kind)_stop_$($p.Id)").Set() } catch { }
  if (-not $p.WaitForExit(15000)) { Check $false "$kind $($p.Id) 이 닫기 이벤트 뒤에 끝나지 않음 — 강제 종료"; Stop-Process -Id $p.Id -Force }
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
function AuthReq([string]$path, [string]$body) {
  try {
    $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri ('https://127.0.0.1:8443' + $path) -Body $body -ContentType 'application/x-www-form-urlencoded'
    return @{ status = [int]$r.StatusCode; body = [string]$r.Content }
  } catch [System.Net.WebException] {
    $resp = $_.Exception.Response
    if (-not $resp) { return @{ status = 0; body = $_.Exception.Message } }
    $text = if ($_.ErrorDetails) { $_.ErrorDetails.Message } else { (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
    return @{ status = [int]$resp.StatusCode; body = $text }
  }
}
function Tool([string[]]$a) {
  $o = & $AuthExe --config $ini --test-db @a 2>&1 | Out-String
  return @{ code = $LASTEXITCODE; text = $o }
}
# 정품 묶음 차례(`LoginUnitTests` bundle) — 받은 줄 전부(376 이나 378 까지)
function LoginBundle([string]$nick, [string]$tok) {
  $k = Open $LobbyPort
  Send $k @('NOXCLIENT 5', 'CVERS 11020 9472', 'PASS supersecret', "NICK $nick", "apgar $tok 0", 'SERIAL 7726', 'USER UserName HostName irc.westwood.com :RealName', 'verchk 32512 720916', 'SETOPT 17,33', 'SETCODEPAGE 949')
  $got = @()
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt 6) {
    try { $l = $k.r.ReadLine() } catch { break }
    if ($null -eq $l) { break }
    $got += $l
    if ((Kind $l) -eq '376' -or (Kind $l) -eq '378') { break }
  }
  return @{ k = $k; lines = $got }
}
function Has($lines, [string]$kind) { return @($lines | Where-Object { (Kind $_) -eq $kind }).Count -ge 1 }

$tag = $PID
$tmp = Join-Path $env:TEMP "bansmoke_$tag"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$al = Join-Path $tmp 'auth.txt'; $ll = Join-Path $tmp 'lobby.txt'; $ga = Join-Path $tmp 'gsA.txt'; $gb = Join-Path $tmp 'gsB.txt'
$ap = Start-Process -FilePath $AuthExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $al -ArgumentList @('--config', "`"$ini`"", '--test-db', '--workers', '2', '--seconds', '120')
$null = $ap.Handle
$up = $false
for ($i = 0; $i -lt 150 -and -not $ap.HasExited; $i++) { Start-Sleep -Milliseconds 100; if ((ReadLog $al) -match '듣기 https') { $up = $true; break } }
if (-not $up) { Write-Host (ReadLog $al); if (-not $ap.HasExited) { StopProc $ap 'nox_auth' }; Write-Host '  [실패] nox_auth 가 안 뜸'; exit 1 }
$lp = Start-Process -FilePath $LobbyExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $ll -ArgumentList @(
  '--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort, '--test-db', '--seconds', '100')
$null = $lp.Handle
function StartGs([string]$ip, [string]$log) {
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
    '--port', $UdpPort, '--bind', $ip, '--register-ip', $ip, '--config', "`"$ini`"", '--lobby', "127.0.0.1:$LinkPort", '--quiet', '--no-results')
  $null = $p.Handle
  return $p
}
$pa = StartGs '127.0.0.1' $ga
$pb = StartGs '127.0.0.2' $gb
$pwA = 'Al1ce!pw'; $pwB = 'B0bpass9'
$held = @()
try {
  Write-Host "로비 :$LobbyPort · 연결 :$LinkPort · 게임 서버 A 127.0.0.1:$UdpPort · B 127.0.0.2:$UdpPort · DB $testDb"
  Check (WaitLog $ll '등록 — 실행' 2 15) '① 게임 서버 둘 등록(로비 기록 둘)'
  $keys = @{}
  foreach ($u in @(@{ n = 'alice'; p = $pwA }, @{ n = 'bob'; p = $pwB })) {
    $e = [uri]::EscapeDataString($u.p)
    $s = AuthReq '/signup' "name=$($u.n)&password=$e&password2=$e"
    $k = AuthReq '/launcher/login' "name=$($u.n)&password=$e&remember=1"
    Check ($s.status -eq 201 -and $k.status -eq 200 -and $k.body -cmatch '^NXK1') "① $($u.n) 가입 $($s.status) · 열쇠 $($k.status)"
    $keys[$u.n] = $k.body
  }
  function NewToken([string]$name) { return (AuthReq '/token' "name=$name&key=$($keys[$name])") }
  $tA = NewToken 'alice'; $tA2 = NewToken 'alice'; $tB = NewToken 'bob'
  Check ($tA.status -eq 200 -and $tA2.status -eq 200 -and $tB.status -eq 200) '① 토큰 셋(alice 둘 — 하나는 정지 뒤에 낼 것)'
  $a = LoginBundle 'alice' $tA.body
  $b = LoginBundle 'bob' $tB.body
  Check ((Has $a.lines '376') -and (Has $b.lines '376')) '① 둘 다 로그인(376)'
  Check (WaitLog $ll '\[로비\] 저장소 = DB' 1 1) '① 로비 저장소 = DB'

  $ban = Tool @('--ban', 'alice', '--days', '1', '--reason', 'smoke')
  Check ($ban.code -eq 0) "② 운영 명령 --ban 종료 코드 $($ban.code)"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $seen = @(); $closed = $false
  while ($sw.Elapsed.TotalSeconds -lt 8 -and -not $closed) {
    try { $l = $a.k.r.ReadLine() } catch { continue }
    if ($null -eq $l) { $closed = $true } else { $seen += $l }
  }
  $took = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
  Check ($closed -and $took -lt 7) "② alice 접속 끊김 ${took}초(읽기 간격 5초)"
  Check (@($seen | Where-Object { $_ -eq 'ERROR :Closing Link (account suspended)' }).Count -eq 1) "② 끊기 전 줄 하나 — $($seen -join ' / ')"
  Send $b.k @('LIST 37 37')
  $list = ReadUntil $b.k '323'
  Check (Has $list '323') '② bob 접속은 그대로(LIST 답)'

  Check ((WaitLog $ga 'KICK alice — 내보낸 세션 0개' 1 3) -and (WaitLog $gb 'KICK alice — 내보낸 세션 0개' 1 3)) '③ 게임 서버 둘 다 KICK 받음(없는 이름 — 세션 0개)'

  $r = LoginBundle 'alice' $tA2.body
  $line378 = @($r.lines | Where-Object { (Kind $_) -eq '378' })
  Check ($line378.Count -eq 1 -and $line378[0] -match 'Account suspended' -and -not (Has $r.lines '376')) "④ 정지 전에 받은 토큰 → 로비 거절 — $($line378 -join ' ')"
  $r.k.c.Close()
  $t403 = NewToken 'alice'
  Check ($t403.status -eq 403 -and $t403.body -match '^NXL!banned') "④ 새 토큰 → 로그인 서버 $($t403.status)"

  $un = Tool @('--unban', 'alice')
  Check ($un.code -eq 0) "⑤ 운영 명령 --unban 종료 코드 $($un.code)"
  Start-Sleep -Milliseconds 6500
  $t2 = NewToken 'alice'
  $a2 = LoginBundle 'alice' $t2.body
  Check ($t2.status -eq 200 -and (Has $a2.lines '376')) "⑤ 푼 뒤 새 토큰 $($t2.status) · 로그인(376)"
  $a2.k.c.Close()
  $b.k.c.Close()
  $a.k.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  foreach ($x in @($pa, $pb)) { StopProc $x 'nox_server' }
  StopProc $lp 'nox_lobby'
  StopProc $ap 'nox_auth'
}
$tl = ReadLog $ll
$okEnd = $false
if ($tl -match '\[로비\] 끝 — 정지 읽기 (\d+)\(읽음 (\d+) · 못 읽음 0 · 늦은 답 0 · 마지막 행 0\) · 내보낸 이름 1\(로비 접속 끊음 1 · KICK 줄 2\) · 정지 로그인 거절 1 ') {
  $okEnd = [int]$Matches[1] -ge 2 -and [int]$Matches[2] -ge 2
}
Check $okEnd '⑥ 끝 줄 — 읽기 2번 이상 · 못 읽음 0 · 내보낸 이름 1 · 끊음 1 · KICK 2 · 로그인 거절 1'
if (-not $okEnd) { Write-Host (($tl -split "`r?`n") | Where-Object { $_ -match '정지' }) }
if (-not $script:fails) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue } else { Write-Host "기록: $tmp" }
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
