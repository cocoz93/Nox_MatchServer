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
  # ★ 캡처를 끈다 — 「캡처가 튕김의 조건인가」를 가르려고(2026-09-11).
  #   졸업시험은 나가는데 이분법(캡처 없음)은 안 나간다. 남은 차이 중 하나가 이것이다.
  [switch]$NoShots,
  # 주문을 아예 안 누른다 — 「주문이 원인인가」를 가르는 진짜 대조군.
  [switch]$NoSpells,
  # ★ 판 종류. **rena 는 혼자 돌면 때릴 적이 없어** 「맞고 죽기」를 못 잰다 —
  #   quest 로 돌리면 몬스터가 나와 실제로 때린다(2026-09-11 sp1 런에서 드러난 구멍).
  [ValidateSet('arena','ctf','quest')][string]$Mode = 'arena',
  # ★ 들어올 때 체력을 낮춘다(0 이면 안 건드림). **최대 체력은 그대로**라 부활 판정이 선다.
  #   거머리 2마리가 90초에 38 밖에 못 깎아 「죽음·부활」을 못 쟀다(2026-09-11).
  [int]$StartHealth = 0,
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
  if ($NoShots) { return $false }   # 진단 — 캡처가 조건인지 가르는 중
  $png = Join-Path $ctrl ("{0}_{1}.png" -f $Tag, $name)
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'),
                          ("shotfull {0}`n" -f $png), (New-Object Text.UTF8Encoding($false)))
  Start-Sleep -Seconds 2
  return (Test-Path $png)
}
# 서버가 스스로 말하는 값 하나
# ★★ **시전 횟수는 틱 줄로 세면 안 된다** (2026-09-11).
#   `[틱]` 은 5초에 한 번쯤 찍히는데 주문을 누른 뒤 1.5초 만에 읽으니 **아직 갱신 전**이다.
#   그래서 서버는 `주문 1` 인데 하네스는 칸마다 `+0` 이라 찍었고, 그것을 「주문이 안 나간다」로
#   읽어 한참을 헤맸다. `[시전]` 줄은 **받는 즉시** 찍히므로 이쪽이 맞다.
function SpellSeen() {
  return @([regex]::Matches((ReadShared $slog), '\[시전\]')).Count
}
# ★ 사람 체력을 읽는다 — `[자리]` 줄의 `체력N/M`. 못 읽으면 -1.
#   ★★ 「죽었나」를 **피해 계수로 재면 안 된다** — 그 값은 **모든 대상에 준 피해 누계**라
#     사람 체력과 다른 값이다(2026-09-11에 헷갈렸다). 체력은 체력으로 잰다.
function PlayerHp([ref]$maxOut) {
  $lines = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[자리\].*체력' })
  if ($lines.Count -eq 0) { return -1 }
  if ($lines[-1] -match '체력(\d+)/(\d+)') {
    if ($maxOut) { $maxOut.Value = [int]$Matches[2] }
    return [int]$Matches[1]
  }
  return -1
}
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
       '" --port 18590 --standalone --map "' + $Map + '" --mode ' + $Mode + $(if ($StartHealth -gt 0) { ' --start-health ' + $StartHealth } else { '' }) + ' > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $srv) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
