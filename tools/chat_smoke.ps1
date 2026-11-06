# chat_smoke.ps1 — 귓속말 채팅 서버(nox_chat) 소켓 왕복 (`Docs/MO_WHISPER_PLAN_1105.md` 8절 chat_smoke 줄).
#
# 판단은 chat_test 가 본다. 여기서는 실행 파일 · 소켓 · 설정 읽기 · 끊겼다 다시 붙기만 본다(DB 없음 — 정지 읽기는 ban_smoke 꼴의 실물 몫).
#   ① 토큰 판 — 설정의 [auth] key 로 서명한 로그인 토큰(LoginToken.h 57바이트 · HMAC-SHA256 앞 16)이 AUTHOK · 같은 토큰 다시 = AUTHNO replay · 끊김
#   ② 이름 판(--no-auth) — 둘이 붙어 한글 귓속말 F · TO · 없는 사람 NOUSER · PING/PONG · 받는 쪽이 끊었다 다시 붙기 · 같은 이름 새 연결 = 옛 연결 KICKED ·
#      30초 조용한 연결에 서버 PING(실행 파일이 Tick 을 부르나 — 이 판만 40초)
#   ③ 서버 기록 — 귓속말 내용이 안 찍힘 · 토큰이 안 찍힘 · 끝 줄 CHATSTATS 의 전달 · 같은 이름 · 다시 쓴 토큰 셈
#   ④ -Db 면 ①~③ 대신 정지 읽기(시험 DB · config\nox_mo.ini [db] · ctest chat_smoke_db) — 정지 행을 넣으면 5초 주기 안에 그 이름을 끊음 ·
#      다시 로그인 = AUTHNO banned · 풀면 다음 읽기에 들어옴 · 닫기 이벤트(nox_chat_stop_<pid>)로 끝. 설정 · DB 가 없으면 77 = 건너뜀
param([int]$Port = 14020, [int]$Port2 = 14021, [int]$Port3 = 14022, [string]$Exe = '', [switch]$Db)
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\nox_chat.exe' }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}
$utf8 = New-Object Text.UTF8Encoding($false)
function Open($port) {
  for ($i = 0; $i -lt 40; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient
      $c.Connect('127.0.0.1', $port)
      $s = $c.GetStream(); $s.ReadTimeout = 3000
      $w = New-Object System.IO.StreamWriter($s, $utf8); $w.NewLine = "`r`n"
      return @{ c = $c; r = (New-Object System.IO.StreamReader($s, $utf8)); w = $w }
    } catch { Start-Sleep -Milliseconds 100 }
  }
  throw "접속 실패: 127.0.0.1:$port"
}
function Send($k, [string]$line) { $k.w.WriteLine($line); $k.w.Flush() }
function Recv($k) { try { return $k.r.ReadLine() } catch { return '(시간 초과)' } }

# 로그인 토큰 — LoginToken.cpp PackBody 와 같은 차례(작은 끝수 · IP 는 a.b.c.d 차례 · 이름 16바이트 0 채움)
function Token([byte[]]$key, [string]$name, [uint64]$nonce) {
  $b = New-Object byte[] 57
  $b[0] = 1
  [BitConverter]::GetBytes([uint64]7).CopyTo($b, 1)
  $b[9] = 127; $b[10] = 0; $b[11] = 0; $b[12] = 1
  $exp = [uint32]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 60)
  [BitConverter]::GetBytes($exp).CopyTo($b, 13)
  [BitConverter]::GetBytes($nonce).CopyTo($b, 17)
  [Text.Encoding]::ASCII.GetBytes($name).CopyTo($b, 25)
  $h = New-Object Security.Cryptography.HMACSHA256(, $key)
  $mac = $h.ComputeHash($b, 0, 41)
  [Array]::Copy($mac, 0, $b, 41, 16)
  return -join ($b | ForEach-Object { $_.ToString('x2') })
}

