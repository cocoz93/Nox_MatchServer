# 부하 실험 한 회의 판정 값 — 실행 폴더(_out/load_run/<시각>_<태그>/)에서 측정 플랜의 버림 조건(2-3절)과 판정 값(2-2절)을 뽑아
#   한 줄로 찍고 그 폴더의 report.json 에 남긴다.
#
#   왜 이 대본이 있나: 판정 값(틱 완료 · 보내는 쪽 지연의 가장 느린 1%)은 10초마다 찍힌 분포 줄을 측정 구간만큼 더해야 나온다.
#   구간 경계를 분포 줄에 안 맞추면 분포 합과 틱 수가 어긋나고, 더미 lateTicks 를 실행 전체로 세면 입장 구간의 것이 섞인다.
#   정본 = Docs/MO_MEASURE_PLAN_1111.md 2절 · 3절 · 7절. 3회 중앙값과 멈춤 규칙은 load_series.ps1 이 report.json 을 모아 정한다.
#
# 사용:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_report.ps1 -RunDir _out\load_run\<시각>_<태그>
#
# 측정 구간 — 처음 입장의 마지막 방(첫 방 닫기 앞의 마지막 방 만들기)이 만들어진 [구간] 줄 끝 + DiscardSec 뒤의 첫 분포 경계부터,
#   WindowSec 이 찬 첫 분포 경계까지. 분포 줄은 [구간] 10줄마다 찍히고 그 10줄 몫만 담는다(TickSections.cpp kLinesPerHistogram).
#   시각은 [구간] 줄의 「지난」을 차례로 더한 「서버가 뜬 뒤 ms」다 — 줄 간격은 서버가 밀리면 1초보다 벌어진다(2-3절).
#   더미 시각은 서버의 첫 방 만들기 줄로 맞춘다(첫 더미가 들어온 때) — 그 줄 하나만큼 어긋날 수 있어 더미 쪽은 넓게(양끝 포함) 센다.
#
# 종료 코드: 0 = 다 읽음(버림이 있어도) · 2 = 읽을 폴더 없음
param(
  [Parameter(Mandatory = $true)][string[]]$RunDir,
  [int]$WindowSec = 300,          # 측정 구간(3절)
  [int]$DiscardSec = 30,          # 마지막 방 만들기 뒤 버리는 초(3절 · M1 에서 확정)
  [double]$OverMs = 33,           # 가장 느린 1% 칸이 이 값을 넘으면 넘음(2-2절 · 칸은 1ms 정수라 33 칸은 안 넘음)
  [double]$LossPercent = 0.1,     # 입력 · 조각 차이가 이 % 를 넘으면 사라짐(2-2절)
  [int]$EdgeSec = 0,              # 0 보다 크면 측정 구간의 처음 · 마지막 이 초를 따로 셈(edgeFirst · edgeLast — M4 확인 300 · M9 600)
  [switch]$Quiet                  # 한 줄을 안 찍고 report.json 만
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
$utf8Bom = New-Object Text.UTF8Encoding($true)
$inv = [Globalization.CultureInfo]::InvariantCulture
function D([string]$s) { return [double]::Parse($s, $inv) }

# PC 검사 문턱의 기본값 — load_run.ps1 의 param 기본값을 그대로 읽는다(두 대본에 값을 따로 적으면 어긋난다)
$gateParams = @('UptimeMaxHours', 'IdleMaxPercent', 'ResidualMaxPercent', 'ResidualMaxReads', 'PowerPlanGuid', 'UpdateServiceStatus')
$runAst = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'load_run.ps1'), [ref]$null, [ref]$null)
$defaults = @{}
foreach ($p in $runAst.ParamBlock.Parameters) {
  if ($p.DefaultValue) { $defaults[$p.Name.VariablePath.UserPath] = [string]$p.DefaultValue.SafeGetValue() }
}