if ((ReadShared $slog) -notmatch '\[맵\]') {
  Write-Host '[멈춤] 서버 로그에 [맵] 줄이 없다 — 맵을 못 읽었다' -ForegroundColor Red
  Cleanup; exit 6
}
Write-Host ("[조건] 직업={0} · 판={1} · 시작체력={2} · 캡처={3} · 꼬리표={4}" -f $Class, $Mode, $StartHealth, $(if ($NoShots) { '끔' } else { '찍음' }), $Tag) -ForegroundColor Cyan
Write-Host '서버 떴음'

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
# ★★ **짧게 걷고 바로 찍는다** (2026-09-11, 수단 바꾸기 ①).
#   무엇이 달라지나 — **캐릭터가 화면에서 덜 움직인다.** 앞 회차는 0.9초씩 걸어
#   둘째 장에서 캐릭터가 **나무 뒤로 들어가** 자세를 못 봤다(`zm_C_wide.png`).
$beforeWalk = TickNum '걸음'
& $di -Target $pid1 -Cmd @('mb 1 1','sleep 250') | Out-Null
if (Shot 'B_walk1') { $shots += 'B_walk1' }    # 걷는 도중 ①
& $di -Target $pid1 -Cmd @('sleep 250') | Out-Null
if (Shot 'C_walk2') { $shots += 'C_walk2' }    # 걷는 도중 ② — ①과 자세가 달라야 한다
& $di -Target $pid1 -Cmd @('sleep 250') | Out-Null
if (Shot 'C2_walk3') { $shots += 'C2_walk3' }  # ③ — 셋을 견주면 한 장이 가려도 남는다
& $di -Target $pid1 -Cmd @('sleep 1200') | Out-Null   # 그 뒤 충분히 걸어 걸음이 오르게
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
# ★★ **소환(A)을 다시 맨 앞으로** (2026-09-11).
#   뒤로 돌렸던 까닭은 「A 에서 죽는다」였는데 그 판정이 **거짓 양성**이었다
#   (지문이 임시 포트를 가리켰다). 이제 다섯 칸을 다 눌러도 클라가 멀쩡하다.
#   ⚠ 그런데 **앞 세 주문이 마나를 다 써서 소환이 안 됐고**, 그래서 죽음 단계가
#     때릴 것이 없어 헛돌았다. 소환을 먼저 해야 소환수가 서고 그 단계가 돈다.
#   ⚠ 죽음 단계가 **「소환수가 주인을 때린다」는 결함에 기대고 있다.** 그 결함을
#     고치면 이 단계는 **둘째 클라**로 바꿔야 한다(G4 뒤 과제).
$slots = [ordered]@{ 'A'=0x1E; 'G'=0x22; 'F'=0x21; 'D'=0x20; 'S'=0x1F }
$killer = ''
# ★★ **주문을 아예 안 누르는 대조군** (2026-09-11).
#   지금까지 주문이 원인이라 **가정**하고 이분법을 돌렸는데, 회차마다 나가는 지점이
#   달랐다 — 성공한 G 에서도, 마나가 모자라 시전이 안 된 S 에서도 나갔다.
#   주문 없이도 나가면 원인은 주문이 아니라 **시간·놀고 있음**이다.
#   이 대조군을 한 번도 안 돌린 것이 오늘의 빈 자리다.
if ($NoSpells) {
  Write-Host '  (주문 안 누름 — 같은 시간만 기다린다)'
  Start-Sleep -Seconds 9
  $goneIdle = ((TickNum '사람') -lt 1)
  Write-Host ("  기다린 뒤 클라 {0}" -f $(if ($goneIdle) { '★게임에서 나감★' } else { '살아있음' }))
  if ($goneIdle) { $fail += '주문을 안 눌렀는데도 클라가 나갔다 — 원인은 주문이 아니다'; $code = 3 }
}
foreach ($k in $slots.Keys) {
  if ($NoSpells) { break }
  if (-not (Alive $pid1)) { break }
  $sp0 = SpellSeen
  & $di -Target $pid1 -Cmd @(("k {0} 1" -f $slots[$k]), 'sleep 120', ("k {0} 0" -f $slots[$k]), 'sleep 1500') | Out-Null
  $sp1 = SpellSeen
  # ★★ **프로세스가 살아 있어도 「게임에서 나갔으면」 죽은 것이다** (2026-09-11).
  #   g8 에서 소환을 쓰자 클라가 `0xae`(나가기)를 보내고 서버가 세션을 걷었는데,
  #   프로세스는 멀쩡해서 하네스가 **「클라 살아있음」이라 찍었다.** 그게 이 시험을
  #   여섯 판이나 헛돌게 했다 — 사람이 보면 「튕겼다」인데 계측은 「살아있다」였다.
  # ★★★ **지문을 고쳤다 (2026-09-11)** — 전에는 `조용해서 내보냄` 을 썼는데
  #   그것이 **거짓 양성**이었다. 그 줄이 가리킨 주소는 **접속 과정의 임시 포트**이고
  #   (서버 찾기·접속 시도용), 그게 정리되는 것은 **정상 동작**이다.
  #   플레이 중인 세션은 멀쩡했다 — 네 회차 모두 `사람 1` 이 끝까지 유지됐다.
  #   그 잘못된 지문 위에서 여섯 판을 돌며 「주문이 죽인다」를 쫓았다.
  #   → 이제 **서버가 세는 사람 수**로 본다. 1 → 0 이면 진짜로 나간 것이다.
  $gone = ((TickNum '사람') -lt 1)
  $ok = (Alive $pid1) -and (-not $gone)
  Write-Host ("  주문칸 {0}  클라가 보냄 +{1}  클라 {2}" -f $k, ($sp1 - $sp0),
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
# ⚠⚠ **낡은 기록 정정(2026-09-11)** — 「소환수가 주인을 때린다」는 **이미 고쳐졌다.**
#   `Server.cpp:599` 의 `NearestTarget` 이 `if (kv.first == friendOwner) continue;` 로 주인을 거른다
#   (`:607` 은 내 편 소환수도 거른다). `proto_test` 의 「부른 사람은 안 때린다」가 지킨다.
#   → **그래서 아레나에 혼자면 때릴 것이 없다.** 죽음·부활을 재려면 `-Mode quest`(몬스터) 또는
#     `-StartHealth` 로 체력을 낮춰야 한다 — 둘 다 손잡이가 있다.
if ($code -eq 0 -and (Alive $pid1)) {
  Write-Host '--- 맞고 죽기 → 부활 (최대 90초) ---'
  $dmg0 = TickNum '피해'
  $died = $false
  $sawDead = $false      # 체력이 0 까지 간 적이 있나 (죽음의 증거)
  for ($w = 0; $w -lt 90; $w += 5) {
    Start-Sleep -Seconds 5
    if ((PlayerHp $null) -eq 0) { $sawDead = $true }
    if (-not (Alive $pid1)) {
      $fail += ("맞는 도중 클라가 죽었다 — 피해 {0} 쌓인 뒤 (주문이 아니라 **죽음**이 튕김의 자리일 수 있다)" -f ((TickNum '피해') - $dmg0))
      $code = 3; $died = $true; break
    }
  }
  $dmg1 = TickNum '피해'
  Write-Host ("  피해 +{0} · 클라 {1}" -f ($dmg1 - $dmg0), $(if (Alive $pid1) { '살아있음' } else { '★죽음★' }))
  if (-not $died) {
    if (($dmg1 - $dmg0) -le 0) { $fail += '90초 동안 아무 피해도 안 들어왔다 — 죽음·부활을 못 쟀다' }
    # ★★ **죽음·부활을 실제로 본다** — 체력이 0 까지 갔다가 최대로 돌아왔나.
    #   $sawDead 는 위 순회에서 채운다. 여기서는 **지금 체력이 최대인지**만 본다.
    $mx = 0
    $hpNow = PlayerHp ([ref]$mx)
    Write-Host ("  체력 {0}/{1} · 죽은 적 {2}" -f $hpNow, $mx, $(if ($sawDead) { '있음' } else { '없음' }))
    if ($sawDead) {
      if ($hpNow -eq $mx -and $mx -gt 0) { Write-Host '  ⭕ 죽음→부활 확인 (체력이 최대로 돌아왔다)' -ForegroundColor Green }
      else { $fail += ("죽었는데 부활 뒤 체력이 최대가 아니다 ({0}/{1})" -f $hpNow, $mx) }
    } else {
      # ⚠ **못 잰 것과 안 되는 것을 가른다** — 안 죽었으면 결함이 아니라 상황을 못 만든 것이다.
      Write-Host ("  ➖ 죽음·부활은 못 쟀다 — 90초 동안 안 죽었다 (체력 {0}/{1}). -StartHealth 로 낮춰 볼 것" -f $hpNow, $mx) -ForegroundColor Yellow
    }
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
