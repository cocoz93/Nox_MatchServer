# walk_genuine.ps1 — **정품 클라를 목표 좌표까지 걸어 보낸다.**
#
# 왜 새로 만드나 — 이미 있는 `walkto.ps1` 은 오픈녹스의 디버그 API
# (`/debug/nox/objects`)로 자기 자리를 읽는다. **정품 클라는 그런 창구가 없다.**
# 대신 **내 서버가 다 알고 있으니** 서버 로그의 `[자리]` 줄을 읽어 각을 잰다.
#
# 조작 — 2026-09-09 실측: **걸음은 오른버튼**이다(키보드는 화살표·WASD 전수 안 먹음).
#   커서가 가리키는 쪽으로 걸으므로, 커서를 화면 가운데에서 원하는 각으로 밀어 두고
#   오른버튼을 잠깐 누른다. 캐릭터는 늘 화면 한가운데다(카메라가 갱신 머리를 따라간다).
#
# ★ 화면각과 월드각의 차(Δ)는 **재지 않고 배운다.** 아이소메트릭은 가로세로 배율이
#   달라 Δ 가 구간마다 20~30도 흔들린다. 한 걸음 걷고 **실제로 간 각**과 보낸 각의
#   차를 다음 걸음에 반영한다(walkto.ps1 이 같은 방식으로 풀었다).
#
#   .\tools\walk_genuine.ps1 -Pid_ 1234 -NetCode 713 -Log ..\_evidence\srv_x.log -Tx 3900 -Ty 2600
param(
  [Parameter(Mandatory)][int]$Pid_,
  [Parameter(Mandatory)][int]$NetCode,
  [Parameter(Mandatory)][string]$Log,
  [Parameter(Mandatory)][double]$Tx,
  [Parameter(Mandatory)][double]$Ty,
  [double]$Near = 35,            # 이만큼 가까워지면 도착
  [int]$MaxStep = 24,            # 걸음 상한 (무한 배회 방지)
  [double]$Delta = 0,            # 화면각 → 월드각 차의 첫 짐작
  [int]$HoldMs = 450,            # 한 걸음에 오른버튼을 누르고 있는 시간
  [double]$Radius = 220,         # 커서를 가운데에서 이만큼 민다 (화면 px)
  # ★ 각도 훑기 — 여덟 방향으로 한 걸음씩 걸어 보고 **화면각 → 월드각** 표를 찍는다.
  #   Δ 를 짐작으로 두지 않고 **재고 들어가려고** 만든 자리다.
  [switch]$Sweep,
  # ★ 감도 훑기 — 커서를 오른쪽으로 조금씩 밀며 방향값이 어떻게 도는지 본다.
  #   인게임 커서 감도를 모르니(메뉴의 2.04 배와 다를 수 있다) **재고 들어간다.**
  [switch]$ScaleProbe,
  # ★ 커서 찍기 — 정해진 주입 좌표로 옮기고 **화면을 찍는다.** 그림에서 커서를 찾아
  #   「주입 단위 → 화면 px」를 **직접** 잰다. 방향값으로 되짚는 간접 추론은
  #   2026-09-10 에 두 번 틀렸다(배율 2.13 이라 했는데 걷기가 계속 어긋났다).
  [string]$CursorProbe = '',
  [string]$ShotDir = '',
  # ★ 판을 실제로 뛴다 — `ctf` 는 적 깃발을 가지러 갔다 집으로, `quest` 는 몬스터를 찾아가 때린다.
  #   `arena` 는 걷지 않고 **상대를 겨누고 친다**.
  #   **정상 배치**(멀리 떨어진 자리)에서도 판이 도는지 보는 자리다.
  [ValidateSet('','ctf','quest','arena')][string]$Play = '',
  [int]$PlaySeconds = 300,
  # ★ 아레나에서 무엇으로 치나 — 직업마다 칸이 다르다(칸은 two_genuine.ps1 의 실측과 같다).
  [ValidateSet('','warrior','wizard','conjurer')][string]$Class = '',
  [switch]$Quiet
)
$ErrorActionPreference = 'Continue'
$di  = Join-Path $PSScriptRoot 'di.ps1'
$R2D = 180.0 / [math]::PI
# ★★★ **인게임 커서 배율은 메뉴와 다르다** — 2026-09-10 커서를 직접 찍어 쟀다.
#
#   주입 (100,60)→화면(148,116) · (200,60)→(271,119) · (300,60)→(393,121) · (400,60)→(514,124)
#   주입 (100,160)→(150,237) · (100,260)→(151,359)
#     ⇒ 가로세로 모두 **1.22 px/단위**, 창 테두리 몫 (26,43)
#
#   ⚠ 메뉴 클릭은 2.04 로 맞춰져 있다(join_my_server.ps1). **같은 값으로 쓰면 안 된다** —
#     2.04 로 두었더니 커서가 늘 화면 왼쪽 위에 머물러 방향값이 136~172 좁은 호에만 있었고,
#     걷기가 다섯 번 어긋났다. 방향값으로 되짚는 간접 추론은 두 번 다 틀린 값을 줬다
#     (3.2 · 2.13). **찍어서 잰 것만 믿는다.**
$PXU = 1.22; $OFFX = 26.0; $OFFY = 43.0
# 캐릭터는 화면 한가운데다(카메라가 갱신 머리를 따라간다). 창 좌표로 잰 값.
$CX = 645.0; $CY = 470.0

