# pickup_sweep.ps1 — **어떤 조작이 줍기(`0x73 TRY_GET`)를 보내는가**를 찾는다.
#
# 왜 만드나 — 서버는 `0x73`·`0x74`·`0x75` 를 받을 준비가 다 돼 있는데(1차 플랜 ③·④)
#   **클라가 무슨 조작에 그걸 보내는지 모른다.** 왼버튼은 아니라고 확인됐다
#   (`Server.h:224`, 물건 위에서 눌러도 안 나갔다 — 2회 시도).
#   그래서 「사람이 5분 눌러 봐야 한다」로 넘겨 뒀는데, **그건 틀렸다.**
#   키를 하나씩 넣는 수단(`di.ps1` 의 `k <스캔코드>`)이 이미 있고,
#   서버에 `[처음] 클라가 보낸 새 오프코드` 줄도 붙였다. 자동으로 찾을 수 있다.
#
# 어떻게 재나 — **물건 위에 서서** 조작을 하나씩 넣고 서버 로그를 본다.
#   ① `[처음]` 줄에 `0x73` 이 뜨면 그 조작이 답이다
#   ② `[물건] 줍기요청 N` 이 오르면 서버가 실제로 처리한 것이다
#   ⚠ 물건 위에 안 서 있으면 클라가 아예 안 보낼 수 있다 — 그래서 먼저 걸어간다.
#     서버가 `[자리]` 줄에 가까운 물건 셋을 찍어 준다(`물건 <번호>=<x>,<y>,<종류>`).
#
#   .\tools\pickup_sweep.ps1 -Tag sw1
#
# 산출물 — `_evidence\sweep_<Tag>.txt` (조작 ↔ 새 오프코드 대응표)
param(
  [Parameter(Mandatory=$true)][string]$Tag,
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map',
  # 한 조작마다 몇 밀리초 누르고 있나. 짧으면 클라가 흘린다.
  [int]$HoldMs = 500,
  # 조작 사이 쉬는 시간. 로그가 따라올 틈을 준다.
  [int]$GapMs = 900
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$slog = Join-Path $ev ("srv_{0}.log" -f $Tag)
$outF = Join-Path $ev ("sweep_{0}.txt" -f $Tag)
$di   = Join-Path $PSScriptRoot 'di.ps1'

function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  try { (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }
}
function Cleanup {
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}

# 서버가 지금까지 본 오프코드 집합 — `[처음]` 줄을 모아 만든다.
function SeenOps() {
  $t = ReadShared $slog
  $s = @{}
  foreach ($m in [regex]::Matches($t, '\[처음\][^\n]*0x([0-9a-f]{2})')) { $s[$m.Groups[1].Value] = $true }
  return $s
}
# `[물건]` 줄의 줍기요청 수
function PickAsks() {
  $t = ReadShared $slog
  $ms = [regex]::Matches($t, '\[물건\] 줍기요청 (\d+)')
  if ($ms.Count -eq 0) { return 0 }
  return [int]$ms[$ms.Count - 1].Groups[1].Value
}
# `[자리]` 줄에서 나와 가장 가까운 물건
function NearestItem() {
  $t = ReadShared $slog
  $lines = @($t -split "`r?`n" | Where-Object { $_ -match '\[자리\]' } | Select-Object -Last 1)
  if ($lines.Count -eq 0) { return $null }
  $m = [regex]::Match($lines[0], '물건 (\d+)=(-?\d+),(-?\d+),(\d+)')
  if (-not $m.Success) { return $null }
  return @{ net=[int]$m.Groups[1].Value; x=[int]$m.Groups[2].Value
            y=[int]$m.Groups[3].Value; type=[int]$m.Groups[4].Value }
}

Cleanup
Start-Sleep -Seconds 2

# ── 1. 서버 (독립 모드 + 맵. ⚠ --map 은 필수다 — 없으면 아무도 못 들어온다) ──
if (Test-Path $slog) { [IO.File]::Delete($slog) }
$srvCmd = 'cmd /c ""' + (Join-Path $root 'build\Release\nox_server.exe') +
          '" --port 18590 --standalone --map "' + $Map + '" --mode arena --aoi --item-at-spawn > "' + $slog + '" 2>&1"'
$sp = Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $srvCmd) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
if ((ReadShared $slog) -notmatch '\[맵\]') {
  Write-Host '[멈춤] 서버 로그에 [맵] 줄이 없다 — 맵을 못 읽었다. 그대로 돌리면 판정이 무효다.' -ForegroundColor Red
  Cleanup; exit 2
}
Write-Host '서버 떴음 (독립 모드 · AOI 켬 — 물건이 보여야 줍기가 성립한다)'

