# graduation.ps1 — **「사람이 5분 놀 수 있나」를 재는 하나의 시험.**
#
# 왜 있나 (2026-09-11) — 2차 플랜은 조각 8개를 각각 닫고 「완료」라고 했는데,
#   사용자가 5분 플레이하자 **애니메이션 없음 · 마나칸 0 · 주문 쓰면 튕김 ·
#   재접속 불가**가 한 번에 나왔다. 조각마다의 닫힘 조건은 다 통과했는데
#   **그 합이 목표인지는 아무도 안 쟀다.**
#
# 그래서 이 시험은 **조각이 아니라 목표**를 잰다. 사람이 하는 것과 같은 순서로 —
#
#     접속 → 걷기 → **쓸 수 있는 주문 전부** → 나가기 → 재접속
#
# ★★ 「주문 전부」가 핵심이다. 2차 회귀는 위저드로 돌려 **F·D 둘만** 눌렀고,
#    소환(컨저러 A, 87 SUMMON_IMP)은 **한 번도 안 눌렀다.** 사용자가 첫 판에 그것을
#    눌러 튕겼다. 범위 밖은 아무도 안 본다 — 그래서 여기서는 **다 누른다.**
#
# ★ 주문마다 **누른 직후 생존을 확인**한다. 그래야 「어느 주문이 죽이는지」가 짚인다.
#
#   .\tools\graduation.ps1 -Tag g1
#
# 종료 코드 — 0 통과 · 3 클라 사망 · 4 멈춤 · 5 재접속 실패 · 6 접속 자체 실패
param(
  [Parameter(Mandatory=$true)][string]$Tag,
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map',
  # 어느 직업으로 볼까. **컨저러가 기본이다** — 소환이 거기 있다.
  [ValidateSet('conjurer','wizard','warrior')][string]$Class = 'conjurer',
  [int]$WaitMenu = 40
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$ctrl = Join-Path $ev 'desk1'
$slog = Join-Path $ev ("srv_{0}.log" -f $Tag)
$di   = Join-Path $PSScriptRoot 'di.ps1'

function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  try { (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }
}
function Alive([int]$procId) {
  if ($procId -le 0) { return $false }
  return @(Get-Process -Id $procId -ErrorAction SilentlyContinue).Count -gt 0
}
function Cleanup {
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}
# 숨긴 데스크톱이라 평범한 창 캡처가 안 먹는다 — 제어 폴더에 명령을 써서 deskhost 가 찍는다.
function Shot([string]$name) {
  $png = Join-Path $ctrl ("{0}_{1}.png" -f $Tag, $name)
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'),
                          ("shotfull {0}`n" -f $png), (New-Object Text.UTF8Encoding($false)))
  Start-Sleep -Seconds 2
  return (Test-Path $png)
}
# 서버가 스스로 말하는 값 하나
function TickNum([string]$key) {
  $l = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
  if ($l.Count -eq 0) { return -1 }
  if ($l[0] -match ($key + ' (\d+)')) { return [int]$Matches[1] }
  return -1
}

$fail = @()          # 여기 쌓인 것이 곧 판정이다
$shots = @()
$code = 0

Cleanup
Start-Sleep -Seconds 2
if (-not (Test-Path $ctrl)) { New-Item -ItemType Directory $ctrl | Out-Null }

# ── 1. 서버 ───────────────────────────────────────────────────────────
if (Test-Path $slog) { [IO.File]::Delete($slog) }
$srv = 'cmd /c ""' + (Join-Path $root 'build\Release\nox_server.exe') +
       '" --port 18590 --standalone --map "' + $Map + '" --mode arena > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $srv) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
if ((ReadShared $slog) -notmatch '\[맵\]') {
  Write-Host '[멈춤] 서버 로그에 [맵] 줄이 없다 — 맵을 못 읽었다' -ForegroundColor Red
  Cleanup; exit 6
}
Write-Host ("서버 떴음 · 직업 {0}" -f $Class)

