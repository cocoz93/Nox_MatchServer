# judge.ps1 — 사람이 직접 볼 것들을 위해 **서버(와 필요하면 상대 하나)를 띄운다**.
#
#   .\tools\judge.ps1            (기본: 전부 — 다섯 가지를 한 판에)
#   .\tools\judge.ps1 벽
#   .\tools\judge.ps1 작살
#   .\tools\judge.ps1 소리
#   .\tools\judge.ps1 휘두르기
#   .\tools\judge.ps1 주문칸
#
# 서버가 새 창으로 뜬다. 그 창을 닫으면 서버가 꺼진다.
# 클라는 게임 폴더의 Game.exe 를 직접 켜서 Multiplay -> Network(LAN) -> MY_SERVER 로 들어간다.
# 들어가면 **왼클릭 한 번** — 접속 직후는 관전 상태라 막대도 칸도 없고 입력이 안 먹는다.
#
# ★ 2026-09-22 — 상대가 필요한 항목(작살)은 **Game2.exe 를 쓰지 않는다.**
#   Game2 는 ddraw 설정 구역이 없어 화면이 깜빡였다. 대신 우리 자동 클라(opennox)를
#   가만히 세워 두고 그걸 상대로 쓴다. 사람이 보는 화면은 **정품 하나**뿐이라
#   그게 그대로 ④ 확정이 된다.
param([string]$무엇 = '전부', [switch]$짝없이)

$ErrorActionPreference = 'Stop'
$뿌리 = Split-Path -Parent $PSScriptRoot
$서버 = Join-Path $뿌리 'build\Release\nox_server.exe'
$빌드 = Join-Path $뿌리 'third_party\opennox-build'
$게임 = 'C:\Program Files\GOG Galaxy\Games\Nox'
$맵   = Join-Path $게임 'maps\Estate\Estate.map'

if (-not (Test-Path $서버)) { throw "서버가 없다: $서버 (먼저 빌드할 것)" }