# [구간] 줄 — TickSections.cpp Print 의 꼴. 값은 모두 그 줄 동안의 몫(앞 줄과의 차)이다
$rxHead = [regex]'^\[구간\] 초 (\d+) · 틱 (\d+) · 틱합 ([\d.]+) · 틱최대 ([\d.]+) · 사건 ([\d.]+)\(수 (\d+)\) · 방 만들기 ([\d.]+)\(수 (\d+) · 최대 ([\d.]+)\) · 방 닫기 ([\d.]+)\(수 (\d+) · 최대 ([\d.]+)\)'
$rxPart = [regex]' · (뒷일|앞|걸음|생각|시전|세계|자기|사람|물체|소환수|정적|묶음|통신 재전송|보내기|넘기기) ([\d.]+)(?= · )'
$rxRecv = [regex]' · 받기 ([\d.]+)\(수 (\d+)\)'
$rxLeave = [regex]' · 끊김 (\d+)\(시간 초과 (\d+) · 0a (\d+)\) · 넘침 (\d+)'
$rxBusy = [regex]' · 지난 ([\d.]+) · 기다림 ([\d.]+) · 바쁨 ([\d.]+)% · 고리 나머지 (-?[\d.]+) · 넘김 지연 평균 ([\d.]+) 최대 (\d+) · 큐 버림 사건 (\d+) 요청 (\d+) · 메모리 개인 ([\d.]+)MB 작업 ([\d.]+)MB · 통신 기다림 ([\d.]+) 바쁨 ([\d.]+)% · 보내기 실패 (\d+)\(마지막 오류 (-?\d+)\)'
# 통신 스레드 몫 — 나머지는 게임 스레드 몫이다(구간 이름은 TickSections.h). 두 스레드는 각자 33ms 를 쓰니 「먼저 차는 구간」은 따로 줄 세운다
$netParts = @('통신 재전송', '보내기', '받기', '넘기기')
# 줄 세우기에서 빼는 것 — 틱합 = 틱 안 11구간(앞~묶음)의 합 · 방 만들기 = 사건 안에 든다(TickSections.h CGameSectionTotals)
$nestedParts = @('틱합', '방 만들기')

function Parse-Section([string]$ln) {
  $h = $rxHead.Match($ln); $b = $rxBusy.Match($ln); $l = $rxLeave.Match($ln); $rv = $rxRecv.Match($ln)
  if (-not ($h.Success -and $b.Success -and $l.Success -and $rv.Success)) { return $null }
  $parts = @{ '틱합' = (D $h.Groups[3].Value); '사건' = (D $h.Groups[5].Value); '방 만들기' = (D $h.Groups[7].Value); '방 닫기' = (D $h.Groups[10].Value); '받기' = (D $rv.Groups[1].Value) }
  foreach ($m in $rxPart.Matches($ln)) { $parts[$m.Groups[1].Value] = D $m.Groups[2].Value }
  return [pscustomobject]@{
    ticks = [int64]$h.Groups[2].Value; made = [int]$h.Groups[8].Value; closed = [int]$h.Groups[11].Value
    parts = $parts; recvCount = [int64]$rv.Groups[2].Value
    leaveTimeout = [int64]$l.Groups[2].Value; leave0a = [int64]$l.Groups[3].Value; overflow = [int64]$l.Groups[4].Value
    elapsed = (D $b.Groups[1].Value); wait = (D $b.Groups[2].Value); rest = (D $b.Groups[4].Value)
    handoffMax = [int]$b.Groups[6].Value; dropEv = [int64]$b.Groups[7].Value; dropReq = [int64]$b.Groups[8].Value
    memPriv = (D $b.Groups[9].Value); netWait = (D $b.Groups[11].Value)
    sendFail = [int64]$b.Groups[13].Value; sendErr = [int]$b.Groups[14].Value
  }
}