# ── 2. 접속 ───────────────────────────────────────────────────────────
$row = switch ($Class) { 'conjurer' { 1 } 'wizard' { 2 } default { 3 } }
& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Tag -CharRow $row -WaitMenu $WaitMenu | Out-Null
$cli = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
if ($cli.Count -eq 0) { Write-Host '[멈춤] 클라가 안 떴다' -ForegroundColor Red; Cleanup; exit 6 }
$pid1 = $cli[0].Id
Start-Sleep -Seconds 3
& $di -Target $pid1 -Cmd @('inject 1') | Out-Null

if ((TickNum '사람') -lt 1) { $fail += '접속은 했는데 서버가 사람을 0 으로 센다' }
if (Shot 'A_join') { $shots += 'A_join' }   # ★ HUD — 체력바·마나바가 여기 보여야 한다

# ── 3. 걷기 ───────────────────────────────────────────────────────────
#   ★ 캡처를 **두 장** 찍는다 — 애니메이션은 한 장으로 못 본다.
#   ⚠ **화살표키로는 안 걷는다** — 녹스의 걸음은 **오른버튼을 누르고 있는 것**이다
#     (`two_genuine.ps1` 의 걷기도 `mb 1` 을 쓴다). 첫 회차에 화살표를 눌러 `걸음 +0` 이
#     나왔고, 그것이 「걷기가 안 된다」로 잘못 읽힐 뻔했다 — 조종이 틀린 것이었다.
$beforeWalk = TickNum '걸음'
& $di -Target $pid1 -Cmd @('mb 1 1','sleep 900') | Out-Null
if (Shot 'B_walk1') { $shots += 'B_walk1' }    # 걷는 도중 ①
& $di -Target $pid1 -Cmd @('sleep 900') | Out-Null
if (Shot 'C_walk2') { $shots += 'C_walk2' }    # 걷는 도중 ② — ①과 자세가 달라야 한다
# ⚠ **오른버튼을 확실히 놓고 충분히 쉰다.**
#   단서 — 걷기가 **실패한** 회차(g1)에서는 주문이 +1 나갔고, 실제로 걸은 회차부터
#   주문이 전부 +0 이었다. 클라가 아직 「이동 중」이면 시전이 안 먹는 것으로 의심된다.
& $di -Target $pid1 -Cmd @('mb 1 0','sleep 400','mb 1 0','sleep 1200') | Out-Null
$walked = (TickNum '걸음') - $beforeWalk
Write-Host ("걸음 +{0}" -f $walked)
if ($walked -le 0) { $fail += '걷기 입력이 서버에 안 닿는다' }
if (-not (Alive $pid1)) { $fail += '걷다가 클라가 죽었다'; $code = 3 }