function Say($s) { if (-not $Quiet) { Write-Host $s } }

# 서버가 아직 쓰고 있는 파일이라 공유 읽기로 연다
function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  $r = New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)
  $t = $r.ReadToEnd(); $r.Close(); $f.Close(); return $t
}

# 마지막 `[자리]` 줄에서 이 사람의 좌표를 뽑는다
function LastPos() {
  $lines = @((ReadShared $Log) -split "`r?`n" | Where-Object { $_ -match '^\[자리\]' })
  if ($lines.Count -eq 0) { return '' }
  return $lines[-1]
}
function Where-Me() {
  $last = LastPos
  if ($last -eq '') { return $null }
  if ($last -match ("사람 {0}=(-?\d+),(-?\d+),(\d+),(\d+)" -f $NetCode)) {
    return @{ x = [double]$Matches[1]; y = [double]$Matches[2]; facing = [int]$Matches[3]; team = [int]$Matches[4] }
  }
  return $null
}
# 깃발 넷 — 팀 · 지금자리 · 들렸나 · 집자리
function Flags() {
  $out = @()
  foreach ($m in [regex]::Matches((LastPos), '깃발 (\d+)=(-?\d+),(-?\d+),(\d+),(-?\d+),(-?\d+)')) {
    $out += @{ team=[int]$m.Groups[1].Value; x=[double]$m.Groups[2].Value; y=[double]$m.Groups[3].Value
               held=[int]$m.Groups[4].Value; hx=[double]$m.Groups[5].Value; hy=[double]$m.Groups[6].Value }
  }
  return $out
}
function Monsters() {
  $out = @()
  foreach ($m in [regex]::Matches((LastPos), '몸 (\d+)=(-?\d+),(-?\d+)')) {
    $out += @{ code=[int]$m.Groups[1].Value; x=[double]$m.Groups[2].Value; y=[double]$m.Groups[3].Value }
  }
  return $out
}
# 나 아닌 사람 중 가장 가까운 하나 — 아레나의 겨눌 대상이다.
# ⚠ 자리 줄의 사람 항목은 `사람 <번호>=<x>,<y>,<방향>,<팀>` 이다(Server.h PositionsLine).
function Foe($me) {
  $best = $null; $bd = [double]::MaxValue
  foreach ($m in [regex]::Matches((LastPos), '사람 (\d+)=(-?\d+),(-?\d+),(\d+),(\d+)')) {
    $c = [int]$m.Groups[1].Value
    if ($c -eq $NetCode) { continue }
    $x = [double]$m.Groups[2].Value; $y = [double]$m.Groups[3].Value
    $d = [math]::Sqrt([math]::Pow($x - $me.x, 2) + [math]::Pow($y - $me.y, 2))
    if ($d -lt $bd) { $bd = $d; $best = @{ code = $c; x = $x; y = $y; dist = $d } }
  }
  return $best
}
function RoundOver() {
  $l = @((ReadShared $Log) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
  return ($l.Count -gt 0 -and $l[0] -match '판끝 예')
}

# 커서를 화면 가운데에서 screenDeg 쪽으로 밀고 오른버튼을 잠깐 누른다
# 커서를 화면 가운데 기준 screenDeg 쪽 반지름 $Radius 자리로 **옮기기만** 한다.
#
# ⚠⚠ **한 번에 크게 밀면 잘린다.** 2026-09-10 실측 —
#   `m -400 -400` 을 열두 번 보내도 커서가 왼쪽 위에 안 붙었고(보낸 각을 다 돌려도
#   방향값이 137~171 좁은 호에만 머물렀다), 같은 거리를 **25단위씩** 나눠 미니
#   화면 끝까지 제대로 갔다(가로 600단위 · 세로 450단위에 끝 → 2.13 px/단위).
#   그래서 홈으로 몰 때도, 자리로 갈 때도 **한 번에 25단위**를 넘기지 않는다.
#   ⚠⚠ **한 프레임에 몰아넣으면 잘린다.** 큐가 빌 때까지 기다려도 마찬가지였다
#     (2026-09-10: 큐를 다 비운 뒤에도 방향값이 154 근처에 붙박였다).
#     게임이 **프레임마다 쌓인 이동량을 한 번에 처리하며 잘라내는** 것으로 보인다 —
#     같은 거리를 `m 25 0` + 220ms 로 나눠 보내면 화면 끝까지 제대로 갔다.
#     그래서 **이벤트 하나를 한 프레임 간격(40ms)으로** 띄워 보낸다.
$script:CurX = -1; $script:CurY = -1     # 주입 단위로 센 커서 자리 (-1 = 모름)
$STEP = 25; $PACE = 40

function Send-Moves([string[]]$moves) {
  if ($moves.Count -eq 0) { return }
  & $di -Target $Pid_ -Cmd (@('inject 1') + $moves) -GapMs $PACE | Out-Null
}

# 왼쪽 위 구석으로 몰고 자리를 (0,0) 으로 삼는다
function Home-Cursor() {
  $m = @(); foreach ($k in 1..80) { $m += 'm -25 -25' }
  Send-Moves $m
  $script:CurX = 0; $script:CurY = 0
}

# 주입 단위 (ix,iy) 로 옮긴다 — 지금 자리에서의 차이만 보낸다
function Move-To([int]$ix, [int]$iy) {
  if ($script:CurX -lt 0) { Home-Cursor }
  $dx = $ix - $script:CurX; $dy = $iy - $script:CurY
  $m = @()
  while ($dx -ne 0 -or $dy -ne 0) {
    $sx = [math]::Sign($dx) * [math]::Min($STEP, [math]::Abs($dx))
    $sy = [math]::Sign($dy) * [math]::Min($STEP, [math]::Abs($dy))
    $m += ("m {0} {1}" -f $sx, $sy)
    $dx -= $sx; $dy -= $sy
  }
  Send-Moves $m
  $script:CurX = $ix; $script:CurY = $iy
}

function Move-Cursor([double]$screenDeg) {
  $rad = $screenDeg / $R2D
  $sx = $CX + $Radius * [math]::Cos($rad)
  $sy = $CY + $Radius * [math]::Sin($rad)
  Move-To ([int][math]::Round(($sx - $OFFX) / $PXU)) ([int][math]::Round(($sy - $OFFY) / $PXU))
}

# ★★★ **큐가 빌 때까지 기다린다.**
#   게임은 DirectInput 버퍼를 한 프레임에 정해진 개수만 걷어 간다. 백 개를 몰아넣고
#   150ms 만 쉬면 **아직 절반도 안 읽힌 상태**에서 버튼을 누르게 된다 —
#   2026-09-10 실측: 열여섯 방향을 다 돌려도 방향값이 136~172 좁은 호에만 머물렀다.
#   (같은 이동을 25단위씩 `sleep 220` 을 끼워 보내면 화면 끝까지 제대로 갔다.)
#   프록시의 `stat` 이 큐 길이를 알려 주니 그걸로 정확히 기다린다.
function Wait-Drain([int]$maxMs = 4000) {
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalMilliseconds -lt $maxMs) {
    $r = & $di -Target $Pid_ -Cmd @('stat') 2>&1
    $line = ($r | Out-String)
    if ($line -match '큐 mouse=(\d+)') {
      if ([int]$Matches[1] -le 0) { Start-Sleep -Milliseconds 60; return }
    }
    Start-Sleep -Milliseconds 60
  }
}

