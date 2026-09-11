# anim_pose.ps1 — **걷는 그림 칸(0x6B)이 실제로 자세를 바꾸나**를 가른다.
#
# 왜 이 도구가 따로 있나 (2026-09-11) —
#   걸으면서 찍은 캡처 세 장 중 둘에서 **캐릭터가 나무 뒤로 가 버렸다.**
#   원인은 간격이 아니라 **캡처 자체가 2초 걸린다**는 것이었다(graduation.ps1:61
#   `Start-Sleep -Seconds 2`). 「0.25초 간격」이라 적어 놓고 실제로는 2.25초였다.
#   ★ 또 **계측의 지연을 안 쟀다** — 3차 G1 에서 얻은 교훈을 그대로 다시 밟았다.
#
# 그래서 수단을 바꾼다 — **움직이지 않고 칸만 바꾼다.**
#   서버 `--anim-frame N` 으로 칸을 한 값에 못 박고, 클라는 **가만히 선 채로** 찍는다.
#   위치 변화가 0 이라 두 장을 같은 자리에서 나란히 견줄 수 있다.
#
# 판정 — 칸 0 과 칸 2 의 캐릭터 픽셀이 다르면 `0x6B` 가 먹은 것이다.
#   같으면 클라가 이 경로를 안 본다는 뜻이고, 갱신 스트림 bit7 쪽을 봐야 한다.
#
#   .\tools\anim_pose.ps1 -Frame 0 -Tag p0
#   .\tools\anim_pose.ps1 -Frame 2 -Tag p2
param(
  [int]$Frame = -1,
  # 사람 **자세 번호**를 못 박는다. 0 서기 · 2 죽음(눕기) · 4 걷기.
  # ★ 걷기(4)는 서기와 그림 차이가 작다. **죽음(2)으로 먼저 가른다** — 먹으면 눕는다.
  [int]$Pose = -1,
  # 연달아 두 장 찍는다 — 서 있는데도 달라지면 **애니가 도는 것**이다.
  [switch]$Twice,
  [string]$Tag = 'pose',
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$slog = Join-Path $root ("_evidence\srv_{0}.log" -f $Tag)
$ctrl = Join-Path $root '_evidence\desk1'
$di   = Join-Path $PSScriptRoot 'di.ps1'

Write-Host ("[조건] 자세={0} · 그림칸={1} · 꼬리표={2} · 안 걷는다" -f $Pose, $Frame, $Tag) -ForegroundColor Cyan

function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  try { return (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }
}
function Cleanup {
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}
Cleanup
Start-Sleep -Seconds 3
if (Test-Path $slog) { [IO.File]::Delete($slog) }

$exe = Join-Path $root 'build\Release\nox_server.exe'
$args = '--port 18590 --standalone --map "' + $Map + '" --mode arena'
if ($Frame -ge 0) { $args += ' --anim-frame ' + $Frame }
if ($Pose  -ge 0) { $args += ' --anim-pose '  + $Pose  }
$cmd = 'cmd /c ""' + $exe + '" ' + $args + ' > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $cmd) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
if ((ReadShared $slog) -notmatch '\[맵\]') { Write-Host '[멈춤] 서버가 맵을 못 읽었다' -ForegroundColor Red; Cleanup; exit 6 }
Write-Host '서버 떴음'

& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Tag -CharRow 1 |
  Select-String -Pattern '픽 확인' | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
$p = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
if ($p.Count -eq 0) { Write-Host '[멈춤] 클라가 안 떴다' -ForegroundColor Red; Cleanup; exit 6 }
& $di -Target $p[0].Id -Cmd @('inject 1') | Out-Null
Start-Sleep -Seconds 4

# ★ 안 걷는다. 자세는 서버가 못 박아 두었으므로 서 있어도 그 자세가 방송된다.
#
# ★★ `-Twice` 면 **연달아 두 장**을 찍는다. 캐릭터가 안 움직이므로 배경도 그대로다 —
#   그래서 두 장이 다르면 그것은 **애니메이션이 돌고 있다**는 뜻뿐이다.
#   걷기(WALK)가 `AnimLoop` 면 프레임이 시간과 함께 계속 굴러간다
#   (`opennox/src/drawable_vector.go:119` — `AnimStart` 를 안 쓰고 `Frame()` 으로 나눈다).
$shots = @("{0}_stand.png" -f $Tag)
if ($Twice) { $shots += ("{0}_stand2.png" -f $Tag) }
$png = $null
foreach ($nm in $shots) {
  $f = Join-Path $ctrl $nm
  if (Test-Path $f) { [IO.File]::Delete($f) }
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'), ("shotfull {0}`n" -f $f),
                          (New-Object Text.UTF8Encoding($false)))
  Start-Sleep -Seconds 3
  if ($png -eq $null) { $png = $f }
}

$t = ReadShared $slog
$tick = @(($t -split "`r?`n") | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
Write-Host ''
Write-Host '=== 결과 ==='
if ($tick.Count -gt 0) { Write-Host ("  " + $tick[0].Trim()) }
# ★ 자리를 같이 찍는다 — 두 런의 좌표가 다르면 캡처를 나란히 견줄 수 없다.
$pos = @(($t -split "`r?`n") | Where-Object { $_ -match '\[자리\]' } | Select-Object -Last 1)
if ($pos.Count -gt 0) { Write-Host ("  " + $pos[0].Trim()) }
if (Test-Path $png) {
  Write-Host ("  캡처 {0} ({1:N0} 바이트)" -f (Split-Path -Leaf $png), (Get-Item $png).Length) -ForegroundColor Green
} else {
  Write-Host '  [멈춤] 캡처가 안 나왔다' -ForegroundColor Red; Cleanup; exit 7
}
Cleanup
exit 0