# 분포 줄 — 「i:n」 칸(0~99)과 「100이상:n」
function Parse-Hist([string]$ln) {
  $h = New-Object 'int64[]' 101
  foreach ($m in [regex]::Matches($ln, ' (\d+):(\d+)')) { $h[[int]$m.Groups[1].Value] += [int64]$m.Groups[2].Value }
  $o = [regex]::Match($ln, ' 100이상:(\d+)')
  if ($o.Success) { $h[100] += [int64]$o.Groups[1].Value }
  return , $h
}
function Add-Hist([int64[]]$into, [int64[]]$h) { if ($null -ne $h) { for ($i = 0; $i -le 100; $i++) { $into[$i] += $h[$i] } } }
function Sum-Hist([int64[]]$h) { $t = 0L; foreach ($n in $h) { $t += $n }; return $t }
# 가장 느린 1% — 누적이 99% 에 처음 닿는 칸(100 = 100ms 이상). 표본이 없으면 $null
function P99([int64[]]$h) {
  $total = Sum-Hist $h
  if ($total -eq 0) { return $null }
  $need = [math]::Ceiling(0.99 * $total); $acc = 0L
  for ($i = 0; $i -le 100; $i++) { $acc += $h[$i]; if ($acc -ge $need) { return $i } }
  return 100
}

# [구간] 줄 ka+1..kb 와 그 사이 분포 경계(ka < at ≤ kb)의 합 — 측정 구간 · 앞뒤 몫 · 10초 칸이 같은 셈을 쓴다
function Window-Stats($secs, $bounds, [int]$ka, [int]$kb) {
  $elapsed = 0.0; $wait = 0.0; $netWait = 0.0; $rest = 0.0; $ticks = 0L; $recv = 0L; $memMax = 0.0; $handoffMax = 0
  $parts = @{}
  for ($i = $ka; $i -lt $kb; $i++) {                    # 0부터 센 번호 ka..kb-1 = 줄 ka+1..kb
    $s = $secs[$i]
    $elapsed += $s.elapsed; $wait += $s.wait; $netWait += $s.netWait; $rest += $s.rest; $ticks += $s.ticks; $recv += $s.recvCount
    if ($s.memPriv -gt $memMax) { $memMax = $s.memPriv }
    if ($s.handoffMax -gt $handoffMax) { $handoffMax = $s.handoffMax }
    foreach ($k in $s.parts.Keys) { $parts[$k] = [double]$parts[$k] + $s.parts[$k] }
  }
  $tickHist = New-Object 'int64[]' 101; $doneHist = New-Object 'int64[]' 101; $sendHist = New-Object 'int64[]' 101
  $late = 0L; $lateExtra = 0L
  foreach ($b in $bounds) {
    if ($b.at -le $ka -or $b.at -gt $kb) { continue }
    Add-Hist $tickHist $b.tick; Add-Hist $doneHist $b.done; Add-Hist $sendHist $b.send
    $late += $b.late; $lateExtra += $b.lateExtra
  }
  $sec = $elapsed / 1000.0
  $perSec = [ordered]@{}
  foreach ($k in @($parts.Keys | Sort-Object { - $parts[$_] })) { $perSec[$k] = if ($sec -gt 0) { [math]::Round($parts[$k] / $sec, 2) } else { 0 } }
  return [ordered]@{
    windowSec = [math]::Round($sec, 1); ticks = $ticks; recvPieces = $recv
    completionSamples = (Sum-Hist $doneHist); sendSamples = (Sum-Hist $sendHist)
    completionP99Ms = (P99 $doneHist); sendP99Ms = (P99 $sendHist)
    lateRuns = $late; lateExtraTicks = $lateExtra
    gameBusyPct = $(if ($elapsed -gt 0) { [math]::Round(100.0 * ($elapsed - $wait) / $elapsed, 2) } else { $null })
    netBusyPct = $(if ($elapsed -gt 0) { [math]::Round(100.0 * ($elapsed - $netWait) / $elapsed, 2) } else { $null })
    loopRestMsPerSec = $(if ($sec -gt 0) { [math]::Round($rest / $sec, 2) } else { $null })
    partMsPerSec = $perSec; handoffMaxMs = $handoffMax
    memPrivMaxMb = $memMax; memPrivFirstMb = $secs[$ka].memPriv; memPrivLastMb = $secs[$kb - 1].memPriv
    completionHist = $doneHist; sendHist = $sendHist; tickHistSum = (Sum-Hist $tickHist)
  }
}
function Edge-Of($w, [double]$fromMs, [double]$toMs) {
  return [ordered]@{ startSec = [math]::Round($fromMs / 1000.0, 1); endSec = [math]::Round($toMs / 1000.0, 1)
    tickSumMsPerSec = $w.partMsPerSec['틱합']; completionP99Ms = $w.completionP99Ms; sendP99Ms = $w.sendP99Ms
    gameBusyPct = $w.gameBusyPct; netBusyPct = $w.netBusyPct; lateRuns = $w.lateRuns; memPrivLastMb = $w.memPrivLastMb }
}