function Step-Toward([double]$screenDeg) {
  Move-Cursor $screenDeg
  & $di -Target $Pid_ -Cmd @('mb 1 1', ("sleep {0}" -f $HoldMs), 'mb 1 0', 'sleep 260') | Out-Null
}

# ★★ **화면각 → 방향값 표를 직접 잰다.**
#   아이소메트릭은 가로세로 배율이 달라 화면각과 월드각의 차가 **상수가 아니다** —
#   세 귀퉁이 실측이 Δ = -42 / +44 / -97 로 140도나 벌어졌다(2026-09-10).
#   그러니 상수로 보정하지 말고, 열여섯 방향을 **한 번 재서 표로 두고** 되찾아 쓴다.
#   ⚠ 재는 동안에는 오른버튼을 안 누른다 — 사람이 움직이면 표가 오염된다.
$script:CalDeg = @(); $script:CalFacing = @()
function Build-Table() {
  $script:CalDeg = @(); $script:CalFacing = @()
  foreach ($deg in 0..15 | ForEach-Object { $_ * 22.5 }) {
    Move-Cursor $deg
    Start-Sleep -Milliseconds 250
    $m = Where-Me
    if (-not $m) { continue }
    $script:CalDeg += $deg
    $script:CalFacing += $m.facing
  }
  Say ("  화면각→방향값 표 {0}칸: {1}" -f $script:CalDeg.Count, ($script:CalFacing -join ' '))
}

