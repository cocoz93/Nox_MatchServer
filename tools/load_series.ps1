# 부하 측정 시리즈 — 측정 플랜(Docs/MO_MEASURE_PLAN_1111.md) 5절 단계를 차례대로 load_run → load_report 로 돌리고,
#   3절 반복 순서 · 9절 멈춤 규칙 · 지점 판정(2-2절 — 가장 느린 1% 는 3회 중앙값, 끊김 · 사라짐은 한 번이라도)을 맡는다.
#
#   왜 이 대본이 있나: 60회 안팎 · 11~16시간(계산 · 11-13 줄이기 뒤)을 사람 없이 돈다. 지점마다 3회 · 1회차는 처음 넘은 지점의 한 칸 위까지 ·
#   버리면 다시 · 같은 지점 3번 버리면 멈춤을 손으로 하면 차례가 틀어진다. 실행마다 판정 근거는 그 폴더 report.json 에 남고,
#   이 대본은 그것을 series.jsonl 에 모아 다음 차례를 정한다. 「먼저 찬 곳」 고르기(5절)는 사람이 summary.txt 를 읽고 한다.
#
# 사용:
#   첫째 묶음(M1 ~ M4 확인):  powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_series.ps1 -Batch 1
#   둘째 묶음(M5 ~ M9):        ... -Batch 2 -SeriesDir _out\load_series\<첫째 묶음 폴더>   (L · 60% 지점 · 완료 시간 20ms 지점은 그 폴더 state.json 에서)
#   멈춘 뒤 잇기:              같은 명령에 -SeriesDir 를 주면 쓸 수 있는 실행이 이미 있는 자리는 다시 안 돈다
#   흐름만 시험:               -SimulateLimit 100   (실행 없이 「방 수 > 100 이면 넘음」 가짜 판정으로 차례만 돈다)
#
# 종료 코드: 0 = 끝 · 1 = 멈춤 규칙(까닭은 series.txt 마지막 [멈춤] 줄) · 2 = 인자 · 상태 파일 문제
param(
  [ValidateSet(1, 2)][int]$Batch = 1,
  [string]$SeriesDir = '',
  [string[]]$Stage = @(),            # 비우면 묶음의 단계 전부 · 고르면 그 단계만(시험용 — 예: -Stage M2)
  [int]$L = 0,                       # 둘째 묶음 — 주면 state.json 대신 이 값
  [int]$P60 = 0,
  [int]$P20 = 0,                     # 둘째 묶음 — 완료 시간 20ms 지점(M7 · M9 가 도는 방 수 · 11-14)
  [int]$SimulateLimit = 0,          # 0 보다 크면 실행 없이 가짜 판정
  [string]$SimulateAnomaly = '',     # 시험용 — 'above' = 처음 넘은 지점 위 한 칸이 두 번 통과 · 'edge' = M4 확인의 앞뒤가 다름 · 'freq' = 빈도 2배의 큰 구간이 바뀜 · 'm3' = 사건 기록 끄면 걸음이 줆
  [string]$LoadRunExtra = ''         # load_run 에 그대로 붙인다(시험 때 문턱을 풀 때만 — 그런 실행은 load_report 가 버린다)
)

$ErrorActionPreference = 'Stop'
$utf8Bom = New-Object Text.UTF8Encoding($true)
$inv = [Globalization.CultureInfo]::InvariantCulture
$loadRun = Join-Path $PSScriptRoot 'load_run.ps1'
$loadReport = Join-Path $PSScriptRoot 'load_report.ps1'
$repo = Split-Path -Parent $PSScriptRoot

# ── 정한 값 ─────────────────────────────────────────────────────
$OverMs = 33              # 2-2절 — 가장 느린 1% 칸이 이 값을 넘으면 넘음
$JoinSec = 0.2            # 더미 입장 간격 기본 200ms(3절)
$TailSec = 60             # 실행 길이 = 입장 + 측정 구간 + 이 값. 측정 구간 시작은 마지막 방 + 30초를 다음 분포 경계(≤10초)로,
                          #   끝도 경계(≤10초)로 맞추고, 더미 시각은 서버보다 최대 약 3초 앞설 수 있다 — 30+10+10+3 = 53 에 여유. 플랜 6절 어림 45초보다 15초 길다
$MaxRooms = 1024          # 5절 「128방도 안 넘으면 2배를 계속」의 상한 — 더미 10,240명은 이 PC 에서 못 띄운다(4스레드 · 계산)
$M4StartRooms = 16        # M4 첫 지점 — 1 · 2 · 4 · 8방은 빼고(16방까지 게임 스레드 약 13% 이하) 이 지점은 1회만 돈다(측정 플랜 8절 7 · 11-13 사용자 결정)
$M3Floor = 0.1            # M3 — 구간 몫 차이가 이 ms/초 밑이면 흔들림으로 본다(report 의 반올림이 0.01 · 거의 0 인 구간이 우연히 걸리지 않게 · 내가 정함)
$M1RisePct = 5            # M1 — 측정 구간을 셋으로 나눠 틱합이 차례로 오르고 마지막이 처음보다 이 % 넘게 크면 「계속 오름」(내가 정함)
$M1SettlePct = 10         # M1 — 측정 구간 처음 60초의 틱합이 나머지와 이 % 넘게 다르면 버리는 구간 30초가 모자람(내가 정함)

$inSim = $SimulateLimit -gt 0
if (-not $SeriesDir) {
  $root = Join-Path $repo '_out\load_series'
  $SeriesDir = Join-Path $root ((Get-Date -Format 'yyyyMMdd_HHmmss') + $(if ($inSim) { '_sim' } else { '' }))
}
New-Item -ItemType Directory -Force -Path $SeriesDir | Out-Null
$SeriesDir = (Resolve-Path -LiteralPath $SeriesDir).Path
$logPath = Join-Path $SeriesDir 'series.txt'
$recPath = Join-Path $SeriesDir 'series.jsonl'
$statePath = Join-Path $SeriesDir 'state.json'

