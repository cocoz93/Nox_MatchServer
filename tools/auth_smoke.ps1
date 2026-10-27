# auth_smoke.ps1 — 로그인 서버 nox_auth 를 진짜로 띄워 HTTPS 로 가입 · 로그인(로그인 암호 조각 2 · Docs/LOGIN_AUTH_DESIGN.md).
#
# 시험 DB(`[db] test_database` — 보통 nox_mo_test)에 표를 새로 깔고(001 + 002 · 로비 사용자 nox_mo 가 그 DB 만 표를 만든다)
# nox_auth --test-db 를 8443 에 띄운다(인증서 묶음 · 주소 예약이 8443 에만 있다 — tools/auth_setup.ps1). 브라우저처럼 인증서 검사를 켠 채로 붙는다.
#   ① 가입 — 만듦 201 · 같은 이름 409 · 이름 규칙 400 · 확인 다름 400 · 옛 행(암호 NULL)에 암호 201
#   ② 로그인 — 됨 200 `NXL1<16진 114>` · 암호 틀림 401 `NXL!wrong_password` · 계정 없음 401 · 규칙 밖 400 · GET 405
#   ③ DB 행 — 소금 16 · 해시 32 · 반복 600000 · 마지막 로그인 적힘 · 옛 행 번호 그대로
#   ④ 기록 — 친 암호 글자 0곳 · 끝 줄 요청 수 = 보낸 수 · 닫기 이벤트로 끝남(종료 코드 0)
# 사람 1회 전(인증서 · nox_auth DB 사용자 없음)이면 종료 코드 77 = ctest 「건너뜀」
param([string]$Exe = '', [string]$Config = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $repo 'build\Release\nox_auth.exe' }
if (-not $Config) { $Config = Join-Path $repo 'config\nox_mo.ini' }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}
function Skip($why) { Write-Host "[건너뜀] $why"; exit 77 }

# 설정 — INI 한 구역의 값(파일은 UTF-8 · BOM 없음)
function IniGet($section, $key) {
  $cur = ''
  foreach ($l in [IO.File]::ReadAllLines($Config)) {
    $t = $l.Trim()
    if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
    if ($cur -eq $section -and $t -match "^$key\s*=\s*(.*)$") { return $Matches[1].Trim() }
  }
  return ''
}
if (-not (Test-Path -LiteralPath $Config)) { Skip "설정 파일 없음 $Config" }
if (-not (IniGet 'auth_db' 'user')) { Skip '[auth_db] user 없음(config/auth_setup.sql — 사람 1회)' }
$mysql = 'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
if (-not (Test-Path -LiteralPath $mysql)) { Skip "mysql.exe 없음 $mysql" }
$dbHost = if (IniGet 'db' 'host') { IniGet 'db' 'host' } else { '127.0.0.1' }
$testDb = if (IniGet 'db' 'test_database') { IniGet 'db' 'test_database' } else { 'nox_mo_test' }

# 시험 DB 에 SQL 을 로비 사용자로(그 DB 만 표 권한) — 암호는 환경 변수로만 넘김(명령 줄 · 기록에 안 남게)
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
Sql "INSERT INTO accounts (name, created_unix, last_login_unix) VALUES ('legacy1', 1, 1);" | Out-Null
$legacyId = [int](Sql "SELECT id FROM accounts WHERE name = 'legacy1';")

