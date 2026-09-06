# walkto.ps1 — 월드 좌표를 목표로 캐릭터를 걸어 보낸다.
#
# 왜 도구로 만드나 — 캡처마다 "저 사물까지 걸어가기"가 필요한데, 그때마다
# 화면 방향을 손으로 짐작하다 두 번 헛걸었다.
#
#   2026-09-07 헛걸음 ①  화면좌표에 ×1.56 을 한 번 더 곱했다.
#                        nav.ps1 은 이미 1280x960 기준이라 1416 이 되어 창 밖으로 나갔다.
#   2026-09-07 헛걸음 ②  화면축을 두 번 재서 축을 세우려 했는데, 둘째 측정이
#                        벽에 막혀(856 vs 140) 90도 어긋난 축이 나왔다.
#
# ★ 그래서 축을 세우지 않는다. **걸어 보고, 어긋난 만큼 각을 돌린다.**
#
# 원리 — 아이소메트릭은 선형 사영이라 화면각 θ 와 월드각 φ 의 차 Δ 가 대체로 일정하다.
#        (완전한 상수는 아니다. 가로세로 배율이 달라 최대 20~30도쯤 흔들린다.
#         그래서 한 번 재고 끝내지 않고, 걸을 때마다 Δ 를 다시 배운다.)
#
#        걸음마다: 실제 간 월드각 − 보낸 화면각 = 그 자리의 Δ. 다음 걸음에 반영한다.
#
# 막힘 처리 — 간 거리가 너무 짧으면 벽이다. Δ 를 배우지 않고(오염된다) 각을 45도 틀어 돈다.
#
# 쓰는 법
#   .\tools\walkto.ps1 -Pid_ <클라PID> -Tx 3411 -Ty 2027 -Near 40
#   .\tools\walkto.ps1 -Pid_ <클라PID> -Type FlameImmobile          # 가장 가까운 그 종류로
param(
  [Parameter(Mandatory)][int]$Pid_,
  [double]$Tx, [double]$Ty,
  [string]$Type,                 # 주면 그 종류 중 가장 가까운 것을 목표로 삼는다
  [double]$Near = 40,            # 이만큼 가까워지면 도착
  [int]$MaxStep = 14,            # 걸음 상한 (무한 배회 방지)
  [double]$Delta = 14,           # 화면각 → 월드각 차의 첫 짐작
                                 # 2026-09-07 8방향 전수 실측 평균(표본 7 · 최대 벗어남 49도).
                                 # ⚠ 상수가 아니다 — 화면각 0도→월드 9도, 90도→135도로
                                 #   구간마다 다르게 늘어난다(아이소 사영의 가로세로 배율 차).
                                 #   그래서 첫 짐작으로만 쓰고 걸음마다 다시 배운다.
  [string]$Api = "http://127.0.0.1:6060/debug/nox/objects",
  [string]$Shot,                 # 주면 걸음마다 화면을 여기에 찍는다
  [switch]$Quiet
)
$ErrorActionPreference = 'Continue'
$nav = Join-Path $PSScriptRoot 'nav.ps1'
$R2D = 180.0 / [math]::PI

function Get-Objs { Invoke-RestMethod -Uri $Api -TimeoutSec 20 }
function Get-Me($o) { $o | Where-Object { $_.type.id -eq 'NewPlayer' } | Select-Object -First 1 }
function Say($s) { if (-not $Quiet) { $s } }

$objs = Get-Objs
$me = Get-Me $objs
if (-not $me) { "ABORT: 플레이어를 못 찾음"; exit 1 }

if ($Type) {
  $t = $objs | Where-Object { $_.type.id -eq $Type -and $_.pos } |
       Sort-Object { [math]::Sqrt([math]::Pow($_.pos.x - $me.pos.x, 2) + [math]::Pow($_.pos.y - $me.pos.y, 2)) } |
       Select-Object -First 1
  if (-not $t) { "ABORT: $Type 을(를) 못 찾음"; exit 1 }
  $Tx = $t.pos.x; $Ty = $t.pos.y
}

Say ("목표 ({0},{1})  ·  지금 ({2},{3})" -f [int]$Tx, [int]$Ty, [int]$me.pos.x, [int]$me.pos.y)