function Log([string]$s) {
  $line = (Get-Date -Format 'MM-dd HH:mm:ss') + ' ' + $s
  Write-Host $line
  [IO.File]::AppendAllText($logPath, $line + "`r`n", $utf8Bom)
}

# 상태 — 단계 사이에 넘기는 값(M3 에서 끌 계측 · L · 60% 지점 · 20ms 지점). 이어 돌 때 그대로 읽는다
$state = @{}
if (Test-Path -LiteralPath $statePath) {
  $j = Get-Content -Raw -Encoding UTF8 -LiteralPath $statePath | ConvertFrom-Json
  foreach ($p in $j.PSObject.Properties) { $state[$p.Name] = $p.Value }
}
function Save-State { [IO.File]::WriteAllText($statePath, ($state | ConvertTo-Json -Depth 4), $utf8Bom) }

# 실행 기록 — 한 줄 = 실행 하나(report.json 에서 분포 칸을 뺀 것 + stage · key · pass)
$records = New-Object System.Collections.ArrayList
if (Test-Path -LiteralPath $recPath) {
  foreach ($ln in [IO.File]::ReadAllLines($recPath, $utf8Bom)) { if ($ln.Trim()) { [void]$records.Add(($ln | ConvertFrom-Json)) } }
}

function Stop-Series([string]$why) {
  Log "[멈춤] $why"
  Save-State
  exit 1
}

function Median($xs) {
  $s = @($xs | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
  if ($s.Count -eq 0) { return $null }
  if ($s.Count % 2) { return $s[($s.Count - 1) / 2] }
  return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2.0
}
function Q([string]$s) { return "'" + $s.Replace("'", "''") + "'" }
function Part($r, [string]$name) { $v = $r.partMsPerSec.$name; if ($null -eq $v) { return 0.0 } else { return [double]$v } }

# ── 실행 하나 ───────────────────────────────────────────────────
# cfg = @{ rooms; size; window; edge; behavior; classes; freq; cycle; serverExtra; dummyExtra; noDummy }
function Cfg([int]$rooms, [hashtable]$more = @{}) {
  $c = @{ rooms = $rooms; size = 10; window = 300; edge = 0; behavior = 'fight'; classes = '0,1'; freq = 1.0; cycle = 0
          serverExtra = $script:serverExtra; dummyExtra = ''; noDummy = $false }
  foreach ($k in $more.Keys) { $c[$k] = $more[$k] }
  return $c
}
function Run-Sec($c) {
  if ($c.noDummy) { return 30 + $c.window + 25 }   # 방이 없어 측정 구간은 30초 뒤 경계부터
  return [int][math]::Ceiling($c.rooms * $c.size * $JoinSec) + $c.window + $TailSec
}

function Simulate-Run([string]$stage, [string]$key, $c) {
  # 가짜 판정 — 방 수가 SimulateLimit 을 넘으면 틱 완료가 넘는다. 게임 바쁨은 한계에서 약 83%(= 한계 × 1.2 에서 100%)
  $r = $c.rooms; $lim = $SimulateLimit
  $over = $r -gt $lim
  # 'above' — 1회차에 처음 넘은 지점 위 한 칸을 처음 두 번 통과로 뒤집는다(중앙값까지 통과가 되게 · 다시 돈 4~6회차는 안 뒤집음)
  if ($SimulateAnomaly -eq 'above' -and $stage -eq 'M4' -and $key -eq "r$([int]$script:simAboveRooms)" -and [int]$script:simAboveFlips -lt 2) {
    $over = -not $over; $script:simAboveFlips = [int]$script:simAboveFlips + 1
  }
  $busy = [math]::Min(100.0, 100.0 * $r / ($lim * 1.2))
  $walk = 3.0 * $r; $send = 2.0 * $r; $stat = 2.5 * $r
  if ($SimulateAnomaly -eq 'freq' -and $c.freq -eq 2.0) { $stat = 4.0 * $r }   # 빈도 2배에서 정적이 걸음을 넘음
  if ($SimulateAnomaly -eq 'm3' -and $c.serverExtra -like '*--no-event-log*') { $walk = 2.4 * $r }   # 사건 기록을 끄면 걸음이 20% 준다(실측 s9_* 꼴)
  $tick = $walk + $stat + 0.5 * $r
  $e1 = $tick; $e2 = if ($SimulateAnomaly -eq 'edge' -and $stage -eq 'M4C') { $tick * 1.3 } else { $tick }
  $done = if ($over) { 40 } else { [math]::Min(30, 1 + [int]($r / 10)) }
  $blocks = @(); for ($i = 0; $i -lt [int]($c.window / 10); $i++) { $blocks += @{ endSec = 100 + 10 * $i; inWindow = $true; tickSumMsPerSec = $tick; gameBusyPct = $busy; completionP99Ms = $done } }
  $h = [ordered]@{
    dir = "(가짜) $stage $key"; usable = $true; reasons = @(); over = @($(if ($over) { '틱 완료' }))
    rooms = $r; roomSize = $c.size; dummies = $(if ($c.noDummy) { 0 } else { $r * $c.size }); dummyDurationSec = (Run-Sec $c)
    completionP99Ms = $done; sendP99Ms = 1; lateRuns = 0; gameBusyPct = $busy; netBusyPct = $busy / 3; loopRestMsPerSec = 0.1 * $r
    partMsPerSec = [ordered]@{ '틱합' = $tick; '걸음' = $walk; '정적' = $stat; '보내기' = $send; '받기' = $send / 2 }
    timeouts = 0; overflow = 0; inputDiffPct = 0; pieceDiffPct = 0; queueDrops = 0; sendFail = 0; memPrivMaxMb = 10 + $r
    cpuMhz = '가짜'; ticks = 9000; recvPieces = 1000 * $r; windowSec = $c.window
    edgeFirst = @{ tickSumMsPerSec = $e1; completionP99Ms = $done; sendP99Ms = 1 }; edgeLast = @{ tickSumMsPerSec = $e2; completionP99Ms = $done; sendP99Ms = 1 }
    blockSeries = $blocks; dummySummary = @{ sentSpell = [int](1000 * $c.freq * $r) }
  }
  return ($h | ConvertTo-Json -Depth 5 -Compress | ConvertFrom-Json)
}

function Invoke-Run([string]$stage, [string]$key, [int]$pass, $c) {
  $tag = "${stage}_${key}_p$pass"
  $sec = Run-Sec $c
  if ($inSim) { $rep = Simulate-Run $stage $key $c }
  else {
    $cmd = "& $(Q $loadRun) -Tag $(Q $tag) -Rooms $($c.rooms) -RoomSize $($c.size) -Sec $sec -Behavior $(Q $c.behavior) -Classes $(Q $c.classes)" +
           " -FreqMult $(([double]$c.freq).ToString($inv)) -CycleSec $($c.cycle)"
    if ($c.serverExtra) { $cmd += " -ExtraServerArgs $(Q $c.serverExtra)" }
    if ($c.dummyExtra) { $cmd += " -ExtraDummyArgs $(Q $c.dummyExtra)" }
    if ($c.noDummy) { $cmd += ' -NoDummy' }
    if ($LoadRunExtra) { $cmd += " $LoadRunExtra" }
    $cmd += '; exit $LASTEXITCODE'
    Log "[실행] $tag · 방 $($c.rooms) × $($c.size) · $sec 초 · 측정 구간 $($c.window)초"
    $out = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    foreach ($ln in $out) { if ($ln -match '^\[(게이트|멈춤|주의|끝|코어)\]') { Log "  $ln" } }
    $runDir = $null
    foreach ($ln in $out) {
      $m = [regex]::Match($ln, '^\[끝\] (.+?) · 서버 종료 코드'); if ($m.Success) { $runDir = $m.Groups[1].Value }
      $m = [regex]::Match($ln, '^지문: (.+)\\env\.txt$'); if ($m.Success -and -not $runDir) { $runDir = $m.Groups[1].Value }
    }
    if ($code -eq 1) { Stop-Series "PC 검사 실패 — $tag · $runDir\env.txt (무거운 프로그램은 사용자가 끈다)" }
    if ($code -ne 0 -or -not $runDir) { Stop-Series "load_run 종료 코드 $code — $tag · $runDir" }
    $rl = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $loadReport -RunDir $runDir -WindowSec $c.window -EdgeSec $c.edge 2>&1 | ForEach-Object { "$_" })
    foreach ($ln in $rl) { Log "  $ln" }
    $rp = Join-Path $runDir 'report.json'
    if (-not (Test-Path -LiteralPath $rp)) { Stop-Series "report.json 없음 — $runDir" }
    $rep = Get-Content -Raw -Encoding UTF8 -LiteralPath $rp | ConvertFrom-Json
  }
  # 분포 칸은 report.json 에만 두고 기록에는 뺀다
  $keep = [ordered]@{ stage = $stage; key = $key; pass = $pass; at = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
    serverExtra = $c.serverExtra; dummyExtra = $c.dummyExtra }   # 어느 계측을 끄고 돌았나 — env.txt 명령줄과 같은 값을 기록에서 바로 보려고
  foreach ($p in $rep.PSObject.Properties) { if ($p.Name -notin @('completionHist', 'sendHist')) { $keep[$p.Name] = $p.Value } }
  $line = $keep | ConvertTo-Json -Depth 6 -Compress
  [IO.File]::AppendAllText($recPath, $line + "`r`n", $utf8Bom)
  $rec = $line | ConvertFrom-Json
  [void]$records.Add($rec)
  $text = if ($rec.usable) { if (@($rec.over).Count) { "넘음 $(@($rec.over) -join ' · ')" } else { '넘음 없음' } } else { "버림($(@($rec.reasons) -join ' · '))" }
  Log "[결과] $tag · $text · 틱 완료 1% $($rec.completionP99Ms) · 보내기 1% $($rec.sendP99Ms) · 게임 바쁨 $($rec.gameBusyPct)%"
  return $rec
}