# 원하는 방향값에 가장 가까운 화면각을 표에서 찾는다 (원형 거리)
function ScreenAngleFor([int]$wantFacing) {
  if ($script:CalDeg.Count -eq 0) { return $null }
  $best = 0; $bestD = 999
  for ($i = 0; $i -lt $script:CalDeg.Count; $i++) {
    $d = [math]::Abs($script:CalFacing[$i] - $wantFacing)
    if ($d -gt 128) { $d = 256 - $d }
    if ($d -lt $bestD) { $bestD = $d; $best = $i }
  }
  return $script:CalDeg[$best]
}

# 여덟 방향으로 한 걸음씩 걸어 보고 화면각 → 월드각 표를 찍는다
function Do-Sweep() {
  Say '  화면각 → 월드각 (한 걸음씩)'
  $sum = 0.0; $n = 0
  foreach ($deg in @(0, 45, 90, 135, 180, 225, 270, 315)) {
    $before = Where-Me
    if (-not $before) { continue }
    Step-Toward $deg
    $after = Where-Me
    if (-not $after) { continue }
    $mx = $after.x - $before.x; $my = $after.y - $before.y
    $moved = [math]::Sqrt($mx*$mx + $my*$my)
    if ($moved -lt 6) { Say ("    {0,4}도 → 못 감({1:N0}) · 방향값 {2}" -f $deg, $moved, $after.facing); continue }
    $went = [math]::Atan2($my, $mx) * $R2D
    $d = $went - $deg
    while ($d -gt 180)  { $d -= 360 }
    while ($d -lt -180) { $d += 360 }
    $sum += $d; $n++
    Say ("    {0,4}도 → {1,7:N1}도 (Δ {2,6:N1}) · {3:N0} 감 · 방향값 {4}" -f $deg, $went, $d, $moved, $after.facing)
  }
  if ($n -gt 0) { Say ("  평균 Δ = {0:N1}도 (표본 {1})" -f ($sum / $n), $n) }
  else { Say '  [FAIL] 한 방향도 못 갔다' }
}