if ($Db) {
  $repo = Split-Path -Parent $PSScriptRoot
  $cfg = Join-Path $repo 'config\nox_mo.ini'
  function Skip($why) { Write-Host "[건너뜀] $why"; exit 77 }
  if (-not (Test-Path -LiteralPath $cfg)) { Skip "설정 파일 없음 $cfg" }
  function IniGet($section, $key) {   # ban_smoke.ps1 과 같은 읽기
    $cur = ''
    foreach ($l in [IO.File]::ReadAllLines($cfg)) {
      $t = $l.Trim()
      if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
      if ($cur -eq $section -and $t -match "^$key\s*=\s*(.*)$") { return $Matches[1].Trim() }
    }
    return ''
  }
  if (-not (IniGet 'db' 'user')) { Skip '[db] user 없음' }
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
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  Sql "INSERT INTO accounts (name, created_unix, last_login_unix) VALUES ('qa1', $now, $now), ('qa2', $now, $now);" | Out-Null
  $log3 = Join-Path $env:TEMP ("chat_smoke_{0}_db.txt" -f $PID)
  $p3 = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log3 -ArgumentList @(
    '--config', "`"$cfg`"", '--test-db', '--no-auth', '--bind', '127.0.0.1', '--port', $Port3, '--seconds', '60')
  $null = $p3.Handle
  $stopped = $false
  try {
    Write-Host "④ 정지 읽기 — 시험 DB $testDb"
    $x = Open $Port3; Send $x 'AUTH qa1'; Check ((Recv $x) -eq 'AUTHOK') 'qa1 AUTHOK'
    $y = Open $Port3; Send $y 'AUTH qa2'; Check ((Recv $y) -eq 'AUTHOK') 'qa2 AUTHOK'
    $t0 = Get-Date
    Sql "INSERT INTO account_bans (account_id, until_unix, reason, source, created_unix) SELECT id, $($now + 86400), 'smoke', 'smoke', $now FROM accounts WHERE name = 'qa2';" | Out-Null
    $y.c.GetStream().ReadTimeout = 9000   # 5초 주기 + 묻는 시간을 기다린다(시간 초과 뒤 같은 소켓을 다시 읽지 않게 한 번에)
    $l = Recv $y
    $sec = ((Get-Date) - $t0).TotalSeconds
    Check ($null -eq $l -and $sec -lt 7.5) ("정지 행을 넣고 {0:N1}초에 qa2 연결이 줄 없이 끊김(5초 주기 안 · 받은 것 '{1}')" -f $sec, $l)
    Send $x 'PING'; Check ((Recv $x) -eq 'PONG') 'qa1 연결은 그대로'
    $z = Open $Port3; Send $z 'AUTH qa2'
    Check ((Recv $z) -eq 'AUTHNO banned') '정지 중 다시 로그인 → AUTHNO banned'
    Check ($null -eq (Recv $z)) 'banned 뒤 끊김'
    Sql "UPDATE account_bans SET lifted_unix = $now WHERE source = 'smoke';" | Out-Null
    Start-Sleep -Milliseconds 6500   # 다음 읽기(5초 주기)가 지나갈 때까지
    $w2 = Open $Port3; Send $w2 'AUTH qa2'; Check ((Recv $w2) -eq 'AUTHOK') '정지를 푼 뒤 다음 읽기에 qa2 AUTHOK'
    $x.c.Close(); $w2.c.Close()
    try { [void][Threading.EventWaitHandle]::OpenExisting("nox_chat_stop_$($p3.Id)").Set(); $stopped = $true } catch { }
  } finally {
    if (-not $p3.WaitForExit(15000)) { Stop-Process -Id $p3.Id -Force }
  }
  $t3 = [IO.File]::ReadAllText($log3, $utf8)
  Check ($stopped -and $p3.ExitCode -eq 0) "닫기 이벤트로 끝 · 종료 코드 0 ($stopped · $($p3.ExitCode))"
  Check ($t3 -match '정지 읽기 = DB') '기록 — 정지 읽기 = DB 줄'
  $m3 = [regex]::Match($t3, 'CHATSTATS .* bans=(\d+) banfail=(\d+) outcome=([0-9,]+)')
  Check $m3.Success '끝 줄 CHATSTATS'
  if ($m3.Success) {
    $o3 = $m3.Groups[3].Value.Split(',')
    # 1 AuthNoAuth · 5 AuthBanned · 18 BanClosed
    $reads = [int]$m3.Groups[1].Value   # 판 길이 약 12~16초 ÷ 5초 + 처음 한 번 = 3~4 · 7 넘으면 주기를 안 지킴(쉬지 않고 물음)
    Check ($reads -ge 3 -and $reads -le 7 -and $m3.Groups[2].Value -eq '0') "정지 읽기 $reads번(3~7) · 못 읽음 $($m3.Groups[2].Value)"
    Check ($o3[1] -eq '3' -and $o3[5] -eq '1' -and $o3[18] -eq '1') "로그인 3 · 정지 중 로그인 1 · 정지로 끊음 1 ($($m3.Groups[3].Value))"
  }
  if (-not $script:fails) { Remove-Item -LiteralPath $log3 -ErrorAction SilentlyContinue } else { Write-Host "기록: $log3" }
  Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
  exit $(if ($script:fails) { 1 } else { 0 })
}

$key = New-Object byte[] 32; (New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($key)
$hex = -join ($key | ForEach-Object { $_.ToString('x2') })
$ini = Join-Path $env:TEMP ("chat_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[auth]`r`nkey = $hex`r`n")
$log1 = Join-Path $env:TEMP ("chat_smoke_{0}_a.txt" -f $PID)
$log2 = Join-Path $env:TEMP ("chat_smoke_{0}_b.txt" -f $PID)
$p1 = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log1 -ArgumentList @(
  '--config', "`"$ini`"", '--no-db', '--bind', '127.0.0.1', '--port', $Port, '--seconds', '8')
$p2 = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log2 -ArgumentList @(
  '--config', "`"$ini`"", '--no-db', '--no-auth', '--bind', '127.0.0.1', '--port', $Port2, '--seconds', '40')   # 30초 PING 을 기다린다
$null = $p1.Handle; $null = $p2.Handle   # 핸들을 먼저 잡아 둬야 끝난 뒤 ExitCode 가 읽힌다(안 잡으면 빈 값)
try {
  Write-Host '① 토큰 판'
  $tok = Token $key 'qa1' 77
  $a = Open $Port
  Send $a "AUTH qa1 $tok"
  Check ((Recv $a) -eq 'AUTHOK') '맞는 토큰 → AUTHOK(설정 [auth] key 를 읽음)'
  $b = Open $Port
  Send $b "AUTH qa1 $tok"
  Check ((Recv $b) -eq 'AUTHNO replay') '같은 토큰 다시 → AUTHNO replay'
  Check ($null -eq (Recv $b)) 'replay 뒤 서버가 끊음'
  $wrong = $key.Clone(); $wrong[0] = $wrong[0] -bxor 0x55
  $c = Open $Port
  Send $c ("AUTH qa2 " + (Token $wrong 'qa2' 78))
  Check ((Recv $c) -eq 'AUTHNO token') '다른 키로 서명 → AUTHNO token'
  $a.c.Close(); $b.c.Close(); $c.c.Close()

  Write-Host '② 이름 판 — 귓속말 · 다시 붙기 · 같은 이름 · 조용한 연결에 PING'
  $q = Open $Port2; Send $q 'AUTH qa9'; Check ((Recv $q) -eq 'AUTHOK') 'qa9 AUTHOK(끝까지 조용히 둠)'
  $tq = Get-Date
  $x = Open $Port2; Send $x 'AUTH qa1'; Check ((Recv $x) -eq 'AUTHOK') 'qa1 AUTHOK'
  $y = Open $Port2; Send $y 'AUTH qa2'; Check ((Recv $y) -eq 'AUTHOK') 'qa2 AUTHOK'
  Send $x 'W qa2 안녕 하세요'
  Check ((Recv $y) -eq 'F qa1 안녕 하세요') '받는 쪽 F — 한글 · 빈칸 그대로 · 보낸이는 서버가 찍음'
  Check ((Recv $x) -eq 'TO qa2 안녕 하세요') '보낸 쪽 TO'
  Send $x 'W nobody hi'
  Check ((Recv $x) -eq 'NOUSER nobody') '없는 사람 → NOUSER'
  Send $x 'PING'
  Check ((Recv $x) -eq 'PONG') 'PING → PONG'
  $y.c.Close()
  Start-Sleep -Milliseconds 300
  Send $x 'W qa2 hi'
  Check ((Recv $x) -eq 'NOUSER qa2') '받는 쪽이 끊은 뒤 → NOUSER'
  $y2 = Open $Port2; Send $y2 'AUTH qa2'; Check ((Recv $y2) -eq 'AUTHOK') 'qa2 다시 붙음'
  Start-Sleep -Milliseconds 5100   # 보낸 사람당 5초 3줄 — 위 셋(안녕 · nobody · hi)이 빠질 때까지
  Send $x 'W qa2 again'
  Check ((Recv $y2) -eq 'F qa1 again') '다시 붙은 연결로 F'
  Check ((Recv $x) -eq 'TO qa2 again') '보낸 쪽 TO'
  $x2 = Open $Port2; Send $x2 'AUTH qa1'
  Check ((Recv $x) -eq 'KICKED') '같은 이름 새 연결 → 옛 연결 KICKED'
  Check ($null -eq (Recv $x)) 'KICKED 뒤 옛 연결 끊김'
  Check ((Recv $x2) -eq 'AUTHOK') '새 연결 AUTHOK'
  $x2.c.Close(); $y2.c.Close()
  # 서버 Tick — 30초 아무것도 안 보낸 연결에 PING(kChatPingMs · Tick 은 1초마다). 90초 끊김은 chat_test 시각표만(스모크가 너무 길어짐)
  $q.c.GetStream().ReadTimeout = 34000
  $lq = Recv $q
  $sq = ((Get-Date) - $tq).TotalSeconds
  Check ($lq -eq 'PING' -and $sq -ge 29.5 -and $sq -lt 33) ("조용한 연결에 {0:N1}초에 서버 PING(30초 · 받은 것 '{1}')" -f $sq, $lq)
  Send $q 'PONG'; Start-Sleep -Milliseconds 300; $q.c.Close()
} finally {
  foreach ($p in @($p1, $p2)) { if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force } }
}
Write-Host '③ 서버 기록'
$t1 = [IO.File]::ReadAllText($log1, $utf8); $t2 = [IO.File]::ReadAllText($log2, $utf8)
Check ($p1.ExitCode -eq 0 -and $p2.ExitCode -eq 0) "종료 코드 0 ($($p1.ExitCode) · $($p2.ExitCode))"
Check (-not $t1.Contains($tok)) '토큰 판 기록에 토큰이 안 찍힘'
Check (-not $t2.Contains('안녕') -and -not $t2.Contains('again')) '이름 판 기록에 귓속말 내용이 안 찍힘'
# CHATSTATS outcome = ChatOutcome 차례(ChatHub.h) — 0 AuthOk · 1 AuthNoAuth · 3 AuthToken · 4 AuthReplay · 7 Replaced · 8 Sent · 9 NoUser · 14 PingAnswered
$m1 = [regex]::Match($t1, 'CHATSTATS .* outcome=([0-9,]+)'); $m2 = [regex]::Match($t2, 'CHATSTATS .* outcome=([0-9,]+)')
Check ($m1.Success -and $m2.Success) '끝 줄 CHATSTATS 둘'
if ($m1.Success -and $m2.Success) {
  $o1 = $m1.Groups[1].Value.Split(','); $o2 = $m2.Groups[1].Value.Split(',')
  Check ($o1.Count -eq 19 -and $o2.Count -eq 19) '갈래 19칸'
  Check ($o1[0] -eq '1' -and $o1[3] -eq '1' -and $o1[4] -eq '1') "토큰 판 — 로그인 1 · 토큰 거절 1 · 다시 쓴 토큰 1 ($($m1.Groups[1].Value))"
  Check ($o2[1] -eq '5' -and $o2[7] -eq '1' -and $o2[8] -eq '2' -and $o2[9] -eq '2' -and $o2[14] -eq '1' -and $o2[15] -eq '1') "이름 판 — 로그인 5 · 같은 이름 1 · 전달 2 · 없는 사람 2 · PING 1 · PONG 1 ($($m2.Groups[1].Value))"
}
if (-not $script:fails) { Remove-Item -LiteralPath $ini, $log1, $log2 -ErrorAction SilentlyContinue } else { Write-Host "기록: $log1 · $log2" }   # ban_smoke 와 같게 — 실패면 남김
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