# 쓸 수 있는 실행이 이미 있으면 그것을, 없으면 돌린다. 같은 지점(stage · key)의 버림이 3회가 되면 멈춘다(9절)
function Run-Point([string]$stage, [string]$key, [int]$pass, $c) {
  foreach ($r in $records) { if ($r.stage -eq $stage -and $r.key -eq $key -and $r.pass -eq $pass -and $r.usable) { return $r } }
  while ($true) {
    $discards = @($records | Where-Object { $_.stage -eq $stage -and $_.key -eq $key -and -not $_.usable }).Count
    if ($discards -ge 3) { Stop-Series "같은 지점에서 실행 버림 3회 — $stage $key(까닭은 위 [결과] 줄)" }
    $r = Invoke-Run $stage $key $pass $c
    if ($r.usable) { return $r }
  }
}
function Point-Records([string]$stage, [string]$key, [int[]]$passes) {
  return @($records | Where-Object { $_.stage -eq $stage -and $_.key -eq $key -and $_.usable -and $passes -contains $_.pass })
}

# 지점 판정(2-2절) — 가장 느린 1% 는 3회 중앙값, 끊김 · 넘침과 사라짐은 3회 중 한 번이라도
function Verdict($recs) {
  $over = New-Object System.Collections.ArrayList
  $c = Median ($recs | ForEach-Object { $_.completionP99Ms })
  $s = Median ($recs | ForEach-Object { $_.sendP99Ms })
  if ($null -ne $c -and $c -gt $OverMs) { [void]$over.Add('틱 완료') }
  if ($null -ne $s -and $s -gt $OverMs) { [void]$over.Add('보내는 쪽 지연') }
  foreach ($w in @('끊김 · 넘침', '사라짐')) { if (@($recs | Where-Object { @($_.over) -contains $w }).Count) { [void]$over.Add($w) } }
  return [pscustomobject]@{
    isOver = ($over.Count -gt 0); over = @($over); completionP99 = $c; sendP99 = $s
    gameBusy = (Median ($recs | ForEach-Object { $_.gameBusyPct })); tickSum = (Median ($recs | ForEach-Object { Part $_ '틱합' }))
    tickSumRange = $(if ($recs.Count) { $t = @($recs | ForEach-Object { Part $_ '틱합' }); ($t | Measure-Object -Maximum).Maximum - ($t | Measure-Object -Minimum).Minimum } else { 0 })
  }
}