$me = Where-Me
if (-not $me) { Say "[FAIL] 서버 로그에서 사람 $NetCode 를 못 찾음"; exit 1 }
if ($CursorProbe -ne '') {
  # "100,100;300,100;500,100;100,300" 꼴. 각 자리로 옮기고 한 장씩 찍는다.
  Home-Cursor
  $n = 0
  foreach ($pt in ($CursorProbe -split ';')) {
    $xy = $pt -split ','
    Move-To ([int]$xy[0]) ([int]$xy[1])
    Start-Sleep -Milliseconds 400
    $n++
    $name = ("cur{0}_{1}_{2}" -f $n, $xy[0], $xy[1])
    [IO.File]::WriteAllText((Join-Path $ShotDir 'cmd.txt'),
      ("target {0}`nshotfull {1}\{2}.png`n" -f $Pid_, $ShotDir, $name),
      (New-Object Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 2
    Say ("  찍음 {0} (주입 {1},{2})" -f $name, $xy[0], $xy[1])
  }
  exit 0
}
if ($ScaleProbe) {
  # 먼저 왼쪽 위로 실컷 몬다. 그다음 오른쪽으로 조금씩 밀며 방향값을 읽는다.
  # ★ 방향값이 **한 바퀴(0~255)를 다 돌면** 커서가 캐릭터를 지나쳐 간 것이고,
  #   좁은 호에만 머물면 커서가 화면 구석에 붙박인 것이다.
  $home = @('inject 1'); foreach ($k in 1..60) { $home += 'm -400 -400' }
  & $di -Target $Pid_ -Cmd ($home + @('sleep 300')) | Out-Null
  $m0 = Where-Me
  Say ("  홈으로 몬 뒤 방향값 {0}" -f $m0.facing)
  # 25단위씩 밀며 방향값이 **더 안 변하는 자리**(화면 끝)를 찾는다.
  #   화면 너비 1280 / 그 단위수 = 한 단위가 몇 px 인가.
  $acc = 0; $prev = $m0.facing; $same = 0; $satX = 0
  foreach ($k in 1..48) {
    & $di -Target $Pid_ -Cmd @('inject 1','m 25 0','sleep 220') | Out-Null
    $acc += 25
    $f = (Where-Me).facing
    if ($f -eq $prev) { $same++ } else { $same = 0 }
    if ($same -eq 0) { Say ("    오른쪽 {0,4} → {1}" -f $acc, $f) }
    if ($same -ge 4 -and $satX -eq 0) { $satX = $acc - 100; Say ("    ↳ 가로 끝 ≈ {0} 단위" -f $satX); break }
    $prev = $f
  }
  # 세로 — 다시 홈으로 몰고 같은 식으로
  & $di -Target $Pid_ -Cmd ($home + @('sleep 300')) | Out-Null
  $m0 = Where-Me
  $acc = 0; $prev = $m0.facing; $same = 0; $satY = 0
  foreach ($k in 1..48) {
    & $di -Target $Pid_ -Cmd @('inject 1','m 0 25','sleep 220') | Out-Null
    $acc += 25
    $f = (Where-Me).facing
    if ($f -eq $prev) { $same++ } else { $same = 0 }
    if ($same -eq 0) { Say ("    아래   {0,4} → {1}" -f $acc, $f) }
    if ($same -ge 4 -and $satY -eq 0) { $satY = $acc - 100; Say ("    ↳ 세로 끝 ≈ {0} 단위" -f $satY); break }
    $prev = $f
  }
  if ($satX -gt 0) { Say ("  가로 배율 ≈ {0:N2} px/단위" -f (1280.0 / $satX)) }
  if ($satY -gt 0) { Say ("  세로 배율 ≈ {0:N2} px/단위" -f (960.0 / $satY)) }
  exit 0
}
if ($Sweep) { Build-Table; Do-Sweep; exit 0 }

# ── 아레나 — 걷지 않고 **겨누고 친다** ───────────────────────
#
# ★★ 왜 생겼나 (2026-09-12) — 회귀에서 wizard 아레나가 **주문 172 발에 명중 0** 이었다.
#   까닭은 서버가 아니라 하네스였다. 옛 싸움 하네스는 **커서를 한 번도 안 움직였고**,
#   그래서 몸 방향이 접속 초기값 66 에 붙박였다(66 × π/128 ≈ 93°, 아래쪽).
#   그런데 상대는 x 축으로 40 떨어진 자리(0° 또는 180°)에 있었다 —
#   서버는 겨눔이 없으면 몸 방향으로 쏘므로(`Server.cpp` `AimRadians`) 늘 빗나갔다.
#   ⭕ 지문 — 자리 줄에 둘 다 `facing 66` 이고 체력이 `75/75` 그대로였다.
#
# ★ 겨눔은 걷기와 같은 길로 한다 — `Build-Table` 이 커서를 열여섯 방향으로 돌려
#   **서버가 되돌려 주는 방향값**으로 표를 만든다. 짐작이 아니라 재는 것이다.
# 직업별 치는 칸 — two_genuine.ps1 의 2026-09-09 실측 칸과 같다.
#   위저드 F=파이어볼(27) · D=연쇄번개(43) / 컨저러 A=임프소환(87) / 워리어 왼버튼
#
# ★★ **왼버튼은 위저드의 공격이 아니다** (2026-09-12). 09-12 회귀에서 wizard 퀘스트가
#   스테이지 2 에서 멈췄는데, 시전된 주문이 전부 **41 = SPELL_LESSER_HEAL**(약한치유)
#   이었다 — 퀘스트 몰이가 왼버튼만 눌렀고 그 자리에 걸린 주문이 치유였던 것이다.
#   몬스터는 한 마리도 안 죽는다. 그래서 아레나·퀘스트 **둘 다** 이 칸을 쓴다.
$hit = switch ($Class) {
  'wizard'   { @('k 0x21 1','sleep 90','k 0x21 0','sleep 400','k 0x20 1','sleep 90','k 0x20 0') }
  'conjurer' { @('k 0x1E 1','sleep 90','k 0x1E 0') }
  default    { @('mb 0 1','sleep 80','mb 0 0') }
}

if ($Play -eq 'arena') {
  Build-Table
  $t0 = Get-Date
  $shots = 0
  while (((Get-Date) - $t0).TotalSeconds -lt $PlaySeconds) {
    if (RoundOver) { Say ('  ★ 판이 끝났다 (친 횟수 {0})' -f $shots); exit 0 }
    $me = Where-Me
    if (-not $me) { Say '  [FAIL] 자리를 잃었다'; exit 1 }
    $foe = Foe $me
    if (-not $foe) { Say '  [FAIL] 상대가 안 보인다'; exit 1 }

    # 상대 쪽 방향값 — 서버와 **같은 눈금**을 쓴다(`facing × π/128`, Server.cpp AimRadians).
    $wantF = [int][math]::Round(([math]::Atan2($foe.y - $me.y, $foe.x - $me.x) * 128.0 / [math]::PI))
    if ($wantF -lt 0) { $wantF += 256 }
    $screenDeg = ScreenAngleFor $wantF
    if ($null -eq $screenDeg) { Say '  [FAIL] 표가 비었다'; exit 1 }
    Move-Cursor $screenDeg
    Start-Sleep -Milliseconds 150

    # ★ 겨눔이 **실제로 돌았는지** 보고 나서 친다. 안 돌았으면 치는 것이 뜻이 없다 —
    #   이 한 줄이 없으면 「172 발 쐈다」가 또 판정으로 둔갑한다.
    $now = Where-Me
    $gap = 999
    if ($now) { $gap = [math]::Abs($now.facing - $wantF); if ($gap -gt 128) { $gap = 256 - $gap } }

    & $di -Target $Pid_ -Cmd (@('inject 1') + $hit) | Out-Null
    $shots++
    Say ("  {0,3}: 거리 {1,5:N0} · 바란방향 {2,3} · 지금 {3,3}(차 {4,3}) · 화면각 {5,5:N0}" -f `
         $shots, $foe.dist, $wantF, $(if ($now) { $now.facing } else { -1 }), $gap, $screenDeg)
    Start-Sleep -Milliseconds 250
  }
  Say ('  시간 끝 (친 횟수 {0})' -f $shots)
  exit 0
}

# ── 판을 실제로 뛴다 ─────────────────────────────────────────
#
# 한 걸음 = 「지금 자리 → 목표」 각을 재고 그쪽으로 오른버튼 한 번.
# 목표는 판마다 다르다 —
#   깃발 : 적 깃발을 안 들었으면 그 자리로, 들었으면 우리 집으로
#   퀘스트: 가장 가까운 몬스터로. 사거리 안이면 왼버튼으로 때린다.
if ($Play -ne '') {
  Build-Table
  $t0 = Get-Date
  $stuck = 0
  while (((Get-Date) - $t0).TotalSeconds -lt $PlaySeconds) {
    if (RoundOver) { Say '  ★ 판이 끝났다'; exit 0 }
    $me = Where-Me
    if (-not $me) { Say '  [FAIL] 자리를 잃었다'; exit 1 }

    $tx = $null; $ty = $null; $what = ''
    if ($Play -eq 'ctf') {
      $fs = Flags
      if ($fs.Count -lt 2) { Say '  [FAIL] 깃발이 안 보인다'; exit 1 }
      $mine = $fs | Where-Object { $_.team -eq $me.team } | Select-Object -First 1
      $foe  = $fs | Where-Object { $_.team -ne $me.team } | Select-Object -First 1
      # 적 깃발이 내 자리에 붙어 오면 내가 든 것이다(들림=1 이고 거리가 가깝다)
      $carrying = ($foe.held -eq 1 -and
                   [math]::Sqrt([math]::Pow($foe.x-$me.x,2)+[math]::Pow($foe.y-$me.y,2)) -lt 20)
      if ($carrying) { $tx = $mine.hx; $ty = $mine.hy; $what = '집으로' }
      else           { $tx = $foe.x;  $ty = $foe.y;  $what = '적 깃발' }
    } else {
      $ms = Monsters
      if ($ms.Count -eq 0) { Say '  몬스터가 없다 — 판이 끝난 듯'; Start-Sleep -Seconds 2; continue }
      $near1 = $ms | Sort-Object { [math]::Sqrt([math]::Pow($_.x-$me.x,2)+[math]::Pow($_.y-$me.y,2)) } | Select-Object -First 1
      $tx = $near1.x; $ty = $near1.y; $what = ('몬스터 {0}' -f $near1.code)
    }

    $dx = $tx - $me.x; $dy = $ty - $me.y
    $dist = [math]::Sqrt($dx*$dx + $dy*$dy)
    if ($Play -eq 'quest' -and $dist -le 40) {
      # 사거리 안 — 때린다. **직업 칸으로** 친다(위 `$hit` 주석 참조).
      # ⚠ 옛 코드는 직업을 안 가리고 왼버튼만 눌렀고, 위저드는 그것이 **치유**였다.
      #
      # ★ 치기 전에 **겨눈다** (2026-09-12). 안 그러면 걸어온 방향 그대로 쏘는데,
      #   몬스터는 움직이므로 어긋난다 — 09-12 시험에서 연쇄번개 242 발에
      #   투사체명중이 **6** 뿐이었다.
      $wf = [int][math]::Round(([math]::Atan2($dy, $dx) * 128.0 / [math]::PI))
      if ($wf -lt 0) { $wf += 256 }
      $sd = ScreenAngleFor $wf
      if ($null -ne $sd) { Move-Cursor $sd; Start-Sleep -Milliseconds 120 }
      & $di -Target $Pid_ -Cmd (@('inject 1') + $hit + @('sleep 450') + $hit) | Out-Null
      Say ("  {0} 사거리 안({1:N0}) — 때림({2}) 겨눔 {3}" -f $what, $dist, $(if ($Class) { $Class } else { '왼버튼' }), $wf)
      continue
    }
    # ★★ 깃발은 **몸이 겹쳐야** 잡힌다 — 서버의 닿는 거리는 20 이다
    #   (사람 반지름 10 + 깃발 반지름 10, `GameBalance.h` kFlagTouchDist).
    #   여기 있던 25 는 **접촉이 40 이던 시절 값**이라, 정본으로 줄이자
    #   24 에서 걸음을 멈추고 영영 기다렸다(2026-09-10 gd2~gd4 세 판 헛돔).
    #   ⚠ 이 값은 서버 상수보다 **작아야** 한다. 크면 안 닿는 자리에서 멈춘다.
    if ($Play -eq 'ctf' -and $dist -le 14) { Start-Sleep -Milliseconds 400; continue }

    $worldDeg = [math]::Atan2($dy, $dx) * $R2D
    $wantF = [int][math]::Round((($worldDeg + 360) % 360) * 256.0 / 360.0) % 256
    $screenDeg = ScreenAngleFor $wantF
    Step-Toward $screenDeg
    $after = Where-Me
    $mv = if ($after) { [math]::Sqrt([math]::Pow($after.x-$me.x,2)+[math]::Pow($after.y-$me.y,2)) } else { 0 }
    # ⚠ 「막혔다」 기준을 6 → 3 으로 내렸다. 정본 이식 뒤 한 걸음이 4~5 로 잘아져
    #   멀쩡히 나아가는데도 막힘으로 세어 열두 번 만에 접혔다(2026-09-10 gd5:
    #   거리가 148→121 로 줄고 있는데 「0 감」이 여섯 줄 이어졌다).
    if ($mv -lt 3) {
      $stuck++
      Step-Toward (($screenDeg + 60) % 360)     # 벽 — 돌아간다
      if ($stuck -ge 12) { Say '  [FAIL] 열두 번 막혔다'; exit 1 }
    } else { $stuck = 0 }
    Say ("  {0} 까지 {1,5:N0} · 화면각 {2,5:N0} · {3:N0} 감" -f $what, $dist, $screenDeg, $mv)
  }
  Say '  [미달] 시간 안에 못 끝냈다'
  exit 1
}
Say ("목표 ({0},{1}) · 지금 ({2},{3})" -f [int]$Tx, [int]$Ty, [int]$me.x, [int]$me.y)
Build-Table

$stuck = 0
for ($i = 1; $i -le $MaxStep; $i++) {
  $me = Where-Me
  if (-not $me) { Say '  [FAIL] 자리를 잃었다'; exit 1 }
  $dx = $Tx - $me.x; $dy = $Ty - $me.y
  $dist = [math]::Sqrt($dx*$dx + $dy*$dy)
  if ($dist -le $Near) { Say ("  도착 — {0} 걸음, 남은 거리 {1:N0}" -f ($i-1), $dist); exit 0 }

  # 월드각 → 방향값(한 바퀴 256) → 표에서 화면각
  $worldDeg = [math]::Atan2($dy, $dx) * $R2D
  $wantF = [int][math]::Round((($worldDeg + 360) % 360) * 256.0 / 360.0) % 256
  $screenDeg = ScreenAngleFor $wantF
  if ($null -eq $screenDeg) { Say '  [FAIL] 표가 비었다'; exit 1 }
  Step-Toward $screenDeg

  $after = Where-Me
  if (-not $after) { continue }
  $mx = $after.x - $me.x; $my = $after.y - $me.y
  $moved = [math]::Sqrt($mx*$mx + $my*$my)
  if ($moved -lt 6) {
    # 벽이다. 표는 건드리지 말고 각을 틀어 돌아간다.
    $stuck++
    $screenDeg = ($screenDeg + 45) % 360
    Step-Toward $screenDeg
    Say ("  {0,2}: 거리 {1,5:N0} · 바란방향 {2,3} · 화면각 {3,5:N0} · 못 감({4:N0}) → 45도 틀어 봄" -f $i, $dist, $wantF, $screenDeg, $moved)
    if ($stuck -ge 8) { Say '  [FAIL] 여덟 번 못 갔다 — 접는다'; exit 1 }
    continue
  }
  $stuck = 0
  $wentDeg = [math]::Atan2($my, $mx) * $R2D
  Say ("  {0,2}: 거리 {1,5:N0} · 바란방향 {2,3} · 화면각 {3,5:N0} · 간각 {4,6:N0} · {5:N0} 감" -f $i, $dist, $wantF, $screenDeg, $wentDeg, $moved)
}
$me = Where-Me
$dx = $Tx - $me.x; $dy = $Ty - $me.y
Say ("  [미달] {0} 걸음 뒤에도 {1:N0} 남음" -f $MaxStep, [math]::Sqrt($dx*$dx + $dy*$dy))
exit 1