function Report-Run([string]$dir) {
  $rs = New-Object System.Collections.ArrayList          # 버림 까닭(2-3절)
  $o = [ordered]@{ dir = $dir; tag = (Split-Path -Leaf $dir); usable = $false; reasons = @(); over = @()
    params = [ordered]@{ windowSec = $WindowSec; discardSec = $DiscardSec; overMs = $OverMs; lossPercent = $LossPercent; edgeSec = $EdgeSec } }

  # ① 지문 — PC 검사 · 문턱 · 미커밋
  $envPath = Join-Path $dir 'env.txt'
  if (-not (Test-Path -LiteralPath $envPath)) { $o.reasons = @('env.txt 없음'); return $o }
  $envMap = @{}
  foreach ($ln in [IO.File]::ReadAllLines($envPath, $utf8)) {
    $i = $ln.IndexOf(': ')
    if ($i -gt 0) { $envMap[$ln.Substring(0, $i)] = $ln.Substring($i + 2) }
  }
  $o.commit = [string]$envMap['커밋']
  foreach ($k in @($envMap.Keys)) {
    if ($k.StartsWith('게이트 ') -and $envMap[$k].StartsWith('실패')) { [void]$rs.Add("PC 검사 실패($($k.Substring(4)))") }
  }
  foreach ($n in $gateParams) {
    $k = "문턱 $n"
    if (-not $envMap.ContainsKey($k)) { [void]$rs.Add('문턱 기록 없음(문턱을 적기 전의 load_run)'); break }
    if ($envMap[$k] -ne $defaults[$n]) { [void]$rs.Add("문턱 $n = $($envMap[$k])(기본 $($defaults[$n]))") }
  }
  $dirty = [string]$envMap['미커밋 파일 수']
  if ($dirty -and [int]$dirty -gt 0) { [void]$rs.Add("미커밋 파일 ${dirty}개") }
  $svCmd = [string]$envMap['서버 명령줄']; $duCmd = [string]$envMap['더미 명령줄']
  $m = [regex]::Match($svCmd, '--load-rooms (\d+) --load-room-size (\d+)')
  $o.rooms = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
  $o.roomSize = if ($m.Success) { [int]$m.Groups[2].Value } else { 0 }
  $m = [regex]::Match($duCmd, '--count (\d+)'); $dummyCount = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
  $m = [regex]::Match($duCmd, '--duration-sec (\d+)'); $durSec = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
  $o.dummies = $dummyCount; $o.dummyDurationSec = $durSec
  $o.cpuMhz = [string]$envMap['CPU 속도(MHz) — 실행 중 10초 평균의']

  # ② 서버 출력
  $svPath = Join-Path $dir 'server.txt'; $duPath = Join-Path $dir 'dummy.txt'
  if (-not (Test-Path -LiteralPath $svPath)) { [void]$rs.Add('server.txt 없음'); $o.reasons = @($rs); return $o }
  $secs = New-Object System.Collections.ArrayList
  $blocks = @{}                                         # 분포 경계 = 그때까지 찍힌 [구간] 줄 수(문자열 키)
  $badLines = 0; $endSent = $null; $endDropEv = $null; $endDropReq = $null; $endInput = $null; $endRecvPieces = $null; $endRecvErr = $null
  foreach ($ln in [IO.File]::ReadAllLines($svPath, $utf8)) {
    if ($ln.StartsWith('[구간] ')) {
      $p = Parse-Section $ln
      if ($null -eq $p) { $badLines++ } else { [void]$secs.Add($p) }
      continue
    }
    if ($ln.StartsWith('[틱분포] ') -or $ln.StartsWith('[완료분포] ') -or $ln.StartsWith('[보내기분포] ')) {
      $at = [string]$secs.Count
      if (-not $blocks.ContainsKey($at)) { $blocks[$at] = @{ at = $secs.Count; tick = $null; done = $null; send = $null; late = 0L; lateExtra = 0L } }
      # 같은 경계에 분포가 두 벌일 수 있다 — 마지막 [구간] 이 10번째 줄이면 Print 안에서 한 벌, PrintFinal 이 남은 표본으로 또 한 벌. 덮지 말고 더한다
      $b = $blocks[$at]; $h = Parse-Hist $ln
      $kind = if ($ln.StartsWith('[틱분포]')) { 'tick' } elseif ($ln.StartsWith('[완료분포]')) { 'done' } else { 'send' }
      if ($null -eq $b[$kind]) { $b[$kind] = $h } else { Add-Hist $b[$kind] $h }
      if ($kind -eq 'done') {
        $mm = [regex]::Match($ln, '밀림 (\d+)\(틱 (\d+)\)')
        if ($mm.Success) { $b.late += [int64]$mm.Groups[1].Value; $b.lateExtra += [int64]$mm.Groups[2].Value }
      }
      continue
    }
    $mm = [regex]::Match($ln, '^\[종료\] 끝 — .*보낸 조각 (\d+) · 큐 버림\(이벤트 (\d+) · 요청 (\d+)\)')
    if ($mm.Success) { $endSent = [int64]$mm.Groups[1].Value; $endDropEv = [int64]$mm.Groups[2].Value; $endDropReq = [int64]$mm.Groups[3].Value; continue }
    $mm = [regex]::Match($ln, '^\[종료\] 받은 것 — 입력\(0x3f\) (\d+) · 메시지 전체 (\d+) · 받은 조각 (\d+) · 한 번에 비운 조각 최대 (\d+) · 받기 오류 (\d+)')
    if ($mm.Success) { $endInput = [int64]$mm.Groups[1].Value; $endRecvPieces = [int64]$mm.Groups[3].Value; $endRecvErr = [int64]$mm.Groups[5].Value }
  }
  if ($badLines -gt 0) { [void]$rs.Add("[구간] 줄 ${badLines}개를 못 읽음(옛 꼴이거나 잘림)") }
  if ($null -eq $endSent -or $null -eq $endInput) { [void]$rs.Add('[종료] 줄 없음') }
  $n = $secs.Count
  $T = New-Object 'double[]' ($n + 1)                   # T[k] = k 번째 [구간] 줄 끝의 서버 시각(ms) · T[0] = 0
  for ($k = 1; $k -le $n; $k++) { $T[$k] = $T[$k - 1] + $secs[$k - 1].elapsed }

  # ③ 측정 구간 — 처음 입장의 마지막 방 만들기(첫 방 닫기 앞) + DiscardSec 뒤 첫 분포 경계부터 WindowSec 이 찬 분포 경계까지
  $firstClose = 0
  for ($k = 1; $k -le $n; $k++) { if ($secs[$k - 1].closed -gt 0) { $firstClose = $k; break } }
  $firstMade = 0; $lastMade = 0
  for ($k = 1; $k -le $n; $k++) {
    if ($firstClose -gt 0 -and $k -ge $firstClose) { break }
    if ($secs[$k - 1].made -gt 0) { if ($firstMade -eq 0) { $firstMade = $k }; $lastMade = $k }
  }
  $bounds = @($blocks.Values | Where-Object { $_.at -gt 0 } | Sort-Object { $_.at })
  $w0 = $T[$lastMade] + $DiscardSec * 1000.0
  $ka = $null; $kb = $null
  foreach ($b in $bounds) { if ($T[$b.at] -ge $w0) { $ka = $b.at; break } }
  # 줄 하나의 「지난」은 1000ms 에서 조금 모자랄 수 있다(줄은 ms 시계로 1초를 보고 찍고, 지난 값은 더 고운 시계다) — 길이 비교에 반 줄을 봐준다
  $slack = 500.0
  if ($null -ne $ka) { foreach ($b in $bounds) { if ($b.at -gt $ka -and $T[$b.at] - $T[$ka] -ge $WindowSec * 1000.0 - $slack) { $kb = $b.at; break } } }
  if ($null -eq $kb) {
    [void]$rs.Add("측정 구간 $WindowSec 초를 못 채움(마지막 방 만들기 $([math]::Round($T[$lastMade] / 1000.0, 1))초 · [구간] $n 줄 · 서버 $([math]::Round($T[$n] / 1000.0, 1))초)")
    $o.reasons = @($rs); return $o
  }
  # 더미 시각 = 서버 시각 − Dofs. 첫 방 만들기 줄 안 어딘가가 첫 더미의 입장이다 — 더미가 소켓을 차리는 몫(더미 수에 비례)만큼 앞으로 2초 넓힌다
  $dMin = if ($firstMade -gt 0) { $T[$firstMade - 1] - 2000.0 } else { 0.0 }
  $dMax = if ($firstMade -gt 0) { $T[$firstMade] } else { 0.0 }
  if ($dummyCount -gt 0 -and $durSec -gt 0 -and $T[$kb] -gt $dMin + $durSec * 1000.0) {
    [void]$rs.Add('측정 구간이 더미가 끝나는 때를 넘음(더미가 0a 를 보내는 구간이 섞임)')
  }
  $o.windowStartSec = [math]::Round($T[$ka] / 1000.0, 1); $o.windowEndSec = [math]::Round($T[$kb] / 1000.0, 1)

  # ④ 측정 구간의 합
  $main = Window-Stats $secs $bounds $ka $kb
  if ($main.tickHistSum -ne $main.ticks) { [void]$rs.Add("측정 구간의 [틱분포] 합 $($main.tickHistSum) ≠ [구간] 틱 $($main.ticks)") }
  foreach ($k in @($main.Keys)) { if ($k -ne 'tickHistSum') { $o[$k] = $main[$k] } }
  # 앞뒤 EdgeSec 몫 — load_series 가 M4 확인(5분) · M9(10분)에서 처음과 마지막을 견준다. 경계는 같은 분포 경계에 맞춘다
  if ($EdgeSec -gt 0) {
    $k1 = $null; $k2 = $null
    foreach ($b in $bounds) {
      if ($null -eq $k1 -and $b.at -gt $ka -and $T[$b.at] - $T[$ka] -ge $EdgeSec * 1000.0 - $slack) { $k1 = $b.at }
      if ($b.at -ge $ka -and $b.at -lt $kb -and $T[$kb] - $T[$b.at] -ge $EdgeSec * 1000.0 - $slack) { $k2 = $b.at }   # 조건에 드는 마지막 경계
    }
    if ($null -ne $k1 -and $null -ne $k2 -and $k1 -le $k2) {
      $o.edgeFirst = Edge-Of (Window-Stats $secs $bounds $ka $k1) $T[$ka] $T[$k1]
      $o.edgeLast = Edge-Of (Window-Stats $secs $bounds $k2 $kb) $T[$k2] $T[$kb]
    } else { [void]$rs.Add("앞뒤 $EdgeSec 초가 측정 구간 안에서 겹침") }
  }
  # 10초 칸마다 — 실행 전체(구간 밖도 · inWindow 로 가른다). load_series 가 M1 에서 틱합이 계속 오르는지 · 버리는 구간이 모자란지 본다
  $blockSeries = New-Object System.Collections.ArrayList; $prev = 0
  foreach ($b in $bounds) {
    $w = Window-Stats $secs $bounds $prev $b.at
    [void]$blockSeries.Add([ordered]@{ endSec = [math]::Round($T[$b.at] / 1000.0, 1); inWindow = ($b.at -gt $ka -and $b.at -le $kb)
        tickSumMsPerSec = $w.partMsPerSec['틱합']; gameBusyPct = $w.gameBusyPct; completionP99Ms = $w.completionP99Ms })
    $prev = $b.at
  }
  $o.blockSeries = @($blockSeries)
  $o.lastMadeSec = [math]::Round($T[$lastMade] / 1000.0, 1)

  # ⑤ 실행 전체 — 끊김 · 넘침 · 보내기 실패 · 큐 버림(입장 구간의 것도 판정에 든다 · 2-2절 「3회 중 한 번이라도」)
  $timeouts = 0L; $overflow = 0L; $sendFail = 0L; $lastErr = 0; $left0a = 0L
  foreach ($s in $secs) { $timeouts += $s.leaveTimeout; $overflow += $s.overflow; $sendFail += $s.sendFail; $left0a += $s.leave0a; if ($s.sendFail -gt 0) { $lastErr = $s.sendErr } }
  $o.timeouts = $timeouts; $o.overflow = $overflow; $o.sendFail = $sendFail; $o.sendFailLastError = $lastErr; $o.left0a = $left0a
  $o.queueDrops = if ($null -ne $endDropEv) { $endDropEv + $endDropReq } else { $null }
  $o.serverRecvErrors = $endRecvErr

  # ⑥ 더미 출력
  $sentInput = $null; $recvDgram = $null
  if ($dummyCount -gt 0) {
    if (-not (Test-Path -LiteralPath $duPath)) { [void]$rs.Add('dummy.txt 없음') }
    else {
      $maxEntered = 0; $lateIn = 0L; $sum = $null
      foreach ($ln in [IO.File]::ReadAllLines($duPath, $utf8)) {
        $mm = [regex]::Match($ln, '^\[더미\] 초 (\d+) · 입장 (\d+)/(\d+) .*lateTicks=(\d+)')
        if ($mm.Success) {
          $s = [int]$mm.Groups[1].Value
          if ([int]$mm.Groups[2].Value -gt $maxEntered) { $maxEntered = [int]$mm.Groups[2].Value }
          # 더미 줄 s 는 더미 시각 (s−1, s]초 — 서버 구간과 조금이라도 겹치면 센다
          if ($s * 1000.0 -gt $T[$ka] - $dMax -and ($s - 1) * 1000.0 -lt $T[$kb] - $dMin) { $lateIn += [int64]$mm.Groups[4].Value }
          continue
        }
        if ($ln.StartsWith('[더미 요약] ')) { $sum = $ln }
      }
      if ($null -eq $sum) { [void]$rs.Add('[더미 요약] 줄 없음') }
      else {
        $kv = @{}
        foreach ($mm in [regex]::Matches($sum, ' (\w+)=(\d+)')) { $kv[$mm.Groups[1].Value] = [int64]$mm.Groups[2].Value }
        $mm = [regex]::Match($sum, '입장 (\d+)/(\d+)')
        if ($mm.Success -and [int]$mm.Groups[1].Value -gt $maxEntered) { $maxEntered = [int]$mm.Groups[1].Value }
        $sentInput = $kv['sentInput']; $recvDgram = $kv['recvDgram']
        foreach ($k in @('decodeFail', 'sendFail')) { if ($kv[$k] -gt 0) { [void]$rs.Add("더미 $k $($kv[$k])") } }
        $o.dummyCycleSkipped = $kv['cycleSkipped']
        $o.dummySummary = $kv                             # [더미 요약] 이름=값 전부(load_series 가 M6 빈도 확인에 sentSpell 을 쓴다)
      }
      if ($maxEntered -lt $dummyCount) { [void]$rs.Add("입장 $maxEntered/$dummyCount") }
      $o.dummyLateTicksInWindow = $lateIn
      if ($lateIn -gt 0) { [void]$rs.Add("측정 구간 안 더미 lateTicks $lateIn") }
    }
  }
  $o.inputSent = $sentInput; $o.inputRecv = $endInput
  $o.inputDiffPct = if ($sentInput -gt 0 -and $null -ne $endInput) { [math]::Round(100.0 * [math]::Abs($sentInput - $endInput) / $sentInput, 4) } else { $null }
  $o.piecesSent = $endSent; $o.piecesRecv = $recvDgram
  $o.pieceDiffPct = if ($endSent -gt 0 -and $null -ne $recvDgram) { [math]::Round(100.0 * [math]::Abs($endSent - $recvDgram) / $endSent, 4) } else { $null }

  # ⑦ 넘음(2-2절) — 3회 중앙값 · 한 번이라도는 load_series 가 정한다. 여기는 이 실행 하나의 값
  $over = New-Object System.Collections.ArrayList
  if ($null -ne $o.completionP99Ms -and $o.completionP99Ms -gt $OverMs) { [void]$over.Add('틱 완료') }
  if ($null -ne $o.sendP99Ms -and $o.sendP99Ms -gt $OverMs) { [void]$over.Add('보내는 쪽 지연') }
  if ($timeouts + $overflow -gt 0) { [void]$over.Add('끊김 · 넘침') }
  $lost = ($o.inputDiffPct -gt $LossPercent) -or ($o.pieceDiffPct -gt $LossPercent) -or ($o.queueDrops -gt 0) -or ($sendFail -gt 0)
  if ($lost) { [void]$over.Add('사라짐') }
  $o.over = @($over)
  $o.reasons = @($rs)
  $o.usable = ($rs.Count -eq 0)
  return $o
}