# 게임 · 통신 스레드에서 몫이 가장 큰 구간(5절 「먼저 찬 곳」 4번 — 지점이 하나인 단계). 틱합 · 방 만들기는 다른 구간을 품어 뺀다
$netParts = @('통신 재전송', '보내기', '받기', '넘기기'); $nestedParts = @('틱합', '방 만들기')
function Top-Part($recs, [bool]$net) {
  $names = @{}
  foreach ($r in $recs) { foreach ($p in $r.partMsPerSec.PSObject.Properties) { if ($nestedParts -notcontains $p.Name -and (($netParts -contains $p.Name) -eq $net)) { $names[$p.Name] = 1 } } }
  $best = $null; $bestV = -1.0
  foreach ($n in $names.Keys) { $v = Median ($recs | ForEach-Object { Part $_ $n }); if ($v -gt $bestV) { $best = $n; $bestV = $v } }
  return $best
}

# 7절 한 줄 — 지점마다
function Point-Line([string]$label, $recs) {
  $v = Verdict $recs
  $vals = { param($f) (@($recs | ForEach-Object { & $f $_ }) -join '/') }
  $parts = @{}
  foreach ($r in $recs) { foreach ($p in $r.partMsPerSec.PSObject.Properties) { if ($nestedParts -notcontains $p.Name) { $parts[$p.Name] = 1 } } }
  $med = @($parts.Keys | ForEach-Object { $nm = $_; [pscustomobject]@{ n = $nm; v = (Median ($recs | ForEach-Object { Part $_ $nm })) } } | Sort-Object v -Descending)
  $rooms = [double]$recs[0].rooms
  $partText = @($med | Select-Object -First 6 | ForEach-Object { '{0} {1:F1}(방당 {2:F2})' -f $_.n, $_.v, ($_.v / [math]::Max(1.0, $rooms)) }) -join ' · '
  return ('{0} · {1} · 틱 완료 1% {2}(중앙 {3}) · 보내기 1% {4}(중앙 {5}) · 밀림 {6} · 게임 바쁨 {7} · 통신 바쁨 {8} · 고리 나머지 {9} · 틱합 {10} · {11} · 끊김 {12} · 넘침 {13} · 입력 차이 {14} · 조각 차이 {15} · 보내기 실패 {16} · 메모리 {17} · CPU {18} · 틱 {19} · 받은 조각 {20}' -f `
      $label, $(if ($v.isOver) { "넘음($($v.over -join ' · '))" } else { '통과' }),
      (& $vals { $args[0].completionP99Ms }), $v.completionP99, (& $vals { $args[0].sendP99Ms }), $v.sendP99,
      (& $vals { $args[0].lateRuns }), (& $vals { $args[0].gameBusyPct }), (& $vals { $args[0].netBusyPct }), (& $vals { $args[0].loopRestMsPerSec }),
      (& $vals { Part $args[0] '틱합' }), $partText, (& $vals { $args[0].timeouts }), (& $vals { $args[0].overflow }),
      (& $vals { $args[0].inputDiffPct }), (& $vals { $args[0].pieceDiffPct }), (& $vals { $args[0].sendFail }), (& $vals { $args[0].memPrivMaxMb }),
      (& $vals { $args[0].cpuMhz }), (& $vals { $args[0].ticks }), (& $vals { $args[0].recvPieces }))
}
$summaryPath = Join-Path $SeriesDir 'summary.txt'
function Summary([string]$s) { [IO.File]::AppendAllText($summaryPath, $s + "`r`n", $utf8Bom); Log "[요약] $s" }

$want = if ($Stage.Count) { @($Stage | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) } elseif ($Batch -eq 1) { @('M1', 'M2', 'M3', 'M4', 'M4C') } else { @('M5', 'M6', 'M7', 'M9') }
$script:serverExtra = if ($state.ContainsKey('m3Off')) { (@($state['m3Off']) -join ' ') } else { '' }
Log "[시작] 묶음 $Batch · 단계 $($want -join ' ') · 폴더 $SeriesDir$(if ($inSim) { " · 가짜 판정(한계 $SimulateLimit 방)" })"

# ── M1 시험 1회 ─────────────────────────────────────────────────
if ($want -contains 'M1') {
  $r = Run-Point 'M1' 'r32' 1 (Cfg 32 @{ window = 600; edge = 300; serverExtra = '' })
  $bs = @($r.blockSeries | Where-Object { $_.inWindow } | ForEach-Object { [double]$_.tickSumMsPerSec })
  if ($bs.Count -lt 9) { Stop-Series "M1 — 측정 구간의 10초 칸이 $($bs.Count)개뿐" }
  $third = [int][math]::Floor($bs.Count / 3)
  $t1 = ($bs[0..($third - 1)] | Measure-Object -Average).Average
  $t2 = ($bs[$third..(2 * $third - 1)] | Measure-Object -Average).Average
  $t3 = ($bs[(2 * $third)..($bs.Count - 1)] | Measure-Object -Average).Average
  $head = ($bs[0..5] | Measure-Object -Average).Average; $rest = ($bs[6..($bs.Count - 1)] | Measure-Object -Average).Average
  Summary ('M1 · 10명 × 32방 · 600초 · 틱합(ms/초) 셋으로 나눠 {0:F1} → {1:F1} → {2:F1} · 처음 60초 {3:F1} 대 나머지 {4:F1} · 앞 300초 {5} 대 뒤 300초 {6} · 틱 완료 1% {7} · 마지막 방 만들기 {8}초' -f $t1, $t2, $t3, $head, $rest, $r.edgeFirst.tickSumMsPerSec, $r.edgeLast.tickSumMsPerSec, $r.completionP99Ms, $r.lastMadeSec)
  if ($t1 -lt $t2 -and $t2 -lt $t3 -and $t3 -gt $t1 * (1 + $M1RisePct / 100.0)) { Stop-Series ('M1 — 틱합이 계속 오름({0:F1} → {1:F1} → {2:F1} · 9절 · 까닭부터 본다)' -f $t1, $t2, $t3) }
  if ([math]::Abs($head / $rest - 1) * 100 -gt $M1SettlePct) { Stop-Series ('M1 — 버리는 구간 30초 뒤에도 틱합이 자리 잡지 않음(처음 60초 {0:F1} 대 나머지 {1:F1})' -f $head, $rest) }
}

# ── M2 빈 서버 ──────────────────────────────────────────────────
if ($want -contains 'M2') {
  $recs = @(); for ($p = 1; $p -le 3; $p++) { $recs += Run-Point 'M2' 'empty' $p (Cfg 1 @{ window = 60; noDummy = $true; serverExtra = '' }) }
  Summary ('M2 · 더미 없음 · 60초 × 3 · 틱 {0} · 완료 표본 {1} · 받은 조각 {2} · 게임 바쁨 {3} · 통신 바쁨 {4} · 메모리 {5}' -f `
    (@($recs | ForEach-Object { $_.ticks }) -join '/'), (@($recs | ForEach-Object { $_.completionSamples }) -join '/'), (@($recs | ForEach-Object { $_.recvPieces }) -join '/'),
    (@($recs | ForEach-Object { $_.gameBusyPct }) -join '/'), (@($recs | ForEach-Object { $_.netBusyPct }) -join '/'), (@($recs | ForEach-Object { $_.memPrivMaxMb }) -join '/'))
}