# ── 2. 클라 하나를 게임 안까지 몰기 ──
& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Tag | Out-Null
$cli = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
if ($cli.Count -eq 0) { Write-Host '[멈춤] 클라가 안 떴다'; Cleanup; exit 1 }
$cpid = $cli[0].Id
Write-Host ("클라 pid={0}" -f $cpid)
Start-Sleep -Seconds 3

& $di -Target $cpid -Cmd @('inject 1') | Out-Null

# ── 3. 물건까지 걸어간다 ──
$item = NearestItem
if ($null -eq $item) {
  Write-Host '[멈춤] [자리] 줄에 물건이 없다 — AOI 가 꺼졌거나 맵에 아이템이 없다' -ForegroundColor Red
  Cleanup; exit 2
}
Write-Host ("가장 가까운 물건: 번호 {0} 자리 {1},{2} 종류 {3}" -f $item.net, $item.x, $item.y, $item.type)
# ★ 걷지 않는다 — 서버가 `--item-at-spawn` 으로 아이템을 발밑에 놓아 준다.
#   1차 스윕은 걸어가려다 **127유닛 밖에서 재는 바람에 30가지가 전부 0건**으로 나왔다.
#   그건 「없었다」가 아니라 「못 쟀다」다. 재는 대상은 조작이지 걷기가 아니다.
$posLine = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[자리\]' } | Select-Object -Last 1)
$mMe = [regex]::Match($posLine[0], '사람 \d+=(-?\d+),(-?\d+)')
if (-not $mMe.Success) { Write-Host '[멈춤] [자리] 줄에 사람이 없다'; Cleanup; exit 2 }
$dx = [int]$mMe.Groups[1].Value - $item.x
$dy = [int]$mMe.Groups[2].Value - $item.y
$dist = [Math]::Sqrt($dx*$dx + $dy*$dy)
Write-Host ("물건까지 거리 {0:N0}" -f $dist)
if ($dist -gt 30) {
  Write-Host '[멈춤] 물건 위에 안 서 있다 — 이대로 재면 0건이 「없었다」인지 「못 쟀다」인지 못 가른다' -ForegroundColor Red
  Cleanup; exit 2
}

# ── 4. 조작을 하나씩 넣으며 새 오프코드를 본다 ──
#
# ⚠ **왼버튼은 이미 아니라고 확인됐다** — 그래도 기준선으로 한 번 넣는다.
#   「스윕이 도는가」를 재는 잣대가 있어야 0건을 「없었다」로 읽을 수 있다.
$moves = [ordered]@{}
# 이름 있는 후보부터 — 인벤토리·줍기로 흔히 쓰는 키
$named = [ordered]@{
  'I 인벤토리'=0x17; 'G 줍기?'=0x22; 'SPACE'=0x39; 'ENTER'=0x1C; 'TAB'=0x0F;
  'E'=0x12; 'R'=0x13; 'T'=0x14; 'U'=0x16; 'O'=0x18; 'P'=0x19;
  'H'=0x23; 'J'=0x24; 'K'=0x25; 'L'=0x26; 'B'=0x30; 'N'=0x31; 'M'=0x32;
  'V'=0x2F; 'Y'=0x15; 'CTRL'=0x1D; 'SHIFT'=0x2A; 'ALT'=0x38;
  '1'=0x02; '2'=0x03; '3'=0x04; '4'=0x05; '5'=0x06
}
foreach ($k in $named.Keys) { $moves[$k] = @{ kind='key'; code=$named[$k] } }
$moves['왼버튼(기준선)'] = @{ kind='mb'; code=0 }
$moves['오른버튼']       = @{ kind='mb'; code=1 }
$moves['가운데버튼']     = @{ kind='mb'; code=2 }
$moves['버튼3']          = @{ kind='mb'; code=3 }