# ── 4. ★★ 주문 전부 — 하나씩 누르고 **그때마다 생존 확인** ────────────
#
#   2026-09-09 실측 칸 —
#     컨저러 A 87 임프소환 · S 74 기절 · D 128 독구름 · F 58 요정떼 · G 41 약한치유
#     위저드 A 4 점멸 · S 51 보호막 · D 43 연쇄번개 · F 27 파이어볼 · G 41 약한치유
# ★★ **소환을 맨 뒤로 돌린다** (2026-09-11).
#   g8 에서 밝혀진 것 — 컨저러 A(87 임프소환)를 누르자 시전은 **통과**했는데
#   바로 다음에 클라가 `0xae`(나가기)를 보냈다. 즉 **주문을 쓰자 게임에서 나갔다** —
#   사용자가 말한 「주문 쓰니까 튕김」이 이것이다.
#   A 가 첫 칸이라 거기서 죽어 **나머지 네 칸을 한 번도 못 쟀다.**
#   → 소환이 아닌 것부터 눌러 「소환만 죽이나 주문 전반이 죽나」를 가른다.
#      컨저러 기준 G 41 약한치유 · F 58 요정떼 · D 128 독구름 · S 74 기절 · **A 87 소환**
$slots = [ordered]@{ 'G'=0x22; 'F'=0x21; 'D'=0x20; 'S'=0x1F; 'A'=0x1E }
$killer = ''
foreach ($k in $slots.Keys) {
  if (-not (Alive $pid1)) { break }
  $sp0 = TickNum '주문'
  & $di -Target $pid1 -Cmd @(("k {0} 1" -f $slots[$k]), 'sleep 120', ("k {0} 0" -f $slots[$k]), 'sleep 1500') | Out-Null
  $sp1 = TickNum '주문'
  # ★★ **프로세스가 살아 있어도 「게임에서 나갔으면」 죽은 것이다** (2026-09-11).
  #   g8 에서 소환을 쓰자 클라가 `0xae`(나가기)를 보내고 서버가 세션을 걷었는데,
  #   프로세스는 멀쩡해서 하네스가 **「클라 살아있음」이라 찍었다.** 그게 이 시험을
  #   여섯 판이나 헛돌게 했다 — 사람이 보면 「튕겼다」인데 계측은 「살아있다」였다.
  $gone = ((ReadShared $slog) -match '조용해서 내보냄')
  $ok = (Alive $pid1) -and (-not $gone)
  Write-Host ("  주문칸 {0}  주문 +{1}  클라 {2}" -f $k, ($sp1 - $sp0),
              $(if ($ok) { '살아있음' } elseif ($gone) { '★게임에서 나감★' } else { '★죽음★' }))
  if (-not $ok) {
    # ★ 여기가 이 시험의 값어치다 — **어느 주문이 죽이는지** 짚어낸다
    $killer = $k
    $fail += ("주문칸 {0} 를 누르자 클라가 죽었다" -f $k)
    $code = 3
    break
  }
}
if ($killer -eq '' -and (Alive $pid1)) {
  if (Shot 'D_spells') { $shots += 'D_spells' }
}

# ── 4.5 ★★ 맞고 죽기 → 부활 ──────────────────────────────────────────
#
# ★ 왜 이 단계가 중요한가 (2026-09-11) — 첫 회차에서 소환 주문을 눌러도 **안 튕겼다.**
#   그래서 「소환수 소개가 클라를 죽인다」는 내 첫 진단이 **틀렸다.**
#   사용자 로그와 대조하니 둘 다 `주문 1 · 나는것 0 · 피해 N(투사체명중 0)` 으로 같고
#   **다른 것은 시간뿐**이었다 — 사용자 28, 내 회차 22. 사용자는 더 오래 맞았다.
#   → 튕김은 소환 그 자체가 아니라 **맞아 죽는 순간**일 공산이 크다.
#
# ⚠ 지금은 **소환수가 주인을 때린다**(그 자체가 결함이다). 그래서 가만히 있으면 죽는다 —
#   이 결함이 고쳐지면 이 단계는 둘째 클라나 몬스터로 바꿔야 한다.
if ($code -eq 0 -and (Alive $pid1)) {
  Write-Host '--- 맞고 죽기 → 부활 (최대 90초) ---'
  $dmg0 = TickNum '피해'
  $died = $false
  for ($w = 0; $w -lt 90; $w += 5) {
    Start-Sleep -Seconds 5
    if (-not (Alive $pid1)) {
      $fail += ("맞는 도중 클라가 죽었다 — 피해 {0} 쌓인 뒤 (주문이 아니라 **죽음**이 튕김의 자리일 수 있다)" -f ((TickNum '피해') - $dmg0))
      $code = 3; $died = $true; break
    }
  }
  $dmg1 = TickNum '피해'
  Write-Host ("  피해 +{0} · 클라 {1}" -f ($dmg1 - $dmg0), $(if (Alive $pid1) { '살아있음' } else { '★죽음★' }))
  if (-not $died) {
    if (($dmg1 - $dmg0) -le 0) { $fail += '90초 동안 아무 피해도 안 들어왔다 — 죽음·부활을 못 쟀다' }
    if (Shot 'D2_hurt') { $shots += 'D2_hurt' }
  }
}