# ── M3 계측 켬 · 끔 ─────────────────────────────────────────────
#   기본 3회의 흔들림 폭(최대 − 최소)을 넘게 틱합이나 어느 구간의 몫(중앙값)이 달라지는 계측만 뒤 단계에서 끈다(5절)
if ($want -contains 'M3') {
  $conds = [ordered]@{ base = ''; noevlog = '--no-event-log'; nofirst = '--no-first-seen'; noop = '--no-op-count' }
  #   끈 조건 셋은 1회씩 · 기본만 3회(측정 플랜 8절 7 안 ② · 11-13 사용자 결정). 끈 조건마다 바로 앞에 기본 1회가 오게 번갈아 — 시간에 따른 변화가 한쪽에 몰리지 않게
  $order = @(@('base', 1), @('noevlog', 1), @('base', 2), @('nofirst', 1), @('base', 3), @('noop', 1))
  foreach ($o in $order) { [void](Run-Point 'M3' "r64_$($o[0])" $o[1] (Cfg 64 @{ serverExtra = $conds[$o[0]] })) }
  $base = Point-Records 'M3' 'r64_base' @(1, 2, 3)
  $names = @{}; foreach ($r in $records | Where-Object { $_.stage -eq 'M3' -and $_.usable }) { foreach ($p in $r.partMsPerSec.PSObject.Properties) { if ($p.Name -ne '방 만들기') { $names[$p.Name] = 1 } } }
  $off = New-Object System.Collections.ArrayList
  Summary (Point-Line 'M3 기본 r64' $base)
  foreach ($k in @('noevlog', 'nofirst', 'noop')) {
    $fr = Point-Records 'M3' "r64_$k" @(1)
    Summary (Point-Line "M3 $($conds[$k]) r64" $fr)
    $hits = @()
    foreach ($n in $names.Keys) {
      $bv = @($base | ForEach-Object { Part $_ $n }); $range = ($bv | Measure-Object -Maximum).Maximum - ($bv | Measure-Object -Minimum).Minimum
      $d = (Median ($fr | ForEach-Object { Part $_ $n })) - (Median $bv)
      if ([math]::Abs($d) -gt $range -and [math]::Abs($d) -gt $M3Floor) { $hits += ('{0} {1:+0.0;-0.0}(기본 흔들림 {2:F1})' -f $n, $d, $range) }
    }
    if ($hits.Count) { [void]$off.Add($conds[$k]); Summary "M3 끔 → $($conds[$k]) · $($hits -join ' · ')" } else { Summary "M3 켜 둠 → $($conds[$k]) · 흔들림 폭 안" }
  }
  $state['m3Off'] = @($off); Save-State
  $script:serverExtra = (@($off) -join ' ')
}
if ($Batch -eq 1 -and ($want -contains 'M4') -and -not $state.ContainsKey('m3Off')) { Log '[주의] M3 결과 없음 — 계측을 다 켠 채 M4 를 돈다' }