$log = Join-Path $env:TEMP ("auth_smoke_{0}.txt" -f $PID)
$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--config', "`"$Config`"", '--test-db', '--workers', '2', '--seconds', '60')
$null = $p.Handle  # 핸들을 미리 잡아야 끝난 뒤 ExitCode 가 비지 않음
$listening = $false
for ($i = 0; $i -lt 150 -and -not $p.HasExited; $i++) {
  Start-Sleep -Milliseconds 100
  if ((Test-Path $log) -and (Select-String -LiteralPath $log -Pattern '듣기 https' -Quiet -Encoding UTF8)) { $listening = $true; break }
}
if (-not $listening) {
  $text = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  Write-Host $text
  if ($text -match '사람 1회|1045') { Skip 'nox_auth 가 사람 1회 전 까닭으로 안 뜸(위 줄)' }
  Write-Host '[실패] nox_auth 가 안 뜸'; exit 1
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$base = 'https://127.0.0.1:8443'
$script:sent = 0
function Req([string]$method, [string]$path, [string]$body) {
  $script:sent++
  $sw = [Diagnostics.Stopwatch]::StartNew()
  try {
    if ($method -eq 'POST') { $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri ($base + $path) -Body $body -ContentType 'application/x-www-form-urlencoded' }
    else { $r = Invoke-WebRequest -UseBasicParsing -Method $method -Uri ($base + $path) }
    return @{ status = [int]$r.StatusCode; body = [string]$r.Content; ms = $sw.ElapsedMilliseconds }
  } catch [System.Net.WebException] {
    $resp = $_.Exception.Response
    if (-not $resp) { return @{ status = 0; body = $_.Exception.Message; ms = $sw.ElapsedMilliseconds } }
    # PowerShell 5.1 은 오류 응답 본문을 ErrorDetails 로 먼저 읽어 가 스트림이 비어 있음
    $text = if ($_.ErrorDetails) { $_.ErrorDetails.Message } else { (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
    return @{ status = [int]$resp.StatusCode; body = $text; ms = $sw.ElapsedMilliseconds }
  }
}
$pw = 'Sm0k!p12'
$pwLegacy = 'Leg4cyPw'
try {
  Write-Host '① 가입'
  $r = Req 'POST' '/signup' "name=smoke1&password=$([uri]::EscapeDataString($pw))&password2=$([uri]::EscapeDataString($pw))"
  Check ($r.status -eq 201) "만듦 201 ($($r.status) $($r.body) · $($r.ms) ms — 해시 한 번 포함)"
  $r = Req 'POST' '/signup' "name=smoke1&password=other123&password2=other123"
  Check ($r.status -eq 409) "같은 이름 409 ($($r.status))"
  $r = Req 'POST' '/signup' "name=smoke_1&password=ab12&password2=ab12"
  Check ($r.status -eq 400) "이름 규칙 400 ($($r.status) $($r.body))"
  $r = Req 'POST' '/signup' "name=smoke2&password=ab12cd&password2=ab12ce"
  Check ($r.status -eq 400) "확인 다름 400 ($($r.status))"
  $r = Req 'POST' '/signup' "name=legacy1&password=$pwLegacy&password2=$pwLegacy"
  Check ($r.status -eq 201 -and $r.body.Contains('예전 이 이름의 기록')) "옛 행에 암호 201 · 결과 화면 ($($r.status))"
  $now = [int](Sql "SELECT id FROM accounts WHERE name = 'legacy1' AND pass_hash IS NOT NULL;")
  Check ($now -eq $legacyId) "옛 행 번호 그대로 · 암호 들어감 ($now = $legacyId)"
  $r = Req 'GET' '/signup' ''
  Check ($r.status -eq 200 -and $r.body.Contains('<form method="post" action="/signup"')) "GET /signup 양식 200 ($($r.status))"

  Write-Host '② 로그인'
  $r = Req 'POST' '/login' "name=smoke1&password=$([uri]::EscapeDataString($pw))"
  Check ($r.status -eq 200 -and $r.body -cmatch '^NXL1[0-9a-f]{114}$') "됨 200 NXL1 + 16진 114자 ($($r.status) · 길이 $($r.body.Length) · $($r.ms) ms)"
  $r = Req 'POST' '/login' "name=legacy1&password=$pwLegacy"
  Check ($r.status -eq 200 -and $r.body.StartsWith('NXL1')) "옛 행 계정 로그인 200 ($($r.status))"
  $r = Req 'POST' '/login' "name=smoke1&password=Sm0k!p13"
  Check ($r.status -eq 401 -and $r.body -ceq 'NXL!wrong_password') "암호 틀림 401 ($($r.status) $($r.body))"
  $r = Req 'POST' '/login' "name=nobody9&password=ab12cd"
  Check ($r.status -eq 401 -and $r.body -ceq 'NXL!no_account') "계정 없음 401 ($($r.status) $($r.body))"
  $r = Req 'POST' '/login' "name=smoke1&password=ab1"
  Check ($r.status -eq 400 -and $r.body -ceq 'NXL!bad_input') "규칙 밖 400 ($($r.status) $($r.body))"
  $r = Req 'GET' '/login' ''
  Check ($r.status -eq 405) "GET /login 405 ($($r.status))"
  $r = Req 'POST' '/nope' ''
  Check ($r.status -eq 404) "없는 길 404 ($($r.status))"

  Write-Host '③ DB 행'
  $row = (Sql "SELECT LENGTH(pass_salt), LENGTH(pass_hash), pass_iter, last_login_unix > 1 FROM accounts WHERE name = 'smoke1';") -split "`t"
  Check ($row[0] -eq '16' -and $row[1] -eq '32' -and $row[2] -eq '600000' -and $row[3] -eq '1') "smoke1 — 소금 16 · 해시 32 · 반복 600000 · 마지막 로그인 적힘 ($($row -join ' '))"
  $n = Sql "SELECT COUNT(*) FROM accounts;"
  Check ($n -eq '2') "행 둘(smoke1 · legacy1 — 실패한 가입은 행 없음) ($n)"
} finally {
  try { ([Threading.EventWaitHandle]::OpenExisting("nox_auth_stop_$($p.Id)")).Set() | Out-Null } catch { }
  if (-not $p.WaitForExit(10000)) { Stop-Process -Id $p.Id -Force; Write-Host '  [실패] 닫기 이벤트에 안 끝남(강제 종료)'; $script:fails++ }
}
Write-Host '④ 기록'
$text = [IO.File]::ReadAllText($log)
Check ($p.ExitCode -eq 0) "종료 코드 0 ($($p.ExitCode))"
Check (-not $text.Contains($pw) -and -not $text.Contains($pwLegacy) -and -not $text.Contains('Sm0k%21')) '기록에 친 암호 글자 0곳'
Check (-not ($text -match 'NXL1[0-9a-f]{20}')) '기록에 토큰 0곳'
$m = [regex]::Match($text, '끝 — 요청 (\d+)\(')
Check ($m.Success -and [int]$m.Groups[1].Value -eq $script:sent) "끝 줄 요청 $($m.Groups[1].Value) = 보낸 $($script:sent)"
$h = [regex]::Match($text, '한 번 = ([\d.]+) ms')
Write-Host "  (해시 한 번 $($h.Groups[1].Value) ms — 정할 것 f · 1초 아래면 그대로)"
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
