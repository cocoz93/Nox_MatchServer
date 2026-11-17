# lobby_series.ps1 — 로비를 옮기기 전 값, 여섯 판을 무인으로 (★ 11-17 · `Docs/MO_LOBBY_MEASURE_PLAN_1117.md` 2절 · 4절 ③)
#
# 판 차례(2절): 120 · 240 · 480 · 960 명 5분 → 960 명 30분 → 960 명 빈도 ×10 5분.
#   조건(3절): lobby_load.ps1 -Measure(PC 검사 · 미커밋 검사 · 코어) · 활동 0.1 · 대화방 5 · 더미 일꾼 2 · 인증 · DB 끔(lobby_load 기본)
#   기록: `_out/lobby_series/<시각>/series.txt` — 머리 줄 + 판마다 곡선 한 줄(M · 로비 바쁨 · 프로세스 CPU · 답 느린 1% · 메모리 …)
# 첫 판은 파일럿 — 더미 바쁨 문턱(-DummyCpuMaxMs)을 아직 모르니 첫 판만 돌고 10 으로 끝난다. 첫 판 곡선 줄을 보고 문턱을 정해 적은 뒤
#   `-From 2 -SeriesDir <같은 폴더> -DummyCpuMaxMs <값> -CpuWhy <근거>` 로 잇는다(3절 「파일럿에서 문턱을 정해 적은 뒤」)
# 판정 넷(판마다 하나 · Get-LobbyVerdict):
#   통과 — 다음 판
#   깨짐 — 끊김 · 거절 · 줄 손실 · 답 없음(lobby_load 의 ① · ③ 확인 실패 · 더미 「거절 알림」). 「옮기기 전 로비가 이 M 에서 깨짐」 = 결과. 멈춤
#   버림 — 더미 바쁨(잼 단계 `CPU 더미` 최대 > 문턱). 답 지연에 더미 몫이 섞여 이 판 값을 못 쓴다. M 이 클수록 더 바쁘니 멈춤
#   미결 — 판을 못 돌렸거나 도구가 어긋남(미커밋 · PC 검사 · 코어 · 커밋이 기준과 다름 · 그 밖의 확인 실패 · 지나간 양 모자람). 멈춤
#   ⚠ 방 참가 거절(471)은 깨짐에 안 넣는다 — 방이 찼다는 게임 규칙의 답이지 로비가 못 받은 것이 아니다. 곡선 줄에 수만 적는다
# 종료 코드: 0 = 여섯 판 통과 · 10 = 파일럿 끝(문턱 정할 차례) · 5 = 깨짐 · 6 = 버림 · 7 = 미결 · 2 = 인자 틀림
param(
  [int]$DummyCpuMaxMs = 0,   # 더미 바쁨 문턱(ms/초 — 더미 프로세스 전체 CPU, `[더미 구간]` 의 `CPU 더미`). 0 = 아직 안 정함 → 첫 판(파일럿)만
  [string]$CpuWhy = '',      # 문턱 근거 한 줄(series.txt 에 남김)
  [int]$From = 1,            # 몇 번째 판부터
  [string]$SeriesDir = '',   # 이어 쓸 기록 폴더(-From 2 이상이면 필수)
  [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$enc = New-Object Text.UTF8Encoding($true)

$plan = @(
  @{ N = 1; People = 120; Seconds = 300; Rate = 1 },    # 파일럿 겸 곡선
  @{ N = 2; People = 240; Seconds = 300; Rate = 1 },
  @{ N = 3; People = 480; Seconds = 300; Rate = 1 },
  @{ N = 4; People = 960; Seconds = 300; Rate = 1 },
  @{ N = 5; People = 960; Seconds = 1800; Rate = 1 },   # 드문 튐 · 오래 쥔 접속
  @{ N = 6; People = 960; Seconds = 300; Rate = 10 }    # 활동량에 반응하나
)

function Read-Lines([string]$path) {
  if (Test-Path -LiteralPath $path) { return @([IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8)) } else { return @() }
}
function Num1([string]$line, [string]$pattern) {
  $m = [regex]::Match($line, $pattern)
  if ($m.Success) { return [double]$m.Groups[1].Value } else { return $null }
}
function Avg($xs) { if (@($xs).Count) { return [math]::Round((@($xs) | Measure-Object -Average).Average, 2) } else { return $null } }
function Max($xs) { if (@($xs).Count) { return (@($xs) | Measure-Object -Maximum).Maximum } else { return $null } }

# 판 폴더 하나를 읽는다 — lobby_load 가 남긴 summary.txt · lobby.log · dummy.log · env.txt
function Read-Run([string]$dir) {
  $sum = Read-Lines (Join-Path $dir 'summary.txt')
  $lob = Read-Lines (Join-Path $dir 'lobby.log')
  $dum = Read-Lines (Join-Path $dir 'dummy.log')
  $envl = Read-Lines (Join-Path $dir 'env.txt')
  $r = @{}
  $r.Fails = @($sum | Where-Object { $_ -match '^\s*\[실패\] ' } | ForEach-Object { $_.Trim().Substring(5) })
  $head = @($sum | Where-Object { $_.StartsWith('[더미 끝] 사람 ') })
  $r.Refused = $null; $r.Reject471 = $null; $r.Relogin = $null
  if ($head.Count -eq 1) {
    $r.Refused = Num1 $head[0] ' · 거절 알림 (\d+)'
    $r.Reject471 = Num1 $head[0] '\(거절 471 (\d+)\)'
    $r.Relogin = Num1 $head[0] ' · 다시 로그인 (\d+)'
  }
  $cl = @($envl | Where-Object { $_.StartsWith('커밋: ') })
  $r.Commit = if ($cl.Count) { $cl[0].Substring(4).Trim() } else { '' }
  # [로비 구간] — 잼 자리 = 접속이 최대의 95% 이상인 초(올림 · 끝 단계를 뺀다 · 다시 로그인으로 잠깐 주는 한두 접속은 남는다)
  $sect = @(foreach ($l in $lob) {
      if ($l -match '\[로비 구간\] 초 \d+ · 바쁨 ([\d.]+)% · 접속 (\d+) · 받은 줄 (\d+) · 보낸 줄 (\d+) · .* · CPU 고리 ([\d.]+) 프로세스 ([\d.]+) · 메모리 개인 ([\d.]+)MB') {
        [pscustomobject]@{ Busy = [double]$Matches[1]; Conn = [int]$Matches[2]; In = [double]$Matches[3]; Out = [double]$Matches[4]
                           Loop = [double]$Matches[5]; Proc = [double]$Matches[6]; Mem = [double]$Matches[7] }
      }
    })
  $r.LobbySec = $sect.Count
  $r.ConnMax = if ($sect.Count) { [int](Max ($sect | ForEach-Object { $_.Conn })) } else { 0 }
  $plat = @($sect | Where-Object { $_.Conn -gt 0 -and $_.Conn -ge 0.95 * $r.ConnMax })
  $r.PlateauSec = $plat.Count
  $r.BusyAvg = Avg ($plat | ForEach-Object { $_.Busy }); $r.BusyMax = Max ($plat | ForEach-Object { $_.Busy })
  $r.LoopAvg = Avg ($plat | ForEach-Object { $_.Loop }); $r.ProcAvg = Avg ($plat | ForEach-Object { $_.Proc }); $r.ProcMax = Max ($plat | ForEach-Object { $_.Proc })
  $r.InAvg = Avg ($plat | ForEach-Object { $_.In }); $r.OutAvg = Avg ($plat | ForEach-Object { $_.Out })
  $r.MemMax = Max ($sect | ForEach-Object { $_.Mem })
  # [더미 구간] — 잼 단계만. CPU 더미 = 그 1초 동안 더미 프로세스 CPU(ms · 타이머 눈금이라 0 · 16 · 31 …)
  $jam = @(foreach ($l in $dum) { if ($l -match '^\[더미 구간\] 초 [\d.]+ · 단계 잼 · .* · CPU 더미 ([\d.]+)$') { [double]$Matches[1] } })
  $r.JamSec = $jam.Count
  $r.DummyCpuAvg = Avg $jam; $r.DummyCpuMax = Max $jam
  $r.DummyCpuNonzero = @($jam | Where-Object { $_ -gt 0 }).Count
  $r.LatN = $null; $r.LatP50 = $null; $r.LatP99 = $null; $r.LatMax = $null
  $lat = @($sum | Where-Object { $_.StartsWith('[더미 끝] 답(잼 구간) — ') })
  if ($lat.Count -eq 1 -and $lat[0] -match '모두 (\d+)\(가운데 ([\d.]+) · 느린 1% ([\d.]+) · 최대 ([\d.]+)\)') {
    $r.LatN = [int]$Matches[1]; $r.LatP50 = [double]$Matches[2]; $r.LatP99 = [double]$Matches[3]; $r.LatMax = [double]$Matches[4]
  }
  return $r
}

# 판정 넷 — 머리 주석. 차례: 못 돌린 판 → 커밋 → 더미 바쁨 → 깨짐 → 도구 어긋남 → 지나간 양
function Get-LobbyVerdict([int]$rc, $r, [int]$cpuLimit, [int]$wantSec, [string]$baseCommit) {
  if ($rc -eq 2) { return @{ Kind = '미결'; Why = '미커밋 — 판을 안 돌림' } }
  if ($rc -eq 3) { return @{ Kind = '미결'; Why = 'PC 검사 실패 — 판을 안 돌림' } }
  if ($rc -eq 4) { return @{ Kind = '미결'; Why = '코어 지정 다름' } }
  if ($baseCommit -and $r.Commit -ne $baseCommit) { return @{ Kind = '미결'; Why = "커밋 $($r.Commit) 이 기준 $baseCommit 과 다름" } }
  $brk = @($r.Fails | Where-Object { $_ -match '^(① |③ )' })
  $other = @($r.Fails | Where-Object { $_ -notmatch '^(① |③ )' })
  if ($null -ne $r.Refused -and $r.Refused -gt 0) { $brk += "거절 알림 $($r.Refused)" }
  if ($cpuLimit -gt 0 -and $null -ne $r.DummyCpuMax -and $r.DummyCpuMax -gt $cpuLimit) {
    $also = @($brk + $other)
    return @{ Kind = '버림'; Why = "더미 바쁨 — 잼 CPU 더미 최대 $($r.DummyCpuMax)ms/초 > 문턱 $cpuLimit$(if ($also.Count) { ' · 같이 난 것: ' + ($also -join ' | ') })" }
  }
  if ($brk.Count) { return @{ Kind = '깨짐'; Why = ($brk -join ' | ') } }
  if ($other.Count) { return @{ Kind = '미결'; Why = '도구 어긋남 — ' + ($other -join ' | ') } }
  $short = @()
  if ($null -eq $r.Refused) { $short += '더미 끝 머리 줄 없음' }
  if ($r.JamSec -lt $wantSec - 2) { $short += "더미 잼 $($r.JamSec)초 < $($wantSec - 2)" }
  if ($r.PlateauSec -lt [math]::Floor($wantSec * 0.9)) { $short += "로비 잼 자리 $($r.PlateauSec)초 < $([math]::Floor($wantSec * 0.9))" }
  if ($r.DummyCpuNonzero -eq 0) { $short += '더미 CPU 가 잼 내내 0(못 읽음과 못 가름)' }
  if ($null -eq $r.LatP99) { $short += '답(잼 구간) 줄 없음' } elseif ($r.LatN -eq 0) { $short += '답(잼 구간) 0개 — 느린 1% 0 은 값이 아님' }
  if ($short.Count) { return @{ Kind = '미결'; Why = '지나간 양 모자람 — ' + ($short -join ' | ') } }
  return @{ Kind = '통과'; Why = '' }
}

function Curve($spec, $v, $r, [string]$tag) {
  return ("판 {0} · M {1} · 배율 {2} · {3}초 · {4}{5} · 로비 바쁨 평균 {6}% 최대 {7}% · CPU 고리 평균 {8} 프로세스 평균 {9} 최대 {10}(ms/초) · 메모리 개인 최대 {11}MB" +
          " · 접속 최대 {12} · 줄/초 받음 {13} 보냄 {14} · 답(잼) {15}개 가운데 {16} 느린 1% {17} 최대 {18}(ms) · 더미 CPU 평균 {19} 최대 {20}(ms/초)" +
          " · 471 {21} · 다시 로그인 {22} · 지나간 양 로비 구간 {23}초(잼 자리 {24}) · 더미 잼 {25}초 · {26}") -f
    $spec.N, $spec.People, $spec.Rate, $spec.Seconds, $v.Kind, $(if ($v.Why) { "($($v.Why))" } else { '' }), $r.BusyAvg, $r.BusyMax, $r.LoopAvg, $r.ProcAvg, $r.ProcMax, $r.MemMax,
    $r.ConnMax, $r.InAvg, $r.OutAvg, $r.LatN, $r.LatP50, $r.LatP99, $r.LatMax, $r.DummyCpuAvg, $r.DummyCpuMax, $r.Reject471, $r.Relogin, $r.LobbySec, $r.PlateauSec, $r.JamSec, $tag
}

# ── 시험 — 가짜 판 폴더(summary · lobby.log · dummy.log · env)를 만들어 판정 · 읽기를 본다 ──
if ($SelfTest) {
  $tdir = Join-Path $root ("_out\lobby_series\selftest_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
  $script:ok = 0; $script:bad = 0
  function T([bool]$cond, [string]$what) { if ($cond) { $script:ok++ } else { $script:bad++; Write-Host "[틀림] $what" } }
  function New-FakeRun([string]$name, [hashtable]$o) {
    $d = Join-Path $tdir $name
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    $refused = if ($o.ContainsKey('Refused')) { $o.Refused } else { 0 }
    $jamSec = if ($o.ContainsKey('Jam')) { $o.Jam } else { 300 }
    $cpuMax = if ($o.ContainsKey('CpuMax')) { $o.CpuMax } else { 31 }
    $commit = if ($o.ContainsKey('Commit')) { $o.Commit } else { 'aaa' }
    $sum = @('로비 부하 판 가짜',
      "[더미 끝] 사람 120(활동 12) · 끝 120 · 실패 0(없음) · 다시 로그인 4 · 잰 시간 300.0초 · 채팅 줄 1 · PONG 0 · 방 참가 3(거절 471 2) · 채팅 주소 없음 0 · 거절 알림 $refused · 주인 없는 줄 0 · 끝날 때 남은 접속 0",
      $(if ($o.ContainsKey('LatZero')) { '[더미 끝] 답(잼 구간) — 모두 0(가운데 0.0 · 느린 1% 0.0 · 최대 0.0) · 없음' }
        else { '[더미 끝] 답(잼 구간) — 모두 500(가운데 0.3 · 느린 1% 1.2 · 최대 3.0) · 목록 100(가운데 0.3 · 느린 1% 1.0 · 최대 2.0)' }),
      '  [확인] 더미 종료 코드 0')
    foreach ($f in @($o.Fails)) { if ($f) { $sum += "  [실패] $f" } }
    [IO.File]::WriteAllLines((Join-Path $d 'summary.txt'), [string[]]$sum, $enc)
    # 로비: 올림 3초(40 · 80 · 121) → 잼 300초 121 → 끝 2초(60 · 0)
    $conns = @(40, 80) + @(1..301 | ForEach-Object { 121 }) + @(60, 0)
    $lob = @('00:00:00.000 [로비] 구간 켬 — 초마다 [로비 구간] 줄')
    for ($i = 0; $i -lt $conns.Count; $i++) {
      $busy = if ($i -eq 100) { '2.5' } else { '0.5' }
      $lob += "00:00:00.000 [로비 구간] 초 $($i + 1) · 바쁨 $busy% · 접속 $($conns[$i]) · 받은 줄 10 · 보낸 줄 12 · 깬 수 20 · 지난 1000.0 · 기다림 995.0 · 연결 받음 1 보냄 1 · CPU 고리 16 프로세스 16 · 메모리 개인 8.0MB 작업 10.0MB"
    }
    [IO.File]::WriteAllLines((Join-Path $d 'lobby.log'), [string[]]$lob, $enc)
    $dum = @('[더미 구간] 초 1 · 단계 올림 · 쉬기 0 · 일 0 · 방 0 · 로그인 중 50 · 실패 0 · 끝 0 · 접속 150 · 로비 보낸 줄 1 받은 줄 1 · 채팅 보낸 줄 0 받은 줄 0 · 라우터 보낸 줄 0 받은 줄 0 · 답 1 · 느린 1% 0.5 · 최대 0.5 · 답 없음 0 · 뜻밖 끊김 0 · CPU 더미 999')
    for ($i = 0; $i -lt $jamSec; $i++) {
      $c = if ($i -eq 7) { $cpuMax } elseif ($o.ContainsKey('CpuZero')) { 0 } else { @(0, 16)[$i % 2] }
      $dum += "[더미 구간] 초 $($i + 2) · 단계 잼 · 쉬기 100 · 일 10 · 방 1 · 로그인 중 0 · 실패 0 · 끝 0 · 접속 360 · 로비 보낸 줄 3 받은 줄 4 · 채팅 보낸 줄 1 받은 줄 1 · 라우터 보낸 줄 0 받은 줄 0 · 답 3 · 느린 1% 0.5 · 최대 0.5 · 답 없음 0 · 뜻밖 끊김 0 · CPU 더미 $c"
    }
    [IO.File]::WriteAllLines((Join-Path $d 'dummy.log'), [string[]]$dum, $enc)
    [IO.File]::WriteAllLines((Join-Path $d 'env.txt'), [string[]]@("커밋: $commit"), $enc)
    return $d
  }
  $cases = @(
    @('기본 통과', 0, @{}, 100, '통과'),
    @('① 줄 손실', 0, @{ Fails = @('① 로비 — 더미 보낸 줄 5 = 로비 받은 줄 6') }, 100, '깨짐'),
    @('③ 답 없음', 0, @{ Fails = @('③ 답 없음 2') }, 100, '깨짐'),
    @('③ 뜻밖 끊김', 0, @{ Fails = @('③ 뜻밖 끊김 없음(1몫)') }, 100, '깨짐'),
    @('③ 더미 실패', 0, @{ Fails = @('③ 더미 — 끝 119/120 · 실패 0') }, 100, '깨짐'),
    @('거절 알림만', 0, @{ Refused = 2 }, 100, '깨짐'),
    @('더미 CPU = 문턱', 0, @{ CpuMax = 100 }, 100, '통과'),
    @('더미 CPU 문턱 넘음', 0, @{ CpuMax = 101 }, 100, '버림'),
    @('더미 CPU 넘음 + 깨짐', 0, @{ CpuMax = 101; Fails = @('③ 답 없음 9') }, 100, '버림'),
    @('파일럿(문턱 0)', 0, @{ CpuMax = 5000 }, 0, '통과'),
    @('PC 검사 실패', 3, @{}, 100, '미결'),
    @('미커밋', 2, @{}, 100, '미결'),
    @('코어 다름', 4, @{}, 100, '미결'),
    @('종료 코드', 1, @{ Fails = @('서버 종료 코드 — 로비 1 · 채팅 0 · 라우터 0 · 게임 서버 0') }, 100, '미결'),
    @('② 구간 합', 1, @{ Fails = @('② [로비 구간] 300줄 합 — 받은 줄 1 · 보낸 줄 2(끝 줄 3 · 4)') }, 100, '미결'),
    @('더미 잼 짧음', 0, @{ Jam = 297 }, 100, '미결'),
    @('더미 잼 경계', 0, @{ Jam = 298 }, 100, '통과'),
    @('더미 CPU 다 0', 0, @{ CpuZero = 1; CpuMax = 0 }, 100, '미결'),
    @('커밋 다름', 0, @{ Commit = 'bbb' }, 100, '미결'),
    @('답 0개', 0, @{ LatZero = 1 }, 100, '미결')
  )
  $kinds = @{}
  foreach ($c in $cases) {
    $d = New-FakeRun ($c[0] -replace '[^\w가-힣]', '_') $c[2]
    $r = Read-Run $d
    $v = Get-LobbyVerdict $c[1] $r $c[3] 300 'aaa'
    T ($v.Kind -eq $c[4]) "$($c[0]) — $($v.Kind)(기대 $($c[4])) $($v.Why)"
    $kinds[$v.Kind] = 1 + [int]$kinds[$v.Kind]
  }
  T ($kinds.Keys.Count -eq 4) "판정 종류 $($kinds.Keys.Count)(기대 4) — $(($kinds.Keys | Sort-Object) -join ' · ')"
  # 읽기 값 — 기본 판
  $r = Read-Run (Join-Path $tdir '기본_통과')
  T ($r.PlateauSec -eq 301 -and $r.ConnMax -eq 121 -and $r.LobbySec -eq 305) "로비 잼 자리 $($r.PlateauSec)(기대 301) · 접속 최대 $($r.ConnMax)(121) · 구간 $($r.LobbySec)(305)"
  T ($r.BusyMax -eq 2.5 -and $r.BusyAvg -eq 0.51 -and $r.ProcAvg -eq 16 -and $r.MemMax -eq 8) "바쁨 최대 $($r.BusyMax)(2.5) · 평균 $($r.BusyAvg)(0.51) · 프로세스 $($r.ProcAvg)(16) · 메모리 $($r.MemMax)(8)"
  T ($r.JamSec -eq 300 -and $r.DummyCpuMax -eq 31 -and $r.DummyCpuNonzero -eq 150) "더미 잼 $($r.JamSec)(300) · CPU 최대 $($r.DummyCpuMax)(31 · 올림 999 는 안 셈) · 0 아닌 초 $($r.DummyCpuNonzero)(150)"
  T ($r.LatN -eq 500 -and $r.LatP99 -eq 1.2 -and $r.Reject471 -eq 2 -and $r.Relogin -eq 4 -and $r.Refused -eq 0) "답 $($r.LatN) · 느린 1% $($r.LatP99) · 471 $($r.Reject471) · 다시 로그인 $($r.Relogin) · 거절 알림 $($r.Refused)"
  Write-Host "시험 $($script:ok)/$($script:ok + $script:bad) · 가짜 판 $tdir"
  if ($script:bad -gt 0) { exit 1 } else { exit 0 }
}

# ── 본 흐름 ──
if ($From -lt 1 -or $From -gt $plan.Count) { Write-Host "[멈춤] -From $From — 1~$($plan.Count)"; exit 2 }
if ($From -ge 2 -and (-not $SeriesDir -or $DummyCpuMaxMs -le 0 -or -not $CpuWhy)) { Write-Host '[멈춤] -From 2 이상은 -SeriesDir · -DummyCpuMaxMs · -CpuWhy 가 다 있어야 한다(파일럿에서 정한 문턱)'; exit 2 }
if (-not $SeriesDir) { $SeriesDir = Join-Path $root ("_out\lobby_series\" + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force -Path $SeriesDir | Out-Null
$seriesTxt = Join-Path $SeriesDir 'series.txt'
$stamp = Split-Path -Leaf $SeriesDir
function Out-Series([string]$s) { Write-Host $s; [IO.File]::AppendAllText($seriesTxt, $s + "`r`n", $enc) }

$baseCommit = ''
$prev = Read-Lines $seriesTxt
$bl = @($prev | Where-Object { $_.StartsWith('기준 커밋: ') })
if ($bl.Count) { $baseCommit = $bl[0].Substring(7).Trim() }
if ($From -eq 1) {
  if ($prev.Count) { Write-Host "[멈춤] $seriesTxt 가 이미 있음 — 처음부터면 새 폴더"; exit 2 }
  $baseCommit = (& git -C $root rev-parse HEAD).Trim()
  Out-Series "로비 옮기기 전 값 여섯 판 — 시작 $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
  Out-Series "기준 커밋: $baseCommit"
  Out-Series '조건: lobby_load -Measure · 활동 0.1 · 대화방 5 · 더미 일꾼 2 · 인증 · DB 끔 · 올림 초당 50 · 방 머묾 30초'
} else {
  if (-not $baseCommit) { Write-Host "[멈춤] $seriesTxt 에 기준 커밋 줄이 없음"; exit 2 }
  Out-Series "이어 감 $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') — $From 번째 판부터 · 더미 바쁨 문턱 $DummyCpuMaxMs ms/초 · 근거: $CpuWhy"
}

foreach ($spec in $plan) {
  if ($spec.N -lt $From) { continue }
  $tag = "LS$stamp`_$($spec.N)_m$($spec.People)_r$($spec.Rate)_s$($spec.Seconds)"
  Write-Host "── 판 $($spec.N) 시작 $(Get-Date -Format 'HH:mm:ss') · $tag"
  $console = @(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'lobby_load.ps1') -Measure -People $spec.People -Seconds $spec.Seconds `
      -RateScale $spec.Rate -Active 0.1 -ChatLobbies 5 -Workers 2 -Tag $tag 2>&1 | ForEach-Object { "$_" })
  $rc = $LASTEXITCODE
  $runDir = Join-Path $root "_out\lobby_load\$tag"
  [IO.File]::WriteAllLines((Join-Path $SeriesDir "$tag.console.txt"), [string[]]$console, $enc)
  $r = Read-Run $runDir
  $v = Get-LobbyVerdict $rc $r $DummyCpuMaxMs $spec.Seconds $baseCommit
  Out-Series (Curve $spec $v $r $tag)
  switch ($v.Kind) {
    '깨짐' { Out-Series "▶ 옮기기 전 로비가 M $($spec.People)(배율 $($spec.Rate))에서 깨짐 — 멈춤"; exit 5 }
    '버림' { Out-Series '▶ 더미 바쁨으로 버림 — 멈춤(M 이 클수록 더 바쁘다)'; exit 6 }
    '미결' { Out-Series '▶ 미결 — 멈춤'; exit 7 }
  }
  if ($spec.N -eq 1 -and $DummyCpuMaxMs -le 0) {
    Out-Series "▶ 파일럿 끝 — 위 줄을 보고 더미 바쁨 문턱을 정해 -From 2 -SeriesDir `"$SeriesDir`" -DummyCpuMaxMs <값> -CpuWhy <근거> 로 잇는다"
    exit 10
  }
}
Out-Series "▶ 여섯 판 통과 — 끝 $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
exit 0
