# join_my_server.ps1 — 정품 클라를 띄워 내 서버까지 몰고 화면을 찍는다.
#
#   .\tools\join_my_server.ps1                  기본 (Game3, NoxDesk1)
#   .\tools\join_my_server.ps1 -Exe Game4.exe -Desk NoxDesk1 -Tag c2
#
# 전제 — 게임 폴더에 dinput.dll 프록시가 있고 ddraw.ini 의 no_dinput_hook 이 true.
#        (tools\nox_mode.ps1 -Mode auto)
# 좌표는 창 클라 1280x960 기준을 2.04 로 나눈 값. 게임 커서는 홈에서 그만큼 간다.
param(
  [string]$Exe  = 'Game3.exe',
  [string]$Desk = 'NoxDesk1',
  [string]$Ctrl = 'desk1',        # 제어 폴더 이름 (_evidence 아래). 데스크톱 이름과 다르다
  [string]$Tag  = 'j',
  [int]$WaitMenu = 40,
  # ★ 캐릭터 목록에서 몇 번째 줄을 고를까 (1 부터). **직업이 여기서 갈린다.**
  #   2026-09-09 실측 목록 — **새로 만든 것이 맨 위로 온다.**
  #   컨저러를 만든 뒤: 1 Conjurer · 2 Wizard · 3~5 Warrior.
  #   0 이면 안 고른다(맨 위가 이미 골라져 있다).
  [int]$CharRow = 0,
  # ★ 캐릭터 목록에서 멈춘다 — 새 캐릭터를 만들려면 Load 를 누르면 안 된다.
  [switch]$StopAtChars
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$ctrl = Join-Path $repo "_evidence\$Ctrl"
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
if (-not (Test-Path $ctrl)) { New-Item -ItemType Directory $ctrl | Out-Null }

function Send-Desk([string]$lines) {
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}
function Shot([string]$name) {
  Send-Desk "shotfull $ctrl\$name.png`n"
  Start-Sleep -Seconds 2
  $log = [IO.File]::ReadAllText((Join-Path $ctrl 'log.txt'), [Text.Encoding]::UTF8)
  ($log -split "`r`n") | Select-String -Pattern 'saved' | Select-Object -Last 1 | ForEach-Object { $_.Line.Trim() }
}

# 1) 데스크톱 호스트가 없으면 띄운다 (WMI — 툴 세션과 같이 안 죽게)
if (-not (Get-Process -Name deskhost -ErrorAction SilentlyContinue)) {
  $cl = '"' + $repo + '\tools\deskhost\deskhost.exe" ' + $Desk + ' "' + $ctrl + '"'
  [void](Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cl })
  Start-Sleep -Seconds 1
}

# 2) 클라 기동
Send-Desk "spawn $game\$Exe|$game`n"
Start-Sleep -Seconds 8
$name = [IO.Path]::GetFileName($Exe)
$pid_ = (Get-CimInstance Win32_Process -Filter "Name='$name'" | Select-Object -Last 1).ProcessId
if (-not $pid_) { Write-Host '[FAIL] 클라가 안 떴다'; exit 1 }
Write-Host "클라 pid=$pid_"
# ★★ pid 를 **파일로 남긴다.** 부르는 쪽이 끝난 뒤 이름으로 다시 찾으면,
#   그 사이에 클라가 죽어 있을 때 조회가 빈손이 되어 pid=0 이 된다.
#   그리고 `Get-Process -Id 0` 은 **Idle 프로세스라 성공한다** — 죽은 클라가
#   「살아있음」으로 보였다(2026-09-09 실측). 태어난 자리에서 적어 둘 것.
[IO.File]::WriteAllText((Join-Path $ctrl 'lastpid.txt'), "$pid_", (New-Object Text.UTF8Encoding($false)))
Send-Desk "target $pid_`n"   # ★ 이걸 빼면 shotfull 이 「target 없음」으로 조용히 실패한다
Start-Sleep -Milliseconds 500

$di = Join-Path $repo 'tools\di.ps1'
# 3) 스플래시를 클릭으로 넘긴다 (자동 진행 70초를 기다리지 않는다)
& $di -Target $pid_ -Cmd @('inject 1','mb 0 1','sleep 200','mb 0 0','sleep 1200','mb 0 1','sleep 200','mb 0 0') | Out-Null
Write-Host "메뉴 대기 ${WaitMenu}초..."
Start-Sleep -Seconds $WaitMenu

# 4) 메뉴 → 서버목록 → Join → Load
#    ⚠ Join 팝업 위에서는 홈 금지 — 커서가 팝업을 벗어나면 닫힌다. 상대 이동만.
$home3 = @('m -3000 -3000','m -3000 -3000','m -3000 -3000','sleep 300')
& $di -Target $pid_ -Cmd ($home3 + @('m 280 167','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2000')) | Out-Null   # Multiplay
& $di -Target $pid_ -Cmd ($home3 + @('m 314 267','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2500')) | Out-Null   # Network
Write-Host (Shot "${Tag}1_list")
& $di -Target $pid_ -Cmd ($home3 + @('m 259 57','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2000')) | Out-Null    # 서버줄
& $di -Target $pid_ -Cmd @('m 14 100','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 3000') | Out-Null               # Join (상대 이동)
Write-Host (Shot "${Tag}2_chars")
# ★ 캐릭터 줄 고르기 — **직업이 여기서 갈린다.**
#   창 1296x999 에서 줄 y = 260 · 292 · 325 · 357, x 아무 데나(600).
#   주입 좌표 = (화면 − 창테두리(8,31)) / 2.04  — Load 버튼 (648,790)→(313,372) 로 검산했다.
if ($CharRow -ge 1 -and $CharRow -le 5) {
  $rowY = @(112, 128, 144, 160, 176)[$CharRow - 1]
  & $di -Target $pid_ -Cmd ($home3 + @('m 290 ' + $rowY,'sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 900')) | Out-Null
  Write-Host (Shot "${Tag}2b_pick")
}
if ($StopAtChars) { Write-Host "pid=$pid_"; Write-Host '캐릭터 목록에서 멈췄다'; exit 0 }
& $di -Target $pid_ -Cmd ($home3 + @('m 313 372','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 8000')) | Out-Null   # Load
Write-Host (Shot "${Tag}3_ingame")

# 5) 관전 해제 — SPACE → ENTER → 왼클릭 → **ESC**
#
# ★★ 마지막 ESC 를 빼면 안 된다. 여기 ENTER 는 관전을 깨우지만 **채팅 입력줄도 연다.**
#    그 뒤로 누르는 키는 전부 채팅 글자로 들어가 게임에 안 닿는다 —
#    2026-09-08 에 주문키 A~G 를 아무리 눌러도 `0x79 TRY_SPELL` 이 0건이었고,
#    화면을 찍어 보니 한가운데 `asdfgaaaaaa` 라는 입력줄이 떠 있었다.
#    ESC 로 닫자마자 같은 키 다섯이 그대로 시전으로 나갔다(0x79 5건,
#    화면에 "You cast Force Field. / You invoke the Fireball spell." 이 찍힘).
#    ⚠ 마우스는 채팅줄과 무관해서 근접 공격만 잘 되었다 — 그래서 오래 못 봤다.
& $di -Target $pid_ -Cmd @('k 0x39 1','sleep 150','k 0x39 0','sleep 900','k 0x1C 1','sleep 150','k 0x1C 0','sleep 900','mb 0 1','sleep 150','mb 0 0','sleep 2000','k 0x01 1','sleep 150','k 0x01 0','sleep 800') | Out-Null
Write-Host (Shot "${Tag}4_wake")
Write-Host "pid=$pid_"