# ── M4 방 늘리기 · 사이 좁히기 · 확인 ─────────────────────────────
function M4-Verdict([int]$rooms, [int[]]$passes = @(1, 2, 3)) { return Verdict (Point-Records 'M4' "r$rooms" $passes) }
# 첫 지점(1회만 돈 곳)이 M4 확인에 오르면 견줄 3회 흔들림 폭이 없다(1회면 폭 0 이라 앞뒤 5분이 조금만 달라도 멈춘다) — 그때만 2 · 3회차를 채운다
function Fill-StartPoint([int]$x) { if ($x -eq $M4StartRooms) { for ($p = 2; $p -le 3; $p++) { [void](Run-Point 'M4' "r$x" $p (Cfg $x)) } } }
if ($want -contains 'M4') {
  # 1회차 — 올리다 처음 넘은 지점의 한 칸 위까지(3절)
  $pts = New-Object System.Collections.ArrayList; $rooms = $M4StartRooms; $firstOverP1 = $null
  while ($true) {
    if ($rooms -gt $MaxRooms) { Stop-Series "M4 — $MaxRooms 방까지 1회차에서 안 넘음" }
    $r = Run-Point 'M4' "r$rooms" 1 (Cfg $rooms)
    [void]$pts.Add($rooms)
    if ($null -ne $firstOverP1) { break }
    if (@($r.over).Count) { $firstOverP1 = $rooms; $script:simAboveRooms = $rooms * 2 }
    $rooms *= 2
  }
  # 2 · 3회차 — 1회차에 돈 지점만 한 번씩(같은 지점을 연달아 안 돈다). 첫 지점은 1회만이라 뺀다 — 판정은 그 1회로(Point-Records 가 있는 회차만 모은다)
  for ($p = 2; $p -le 3; $p++) { foreach ($x in @($pts | Where-Object { $_ -ne $M4StartRooms })) { [void](Run-Point 'M4' "r$x" $p (Cfg $x)) } }
  # 중앙값으로 처음 넘은 지점 — 없으면 위로 2배씩 3회를 더 돈다
  $firstOver = $null
  foreach ($x in $pts) { if ((M4-Verdict $x).isOver) { $firstOver = $x; break } }
  while ($null -eq $firstOver) {
    $x = 2 * $pts[$pts.Count - 1]
    if ($x -gt $MaxRooms) { Stop-Series "M4 — $MaxRooms 방까지 중앙값으로 안 넘음" }
    for ($p = 1; $p -le 3; $p++) { [void](Run-Point 'M4' "r$x" $p (Cfg $x)) }
    [void]$pts.Add($x)
    if ((M4-Verdict $x).isOver) { $firstOver = $x }
  }
  if ($firstOver -eq $M4StartRooms) { Stop-Series "M4 — 첫 지점 $M4StartRooms 방부터 넘음(그 아래는 안 돌아 좁힐 아래 지점이 없다)" }
  # 처음 넘은 지점보다 위에서 통과가 나오면 이상 결과 — 두 지점을 3회씩 다시 돌고(4~6회차) 그것으로 다시 본다(5절 L 정의)
  foreach ($x in @($pts | Where-Object { $_ -gt $firstOver })) {
    if ((M4-Verdict $x).isOver) { continue }
    Summary "M4 이상 결과 — r$firstOver 넘음인데 위 r$x 통과 · 두 지점을 3회씩 다시 돈다"
    for ($p = 4; $p -le 6; $p++) { foreach ($y in @($firstOver, $x)) { [void](Run-Point 'M4' "r$y" $p (Cfg $y)) } }
    $vLo = M4-Verdict $firstOver @(4, 5, 6); $vHi = M4-Verdict $x @(4, 5, 6)
    if ($vLo.isOver -and -not $vHi.isOver) { Stop-Series "M4 — 다시 돌아도 r$firstOver 넘음 · r$x 통과" }
    Summary "M4 다시 돈 값 — r$firstOver $(if ($vLo.isOver) { '넘음' } else { '통과' }) · r$x $(if ($vHi.isOver) { '넘음' } else { '통과' }) · 다시 돈 3회로 판정"
    $state["m4Redo_r$firstOver"] = 1; $state["m4Redo_r$x"] = 1
    if (-not $vLo.isOver) { $firstOver = $x }   # 낮은 쪽이 통과로 바뀌면 처음 넘은 지점이 올라간다
    break
  }
  $passOf = { param($x) if ($state.ContainsKey("m4Redo_r$x")) { @(4, 5, 6) } else { @(1, 2, 3) } }
  $lo = @($pts | Where-Object { $_ -lt $firstOver }) | Sort-Object | Select-Object -Last 1
  $hi = $firstOver
  # 사이 좁히기 — 폭이 아래 지점의 10% 이하 또는 1이 될 때까지(5절). 한 지점 3회는 연달아 돈다(가운데가 앞 결과에 따라 정해져 섞을 짝이 없다)
  while (($hi - $lo) -gt 1 -and ($hi - $lo) -gt 0.1 * $lo) {
    $mid = [int][math]::Floor(($lo + $hi) / 2)
    for ($p = 1; $p -le 3; $p++) { [void](Run-Point 'M4' "r$mid" $p (Cfg $mid)) }
    [void]$pts.Add($mid)
    if ((M4-Verdict $mid).isOver) { $hi = $mid } else { $lo = $mid }
  }
  # 다른 씨앗 1회 — 그 행동이 우연히 가벼웠는지(3절). 판정에는 안 쓰고 적는다
  $seed = Run-Point 'M4' "r${lo}_seed2" 1 (Cfg $lo @{ dummyExtra = '--seed 2' })
  $state['m4Points'] = @($pts | Sort-Object -Unique); $state['lCandidate'] = $lo; $state['lAbove'] = $hi; Save-State
  foreach ($x in @($pts | Sort-Object -Unique)) { Summary (Point-Line "M4 r$x" (Point-Records 'M4' "r$x" (& $passOf $x))) }
  Summary "M4 처음 넘은 지점 r$hi($(@((M4-Verdict $hi (& $passOf $hi)).over) -join ' · ')) · L 후보 r$lo · 다른 씨앗 r$lo $(if (@($seed.over).Count) { "넘음($(@($seed.over) -join ' · ')) — L 후보가 우연히 가벼웠을 수 있다" } else { '통과' })"
}

