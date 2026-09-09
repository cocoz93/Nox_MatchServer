# newchar.ps1 — 캐릭터 목록에서 **New** 를 눌러 새 캐릭터를 만든다.
#
# 왜 필요한가 — 3직업을 실클라로 돌리려는데 목록에 **위저드와 워리어뿐**이다
#   (2026-09-09 실측: 1 Jack the Wizard · 2~4 Jack the Warrior).
#   컨저러가 없으니 소환 주문을 쓸 길이 없다 — 위저드 주문칸 다섯에도 소환이 없다.
#
# 이 스크립트는 **좌표를 찾는 탐사용**이다. 각 걸음마다 화면을 찍어 두고,
# 다음 걸음의 좌표는 그 그림을 보고 정한다.
#
#   .\tools\newchar.ps1 -Step chars      캐릭터 목록까지 가서 찍는다
#   .\tools\newchar.ps1 -Step new        거기서 New 를 누르고 찍는다
#   .\tools\newchar.ps1 -Step click -X 200 -Y 150 -Name z1   임의 자리를 눌러 찍는다
param(
  [ValidateSet('chars','new','click','key')][string]$Step = 'chars',
  [int]$X = 0, [int]$Y = 0,
  [string]$Key = '',
  [string]$Name = 'nc',
  [int]$WaitMenu = 40
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$ctrl = Join-Path $repo '_evidence\desk1'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
$di   = Join-Path $PSScriptRoot 'di.ps1'

function Send-Desk([string]$lines) {
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}
function Shot([string]$n) {
  Send-Desk "shotfull $ctrl\$n.png`n"
  Start-Sleep -Seconds 2
  Write-Host "  찍음 $n"
}
function LastPid() {
  $f = Join-Path $ctrl 'lastpid.txt'
  if (-not (Test-Path $f)) { return 0 }
  $v = 0; [void][int]::TryParse(([IO.File]::ReadAllText($f)).Trim(), [ref]$v); return $v
}

# 홈으로 몰고 그 자리에서 상대 이동 — join_my_server.ps1 과 같은 규칙
$home3 = @('m -3000 -3000','m -3000 -3000','m -3000 -3000','sleep 300')

if ($Step -eq 'chars') {
  # 서버가 있어야 목록에 줄이 생긴다
  # ⚠ 프록시도 죽여야 한다 — 18590 을 쥔 채로 남아 있으면 목록이 **빈 채로** 뜨고
  #   화면만 보면 「메뉴가 어긋났다」로 오해한다(2026-09-09 실측).
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  [void](Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = 'cmd /c ""' + (Join-Path $repo 'build\Release\nox_server.exe') +
                  '" --port 18590 --quiet > "' + (Join-Path $repo '_evidence\srv_newchar.log') + '" 2>&1"'
    CurrentDirectory = $repo })
  Start-Sleep -Seconds 3
  & (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Name -WaitMenu $WaitMenu -CharRow 0 -StopAtChars |
    Select-String -Pattern '클라 pid|saved|pid=' | ForEach-Object { Write-Host $_ }
  Write-Host '캐릭터 목록까지 갔다 — 이제 -Step new'
  exit 0
}

$pid_ = LastPid
if ($pid_ -le 4) { Write-Host '[FAIL] 클라 pid 를 못 잡았다'; exit 1 }
Send-Desk "target $pid_`n"
Start-Sleep -Milliseconds 300

if ($Step -eq 'new') {
  # New 버튼 — 화면 (373,789) → 주입 ((373-8)/2.04, (789-31)/2.04) = (179,372)
  & $di -Target $pid_ -Cmd ($home3 + @('m 179 372','sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2500')) | Out-Null
  Shot "${Name}_new"
} elseif ($Step -eq 'click') {
  & $di -Target $pid_ -Cmd ($home3 + @(('m {0} {1}' -f $X, $Y),'sleep 400','mb 0 1','sleep 150','mb 0 0','sleep 2000')) | Out-Null
  Shot "${Name}_click"
} elseif ($Step -eq 'key') {
  $cmds = @('inject 1')
  foreach ($k in ($Key -split ',')) { $cmds += @(("k {0} 1" -f $k), 'sleep 90', ("k {0} 0" -f $k), 'sleep 200') }
  & $di -Target $pid_ -Cmd $cmds | Out-Null
  Start-Sleep -Milliseconds 700
  Shot "${Name}_key"
}