function Fmt($v, [string]$f = '{0}') { if ($null -eq $v) { return '없음' } else { return ($f -f $v) } }

$exit = 0
# -File 로 부르면 「a,b」 가 문자열 하나로 온다 — 쉼표로 나눈다(실행 폴더 이름에는 쉼표가 없다)
foreach ($dir in @($RunDir | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
  if (-not (Test-Path -LiteralPath $dir -PathType Container)) { Write-Host "[판정] 폴더 없음: $dir"; $exit = 2; continue }
  $full = (Resolve-Path -LiteralPath $dir).Path
  $r = Report-Run $full
  [IO.File]::WriteAllText((Join-Path $full 'report.json'), ($r | ConvertTo-Json -Depth 4), $utf8Bom)
  if ($Quiet) { continue }
  $state = if ($r.usable) { '쓸 수 있음' } else { "버림($($r.reasons -join ' · '))" }
  if ($null -eq $r.windowSec) { Write-Host "[판정] $($r.tag) · 방 $($r.rooms) × $($r.roomSize) · $state"; continue }
  $overText = if ($r.over.Count) { "넘음 $($r.over -join ' · ')" } else { '넘음 없음' }
  $keys = @($r.partMsPerSec.Keys | Where-Object { $nestedParts -notcontains $_ })   # 이미 큰 순
  $gameTop = @($keys | Where-Object { $netParts -notcontains $_ } | Select-Object -First 3 | ForEach-Object { "$_ $($r.partMsPerSec[$_])" }) -join ' · '
  $netTop = @($keys | Where-Object { $netParts -contains $_ } | Select-Object -First 2 | ForEach-Object { "$_ $($r.partMsPerSec[$_])" }) -join ' · '
  $top = "틱합 $($r.partMsPerSec['틱합']) · 게임 쪽 $gameTop · 통신 쪽 $netTop"
  Write-Host ("[판정] {0} · 방 {1} × {2} · {3} · {4} · 틱 완료 느린 1% {5} · 보내기 느린 1% {6} · 밀림 {7} · 게임 바쁨 {8}% · 통신 바쁨 {9}% · 고리 나머지 {10}ms/초 · 시간 초과 {11} · 넘침 {12} · 입력 차이 {13}% · 조각 차이 {14}% · 큐 버림 {15} · 보내기 실패 {16} · 메모리 최대 {17}MB · CPU {18} · 구간 {19}~{20}초({21}초 · 틱 {22} · 받은 조각 {23}) · 큰 구간(ms/초) {24}" -f `
    $r.tag, $r.rooms, $r.roomSize, $state, $overText, (Fmt $r.completionP99Ms '{0}ms'), (Fmt $r.sendP99Ms '{0}ms'), $r.lateRuns,
    $r.gameBusyPct, $r.netBusyPct, $r.loopRestMsPerSec, $r.timeouts, $r.overflow, (Fmt $r.inputDiffPct), (Fmt $r.pieceDiffPct),
    (Fmt $r.queueDrops), $r.sendFail, $r.memPrivMaxMb, $(if ($r.cpuMhz) { $r.cpuMhz } else { '기록 없음' }),
    $r.windowStartSec, $r.windowEndSec, $r.windowSec, $r.ticks, $r.recvPieces, $top)
}
exit $exit