$screenAng = 0.0     # 이번에 보낼 화면각(도). 0 = 화면 오른쪽, 90 = 화면 아래
$blocked = 0

for ($i = 1; $i -le $MaxStep; $i++) {
  $me = Get-Me (Get-Objs)
  if (-not $me) { "  {0}: 플레이어가 사라졌다 (죽었나?)" -f $i; break }
  $ax = $me.pos.x; $ay = $me.pos.y

  $vx = $Tx - $ax; $vy = $Ty - $ay
  $dist = [math]::Sqrt($vx * $vx + $vy * $vy)
  if ($dist -le $Near) { Say ("  {0}: 도착 (거리 {1})" -f $i, [int]$dist); break }

  # 가고 싶은 월드각 → Δ 를 빼서 보낼 화면각을 얻는다
  $wantWorld = [math]::Atan2($vy, $vx) * $R2D
  $screenAng = $wantWorld - $Delta + ($blocked * 45)

  $rad = $screenAng / $R2D
  $r = [math]::Min(300, [math]::Max(120, $dist))       # 커서를 중심에서 이만큼 띄운다
  $cx = [int](640 + $r * [math]::Cos($rad))
  $cy = [int](480 + $r * [math]::Sin($rad))
  # 1280x960 안으로 묶는다 — 밖으로 나가면 게임이 클릭을 안 받는다
  $cx = [math]::Min(1250, [math]::Max(30, $cx))
  $cy = [math]::Min(930,  [math]::Max(30, $cy))

  $hold = [int][math]::Min(1200, [math]::Max(250, $dist * 2.5))
  $out = if ($Shot) { $Shot } else { Join-Path $env:TEMP "walkto_step.png" }
  & $nav -Pid_ $Pid_ -Cx $cx -Cy $cy -Click -Right -Hold $hold -Out $out 2>&1 | Out-Null
  Start-Sleep -Milliseconds 350

  $me2 = Get-Me (Get-Objs)
  if (-not $me2) { "  {0}: 플레이어가 사라졌다 (죽었나?)" -f $i; break }
  $mx = $me2.pos.x - $ax; $my = $me2.pos.y - $ay
  $moved = [math]::Sqrt($mx * $mx + $my * $my)

  if ($moved -lt 25) {
    # 벽이다. Δ 를 배우면 오염되니 배우지 않고 각만 틀어 돈다.
    $blocked++
    Say ("  {0}: 막힘 (간 거리 {1}) — 각을 {2}도 틀어 돈다" -f $i, [int]$moved, ($blocked * 45))
    if ($blocked -ge 8) { Say "  포기: 여덟 방향 모두 막힘"; break }
    continue
  }
  $blocked = 0

  # 실제 간 월드각 − 보낸 화면각 = 이 자리의 Δ. 절반만 반영해 출렁임을 줄인다.
  $gotWorld = [math]::Atan2($my, $mx) * $R2D
  $newDelta = $gotWorld - $screenAng
  while ($newDelta -gt 180) { $newDelta -= 360 }
  while ($newDelta -le -180) { $newDelta += 360 }
  $err = $newDelta - $Delta
  while ($err -gt 180) { $err -= 360 }
  while ($err -le -180) { $err += 360 }
  $Delta = $Delta + $err * 0.5

  Say ("  {0}: 거리 {1} → {2}  ·  간 거리 {3}  ·  Δ {4}도" -f `
       $i, [int]$dist, [int][math]::Sqrt([math]::Pow($Tx - $me2.pos.x, 2) + [math]::Pow($Ty - $me2.pos.y, 2)), `
       [int]$moved, [int]$Delta)
}

$me = Get-Me (Get-Objs)
if ($me) {
  $d = [math]::Sqrt([math]::Pow($Tx - $me.pos.x, 2) + [math]::Pow($Ty - $me.pos.y, 2))
  "끝: ({0},{1})  목표까지 {2}  ·  배운 Δ {3}도" -f [int]$me.pos.x, [int]$me.pos.y, [int]$d, [int]$Delta
} else {
  "끝: 플레이어 없음"
}