$lines = @()
$lines += ("# 줍기 조작 스윕 — {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
$lines += ("# 물건 번호 {0} 자리 {1},{2} 종류 {3}" -f $item.net, $item.x, $item.y, $item.type)
$lines += '# 조작 | 새로 나온 오프코드 | 줍기요청 증가'
# ★ 기준선 지표 = **클릭**(`[틱]` 줄의 `클릭 N`). 왼버튼을 누르면 반드시 오른다.
#   1차 스윕은 기준선을 「새 오프코드」로 쟀는데, 왼버튼이 만드는 `0x3f` 는
#   접속 직후 이미 나와서 **「새 것」이 안 된다.** 잣대 자체가 안 움직였던 것이다.
function Clicks() {
  $l = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
  if ($l.Count -eq 0) { return 0 }
  if ($l[0] -match '클릭 (\d+)') { return [int]$Matches[1] }
  return 0
}
$seen = SeenOps
$asks = PickAsks
$clk  = Clicks
$hit = $null
$baseMoved = $false

foreach ($name in $moves.Keys) {
  $mv = $moves[$name]
  if ($mv.kind -eq 'key') {
    & $di -Target $cpid -Cmd @(("k {0} 1" -f $mv.code), ("sleep {0}" -f $HoldMs),
                               ("k {0} 0" -f $mv.code), ("sleep {0}" -f $GapMs)) | Out-Null
  } else {
    & $di -Target $cpid -Cmd @(("mb {0} 1" -f $mv.code), ("sleep {0}" -f $HoldMs),
                               ("mb {0} 0" -f $mv.code), ("sleep {0}" -f $GapMs)) | Out-Null
  }
  $now = SeenOps
  $new = @($now.Keys | Where-Object { -not $seen.ContainsKey($_) } | Sort-Object)
  $nowAsks = PickAsks
  $dAsk = $nowAsks - $asks
  $nowClk = Clicks
  $dClk = $nowClk - $clk
  if ($dClk -gt 0) { $baseMoved = $true }
  $lines += ("{0,-16} | {1,-24} | +{2}" -f $name, ($(if ($new.Count) { $new -join ' ' } else { '(없음)' })), $dAsk)
  Write-Host ("  {0,-16} 새 오프코드 {1,-22} 줍기요청 +{2} · 클릭 +{3}" -f
    $name, $(if ($new.Count) { $new -join ' ' } else { '(없음)' }), $dAsk, $dClk)
  if ($new -contains '73' -or $dAsk -gt 0) {
    $hit = $name
    Write-Host ("  ★★ 찾았다 — 「{0}」 가 줍기를 보낸다" -f $name) -ForegroundColor Green
  }
  $seen = $now
  $asks = $nowAsks
  $clk = $nowClk
}


# ── 5. ★ 커서를 물건 위에 올리고 왼클릭 ─────────────────────────────
#
#   ★★ 근거 — 게임 폴더 `nox.cfg` 에 **`MousePickup = Left`** 라고 적혀 있다.
#     그리고 `MOUSE_BUTTON_LEFT = Action` 이다. 즉 왼버튼 하나가
#     **커서 아래에 무엇이 있느냐**에 따라 공격도 되고 줍기도 된다.
#     4단계에서 왼버튼이 공격(클릭 +1)만 만든 것은 커서가 물건을 안 가리켰기 때문이다.
#
#   ⚠ 커서는 **델타로만** 움직인다(절대 좌표가 없다). 한 번에 크게 밀면 게임이 잘라내므로
#     25단위씩 40ms 간격으로 나눠 보낸다(walk_genuine 실측 규칙 그대로).
#     배율 ≈ 2.13 px/단위 · 화면 가운데(캐릭터 자리) = 645,470 px.
$STEP = 25; $PACE = 40
$curX = -1; $curY = -1
function SendMoves([string[]]$mv) {
  if ($mv.Count -eq 0) { return }
  & $di -Target $cpid -Cmd (@('inject 1') + $mv) -GapMs $PACE | Out-Null
}
function HomeCursor() {
  $m = @(); foreach ($k in 1..80) { $m += 'm -25 -25' }
  SendMoves $m; $script:curX = 0; $script:curY = 0
}
function MoveTo([int]$ix, [int]$iy) {
  if ($script:curX -lt 0) { HomeCursor }
  $dx = $ix - $script:curX; $dy = $iy - $script:curY
  $m = @()
  while ($dx -ne 0 -or $dy -ne 0) {
    $sx = [math]::Sign($dx) * [math]::Min($STEP, [math]::Abs($dx))
    $sy = [math]::Sign($dy) * [math]::Min($STEP, [math]::Abs($dy))
    $m += ("m {0} {1}" -f $sx, $sy); $dx -= $sx; $dy -= $sy
  }
  SendMoves $m; $script:curX = $ix; $script:curY = $iy
}

Write-Host '--- 커서를 물건 위에 올리고 왼클릭 (nox.cfg: MousePickup = Left) ---'
$SCALE = 2.13
# 캐릭터(=물건) 자리 둘레를 훑는다. 물건이 몸에 가려 조금 어긋나 그려질 수 있다.
$grid = @()
foreach ($oy in @(0, -30, 30, -60, 60)) { foreach ($ox in @(0, -30, 30)) { $grid += ,@($ox, $oy) } }
foreach ($g in $grid) {
  $px = 645 + $g[0]; $py = 470 + $g[1]
  MoveTo ([int]($px / $SCALE)) ([int]($py / $SCALE))
  & $di -Target $cpid -Cmd @('mb 0 1', ("sleep {0}" -f $HoldMs), 'mb 0 0', ("sleep {0}" -f $GapMs)) | Out-Null
  $now = SeenOps
  $new = @($now.Keys | Where-Object { -not $seen.ContainsKey($_) } | Sort-Object)
  $nowAsks = PickAsks; $dAsk = $nowAsks - $asks
  $nowClk = Clicks; $dClk = $nowClk - $clk
  if ($dClk -gt 0) { $baseMoved = $true }
  $label = ("커서{0},{1}+왼클릭" -f $g[0], $g[1])
  $lines += ("{0,-16} | {1,-24} | +{2}" -f $label, ($(if ($new.Count) { $new -join ' ' } else { '(없음)' })), $dAsk)
  Write-Host ("  {0,-16} 새 오프코드 {1,-22} 줍기요청 +{2} · 클릭 +{3}" -f $label, $(if ($new.Count) { $new -join ' ' } else { '(없음)' }), $dAsk, $dClk)
  if ($new -contains '73' -or $dAsk -gt 0) {
    $hit = $label
    Write-Host ("  ★★ 찾았다 — 「{0}」 가 줍기를 보낸다" -f $label) -ForegroundColor Green
    break
  }
  $seen = $now; $asks = $nowAsks; $clk = $nowClk
}
$lines += ''
if ($hit) { $lines += ("결과: **{0}** 가 0x73 을 보낸다" -f $hit) }
else      { $lines += '결과: 이 조작 목록에서는 0x73 이 안 나왔다 — 목록을 넓히거나 사람에게 넘긴다' }
[IO.File]::WriteAllLines($outF, $lines, [Text.UTF8Encoding]::new($false))
Write-Host ''
Write-Host ('=== 판정 ===')
if ($hit) { Write-Host ("★ 줍기 조작 = 「{0}」" -f $hit) -ForegroundColor Green }
elseif (-not $baseMoved) {
  Write-Host '⚠ **판정 무효** — 기준선(왼버튼→클릭)조차 안 움직였다.' -ForegroundColor Red
  Write-Host '   0건은 「없었다」가 아니라 「못 쟀다」다. 주입이 안 먹는 것부터 볼 것.'
} else {
  Write-Host '이 목록에서는 못 찾았다 (기준선은 움직였으니 측정은 유효) — 미결로 적고 다음 조각으로' -ForegroundColor Yellow
}
Write-Host ("표: {0}" -f $outF)
Cleanup