if ($want -contains 'M4C') {
  if (-not $state.ContainsKey('lCandidate')) { Stop-Series 'M4 확인 — M4 결과(state.json lCandidate)가 없다' }
  $passOf = { param($x) if ($state.ContainsKey("m4Redo_r$x")) { @(4, 5, 6) } else { @(1, 2, 3) } }
  $cand = [int]$state['lCandidate']; $above = [int]$state['lAbove']
  $passing = @(@($state['m4Points']) | ForEach-Object { [int]$_ } | Where-Object { -not (M4-Verdict $_ (& $passOf $_)).isOver } | Sort-Object)
  # 처음 5분과 마지막 5분이 같은가 — 틱합 차이가 그 지점 M4 3회의 흔들림 폭 안이고, 가장 느린 1% 의 넘음 여부가 같으면 같다(내가 정함)
  $edgeCheck = {
    param($rec, [int]$x)
    Fill-StartPoint $x
    $v = M4-Verdict $x (& $passOf $x)
    $d = [math]::Abs([double]$rec.edgeLast.tickSumMsPerSec - [double]$rec.edgeFirst.tickSumMsPerSec)
    $flip = (([double]$rec.edgeFirst.completionP99Ms -gt $OverMs) -ne ([double]$rec.edgeLast.completionP99Ms -gt $OverMs)) -or
            (([double]$rec.edgeFirst.sendP99Ms -gt $OverMs) -ne ([double]$rec.edgeLast.sendP99Ms -gt $OverMs))
    $line = 'M4 확인 r{0} · 30분 · {1} · 앞 5분 틱합 {2} · 1% {3} 대 뒤 5분 틱합 {4} · 1% {5}(M4 3회 틱합 흔들림 {6:F1})' -f $x,
      $(if (@($rec.over).Count) { "넘음($(@($rec.over) -join ' · '))" } else { '통과' }), $rec.edgeFirst.tickSumMsPerSec, $rec.edgeFirst.completionP99Ms,
      $rec.edgeLast.tickSumMsPerSec, $rec.edgeLast.completionP99Ms, $v.tickSumRange
    Summary $line
    if ($d -gt $v.tickSumRange -or $flip) { Stop-Series "M4 확인 — r$x 의 처음 5분과 마지막 5분이 다름(9절 · 측정 길이를 늘릴지 보고)" }
  }
  $cc = @{ window = 1800; edge = 300 }
  $aboveKnownOver = $false
  while ($true) {
    $rc = Run-Point 'M4C' "r$cand" 1 (Cfg $cand $cc)
    & $edgeCheck $rc $cand
    if (-not @($rc.over).Count) { break }
    # L 후보가 30분에 넘음 — 좁히기에서 통과한 그 아래 지점으로 내려 다시(5절). 위 한 칸은 방금 30분에 넘은 그 지점이라 다시 안 돈다
    $lower = @($passing | Where-Object { $_ -lt $cand }) | Select-Object -Last 1
    if ($null -eq $lower) { Stop-Series "M4 확인 — r$cand 이 30분에 넘었고 그 아래 통과 지점이 없다" }
    Summary "M4 확인 — r$cand 30분에 넘음 → r$lower 로 내려 다시"
    $above = $cand; $cand = [int]$lower; $aboveKnownOver = $true
  }
  if (-not $aboveKnownOver) {
    $ra = Run-Point 'M4C' "r$above" 1 (Cfg $above $cc)
    & $edgeCheck $ra $above
    if (-not @($ra.over).Count) {
      Summary "M4 확인 이상 결과 — 위 r$above 이 30분에 통과 · 두 지점을 한 번씩 다시 돈다"
      $rc2 = Run-Point 'M4C' "r$cand" 2 (Cfg $cand $cc); & $edgeCheck $rc2 $cand
      $ra2 = Run-Point 'M4C' "r$above" 2 (Cfg $above $cc); & $edgeCheck $ra2 $above
      if (@($rc2.over).Count -or -not @($ra2.over).Count) { Stop-Series "M4 확인 — 다시 돌아도 r$cand 넘음 또는 r$above 통과(L 을 못 정함)" }
    }
  }
  # 60% 지점(5절) · 완료 시간 20ms 지점(8절 5번) — M4 지점의 중앙값으로
  $pts = @(@($state['m4Points']) | ForEach-Object { [int]$_ } | Sort-Object)
  $p60 = @($pts | Where-Object { (M4-Verdict $_ (& $passOf $_)).gameBusy -le 60 }) | Select-Object -Last 1
  $p20 = @($pts | Where-Object { (M4-Verdict $_ (& $passOf $_)).completionP99 -le 20 }) | Select-Object -Last 1
  $state['L'] = $cand; $state['P60'] = $p60; $state['P20'] = $p20; Save-State
  Summary "L = $cand 방(30분 통과) · 60% 지점 = $p60 방 · 완료 시간 20ms 지점 = $p20 방 · 다음 = 재부팅 뒤 -Batch 2 -SeriesDir $SeriesDir"
}

# ── 둘째 묶음 ───────────────────────────────────────────────────
if ($Batch -eq 2) {
  if ($L -le 0) { $L = [int]$state['L'] }; if ($P60 -le 0) { $P60 = [int]$state['P60'] }; if ($P20 -le 0) { $P20 = [int]$state['P20'] }
  if ($L -le 0 -or $P60 -le 0 -or $P20 -le 0) { Log '[멈춤] 둘째 묶음 — L · 60% 지점 · 완료 시간 20ms 지점 중 없는 것이 있다(-L · -P60 · -P20 을 주거나 첫째 묶음 폴더를 -SeriesDir 로)'; exit 2 }
  Log "[둘째 묶음] L = $L · 60% 지점 = $P60 · 완료 시간 20ms 지점 = $P20 · 끈 계측 $(if ($script:serverExtra) { $script:serverExtra } else { '없음' })"
}

if ($want -contains 'M5') {
  # 60% 지점 × 10명과 같은 총 인원을 4명 · 2명 방으로(5절) — 번갈아 3회
  $total = $P60 * 10
  $c4 = Cfg ([int][math]::Round($total / 4.0)) @{ size = 4 }; $c2 = Cfg ([int]($total / 2)) @{ size = 2 }
  for ($p = 1; $p -le 3; $p++) { [void](Run-Point 'M5' "s4_r$($c4.rooms)" $p $c4); [void](Run-Point 'M5' "s2_r$($c2.rooms)" $p $c2) }
  Summary (Point-Line "M5 4명 × $($c4.rooms)방" (Point-Records 'M5' "s4_r$($c4.rooms)" @(1, 2, 3)))
  Summary (Point-Line "M5 2명 × $($c2.rooms)방" (Point-Records 'M5' "s2_r$($c2.rooms)" @(1, 2, 3)))
}