# ── 5. 나가기 → 재접속 ────────────────────────────────────────────────
#   사람이 알트F4 로 끄고 다시 들어오는 것과 같은 꼴이다.
if ($code -eq 0) {
  Write-Host '--- 나가기 → 재접속 ---'
  Stop-Process -Id $pid1 -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 8
  & (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag ("{0}re" -f $Tag) -CharRow $row -WaitMenu $WaitMenu | Out-Null
  $cli2 = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
  if ($cli2.Count -eq 0) {
    $fail += '재접속 — 클라가 다시 안 떴다'; $code = 5
  } else {
    $pid2 = $cli2[0].Id
    Start-Sleep -Seconds 5
    $people = TickNum '사람'
    Write-Host ("재접속 뒤 서버가 세는 사람 {0}" -f $people)
    # ★ 1 이어야 한다. 2 면 **옛 세션이 안 걷힌 것**(유령이 남았다) —
    #   사용자가 겪은 「재접속 안 됨」의 정체가 이것으로 의심된다.
    if ($people -ne 1) { $fail += ("재접속 뒤 사람이 {0} 명 — 옛 세션이 안 걷혔다" -f $people); $code = 5 }
    if (-not (Alive $pid2)) { $fail += '재접속 직후 클라가 죽었다'; $code = 5 }
    if (Shot 'E_rejoin') { $shots += 'E_rejoin' }
  }
}

# ── 6. 멈춤·버림 ──────────────────────────────────────────────────────
$logText = ReadShared $slog
$recv = @([regex]::Matches($logText, '\[받음 \d+\][^\r\n]*?\s(\d+)바이트'))
if ($recv.Count -ge 40) {
  $small = 0
  for ($i = $recv.Count - 30; $i -lt $recv.Count; $i++) {
    if ([int]$recv[$i].Groups[1].Value -le 3) { $small++ }
  }
  if ($small -eq 30) { $fail += '클라가 멈췄다 — 마지막 수신 30회가 전부 3바이트'; if ($code -eq 0) { $code = 4 } }
}
$drops = @([regex]::Matches($logText, '\[버림\]')).Count
if ($drops -gt 0) { $fail += ("서버가 클라 조각을 {0}건 버렸다" -f $drops) }

# ── 7. 판정 ───────────────────────────────────────────────────────────
Write-Host ''
Write-Host '================= 졸업 시험 ================='
if ($fail.Count -eq 0) {
  Write-Host '✅ 통과 — 접속·걷기·주문 전부·재접속이 다 됐다' -ForegroundColor Green
} else {
  if ($code -eq 0) { $code = 3 }
  Write-Host ('❌ 불합격 — {0}건' -f $fail.Count) -ForegroundColor Red
  foreach ($f in $fail) { Write-Host ("   · {0}" -f $f) -ForegroundColor Red }
}
Write-Host ''
Write-Host '--- 화면으로만 보이는 것은 아래를 열어 봐야 한다 ---' -ForegroundColor DarkGray
Write-Host '    A_join  = 마나바에 숫자가 있나 · 체력바가 있나' -ForegroundColor DarkGray
Write-Host '    B/C     = 두 장의 자세가 다른가 (같으면 걷기 애니가 없는 것)' -ForegroundColor DarkGray
foreach ($s in $shots) { Write-Host ("      {0}" -f (Join-Path $ctrl ("{0}_{1}.png" -f $Tag, $s))) -ForegroundColor DarkGray }
if ($shots.Count -eq 0) { Write-Host '    ⚠ 캡처가 한 장도 없다 — 화면 판정 불가' -ForegroundColor Yellow }

Cleanup
exit $code