# --quiet — 틱마다 찍던 진단 줄을 끈다. 게임에는 필요 없고 사람이 볼 땐 방해만 된다
#   (그래도 접속·판끝 같은 드문 줄은 찍힌다 — `main.cpp:1805` 참조)
$인자 = @('--port','18590','--standalone','--map',"`"$맵`"",'--mode','arena','--quiet')
$상대필요 = $false
switch ($무엇) {
    '벽'       { $안내 = @(
                   '① Game.exe 로 접속한다(직업은 아무거나).',
                   '② 벽에 대고 걸어 본다. 모서리·좁은 복도에서도.',
                   '',
                   '볼 것 — 벽에 닿았을 때 **멀리 날아가나**.',
                   '        멈추거나 살짝 밀리는 정도면 맞다(정품이 그렇다).',
                   '        09-22 에 사람 무게를 고쳐 숫자상으로는 사라졌다 — 화면 확인만 남았다') }
    '작살'     { $상대필요 = $true
                 $안내 = @(
                   '① Game.exe 로 **워리어**를 골라 접속한다(작살은 워리어 능력 3번).',
                   '② 화면에 서 있는 상대(우리 자동 클라)를 겨누고 능력 3번을 쓴다.',
                   '',
                   '볼 것 — 끌려오는 쪽이 **부드럽게 다가오나**.',
                   '        주인을 지나쳐 앞뒤로 왕복하면 아직 틀린 것이다.',
                   '        09-22 에 고쳐 시험상으로는 안 지나친다 — 화면 확인만 남았다') }
    '소리'     { $안내 = @(
                   '① Game.exe 로 **워리어**를 골라 접속한다.',
                   '② 능력 1~5 를 눌러 본다(광전사·함성·작살·살금살금·늑대의 눈).',
                   '③ 쉬는 중에 또 누르면 거절 소리가 나야 한다.',
                   '',
                   '볼 것 — 능력마다 **소리가 나나**. 아예 없으면 틀린 것이다.',
                   '        ⚠ 소리가 하나도 안 나면 서버가 아니라 nox.cfg 볼륨부터 볼 것') }
    '휘두르기' { $상대필요 = $true
                 $안내 = @(
                   '① Game.exe 로 접속해 무기를 들고 상대를 때려 본다.',
                   '',
                   '볼 것 — 휘두르는 **동작이 중간에 끊기나**.',
                   '        끝까지 이어지면 맞다') }
    '주문칸'   { $안내 = @(
                   '① Game.exe 로 접속한다(**위저드나 컨저러** — 워리어는 주문이 없다).',
                   '② B 를 눌러 주문책을 열고, 주문 하나를 아래 A 칸에 끌어놓는다.',
                   '',
                   '볼 것 — 칸에 **주문이 들어가나**. 들어가면 그 키로 시전된다') }
    default    { $무엇 = '전부'; $상대필요 = $true
                 $안내 = @(
                   '다섯 가지를 한 판에 본다. 직업을 바꿔야 하는 것이 있어 **두 번 접속**한다.',
                   '',
                   '[워리어로 접속]',
                   '  1. 벽에 대고 걸어 보기      — 멀리 날아가나',
                   '  2. 상대에게 능력 3번(작살)  — 부드럽게 끌려오나, 지나쳐 왕복하나',
                   '  3. 능력 1~5 를 눌러 보기    — 능력마다 소리가 나나',
                   '  4. 상대를 때려 보기         — 휘두르는 동작이 중간에 끊기나',
                   '',
                   '[나갔다 위저드나 컨저러로 다시 접속]',
                   '  5. B 로 주문책 -> 주문을 A 칸에 끌어놓기 — 들어가나',
                   '',
                   '상대는 우리 자동 클라가 가만히 서 있는 것이다(때려도 된다).') }
}

Write-Host ""
Write-Host "[$무엇] 서버를 띄운다 — 맵 Estate · 아레나 · 포트 18590" -ForegroundColor Cyan
$p = Start-Process -FilePath $서버 -ArgumentList $인자 -PassThru
Start-Sleep 3
if ($p.HasExited) { throw '서버가 바로 꺼졌다 — 서버 창의 글을 볼 것' }
Write-Host "서버 떴다 (프로세스 $($p.Id)). 끄려면 그 창을 닫으면 된다." -ForegroundColor Green

if ($상대필요 -and -not $짝없이) {
    $env:NOX_JOIN_ADDR='127.0.0.1:18590'; $env:NOX_JOIN_CLASS='1'
    $env:NOX_OBSERVER_CLICKS='1'; $env:NOX_XWIS='false'; $env:NOX_NET_NAT='false'
    [Environment]::SetEnvironmentVariable('NOX_AUTO_WALK', $null)
    # ★ 2026-09-23 — 동행 클라의 stderr 를 **받아 둔다.** 09-22 판에서 이 클라가 판 도중 죽었는데
    #   stderr 를 안 받아 놓아 **까닭을 적을 근거가 한 줄도 없었다**(Go 패닉은 stderr 로만 나간다).
    $동행오류 = Join-Path $빌드 'logs\companion.err.txt'
    $c = Start-Process -FilePath (Join-Path $빌드 'opennox.exe') -WorkingDirectory $빌드 `
         -ArgumentList '-data',"`"$게임`"" -PassThru -RedirectStandardError $동행오류
    Write-Host "상대(자동 클라) 켜는 중 — 25초쯤 뒤 맵에 선다 (프로세스 $($c.Id))" -ForegroundColor Green
}

Write-Host ""
$안내 | ForEach-Object { Write-Host "  $_" }
Write-Host ""
Write-Host "  클라 = 게임 폴더의 Game.exe -> Multiplay -> Network(LAN) -> MY_SERVER" -ForegroundColor Yellow
Write-Host "  들어가면 왼클릭 한 번 (관전 풀기)" -ForegroundColor Yellow
Write-Host "  조작 = 오른버튼 걷기 · 왼버튼 때리기 · A S D F G 칸 · B 주문책" -ForegroundColor Yellow
Write-Host ""