if ($want -contains 'M6') {
  # 빈도 확인 — 10명 방 1개 · 60초로 절반 · 2배 각 1회. sentSpell 의 초당 수를 같은 꼴의 빈도 1 실행 1회와 견준다(5절)
  #   기준은 M4 r1(빈도 1) 중앙값이었으나 11-13 M4 가 16방부터라 r1 이 없다 — 방 수 · 길이가 같은 빈도 1 실행을 하나 더 돈다
  $rate = { param($r) if ($r.dummyDurationSec -gt 0) { [double]$r.dummySummary.sentSpell / [double]$r.dummyDurationSec } else { $null } }
  $baseRate = & $rate (Run-Point 'M6' 'f1_check' 1 (Cfg 1 @{ window = 60; freq = 1.0 }))
  foreach ($f in @(0.5, 2.0)) {
    $r = Run-Point 'M6' "f$($f.ToString($inv))_check" 1 (Cfg 1 @{ window = 60; freq = $f })
    $ratio = if ($baseRate) { (& $rate $r) / $baseRate } else { $null }
    Summary ('M6 빈도 확인 ×{0} · sentSpell 초당 {1:F2} 대 빈도 1 {2:F2} · 비율 {3:F2}' -f $f, (& $rate $r), $baseRate, $ratio)
  }
  # 60% 지점에서 가만히 · 이동 · 소환수 판 · 빈도 절반 · 2배(전투는 M4 값) — 번갈아 3회
  $conds = [ordered]@{ idle = @{ behavior = 'idle' }; move = @{ behavior = 'move' }; summon = @{ classes = '0,2' }; half = @{ freq = 0.5 }; double = @{ freq = 2.0 } }
  for ($p = 1; $p -le 3; $p++) { foreach ($k in $conds.Keys) { [void](Run-Point 'M6' "r${P60}_$k" $p (Cfg $P60 $conds[$k])) } }
  $passOf = { param($x) if ($state.ContainsKey("m4Redo_r$x")) { @(4, 5, 6) } else { @(1, 2, 3) } }
  $fight = Point-Records 'M4' "r$P60" (& $passOf $P60)
  Summary (Point-Line "M6 전투(M4) r$P60" $fight)
  foreach ($k in $conds.Keys) { Summary (Point-Line "M6 $k r$P60" (Point-Records 'M6' "r${P60}_$k" @(1, 2, 3))) }
  # 빈도 절반 · 2배에서 먼저 차는 구간이 달라지면 멈춘다 — V4 를 되살릴지는 사용자(9절)
  foreach ($k in @('half', 'double')) {
    $fr = Point-Records 'M6' "r${P60}_$k" @(1, 2, 3)
    foreach ($net in @($false, $true)) {
      $a = Top-Part $fight $net; $b = Top-Part $fr $net
      if ($a -ne $b) { Stop-Series "M6 — 빈도 $k 에서 $(if ($net) { '통신' } else { '게임' }) 스레드의 가장 큰 구간이 $a → $b(9절 · 변수 통제 플랜 V4 판단은 사용자)" }
    }
  }
}

if ($want -contains 'M7') {
  # 방 여닫기는 완료 시간 20ms 지점에서 돈다(11-14 사용자 결정 · 처음 플랜은 L).
  #   왜: L(첫째 묶음 192방)은 게임 스레드가 99.9% 바빠 가장 느린 틱이 저절로 14~41ms 흔들린다 —
  #   방 하나 만들기(0.56ms · 실측 r192_p1)가 다른 방을 얼마나 늦추는지가 그 흔들림에 묻힌다. 20ms 지점(128방)은 1~2ms 로 고르다
  $c = Cfg $P20 @{ cycle = 60 }
  for ($p = 1; $p -le 3; $p++) { [void](Run-Point 'M7' "r${P20}_cycle60" $p $c) }
  Summary (Point-Line "M7 방 여닫기 60초 r$P20" (Point-Records 'M7' "r${P20}_cycle60" @(1, 2, 3)))
}
if ($Batch -eq 2) { Log '[건너뜀] M8 함수별 시간 — 관리자 권한의 xperf 라 사용자가 명령을 실행한다(5절 · 8절 4번)' }
if ($want -contains 'M9') {
  # 오래 버티기도 완료 시간 20ms 지점에서 돈다(11-14 사용자 결정 · 처음 플랜은 60% 지점).
  #   왜: 운영 방 수는 그보다 아래에 둔다 — 더 무거운 곳에서 60분을 버티면 그 아래도 버틴다.
  #   방이 많을수록 판이 자주 끝나고 다시 열려 메모리가 느는지 · 뒤로 갈수록 느려지는지가 먼저 드러난다(짐작)
  $r = Run-Point 'M9' "r${P20}_60min" 1 (Cfg $P20 @{ window = 3600; edge = 600 })
  Summary (Point-Line "M9 오래 버티기 60분 r$P20" @($r))
  Summary ('M9 처음 10분 대 마지막 10분 · 틱합 {0} 대 {1} · 틱 완료 1% {2} 대 {3} · 메모리 {4} 대 {5}MB' -f $r.edgeFirst.tickSumMsPerSec, $r.edgeLast.tickSumMsPerSec,
    $r.edgeFirst.completionP99Ms, $r.edgeLast.completionP99Ms, $r.memPrivFirstMb, $r.memPrivLastMb)
}

Save-State
Log "[끝] 묶음 $Batch · 실행 기록 $($records.Count)줄 · 요약 $summaryPath"
exit 0
