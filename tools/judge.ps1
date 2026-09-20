# judge.ps1 — 사람이 직접 볼 세 가지를 위해 **서버만 띄운다**.
#
#   .\tools\judge.ps1            (기본: 능력 소리)
#   .\tools\judge.ps1 작살
#   .\tools\judge.ps1 주문칸
#
# 서버가 새 창으로 뜬다. 그 창을 닫으면 서버가 꺼진다.
# 클라는 게임 폴더의 Game.exe 를 직접 켜서 Multiplay → Network(LAN) → MY_SERVER 로 들어간다.
# ⚠ 들어가면 **왼클릭 한 번** — 접속 직후는 관전 상태라 막대도 칸도 없고 입력이 안 먹는다.
param([string]$무엇 = '소리')

$ErrorActionPreference = 'Stop'
$뿌리 = Split-Path -Parent $PSScriptRoot
$서버 = Join-Path $뿌리 'build\Release\nox_server.exe'
$맵   = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\Estate\Estate.map'

if (-not (Test-Path $서버)) { throw "서버가 없다: $서버 (먼저 빌드할 것)" }

$인자 = @('--port','18590','--standalone','--map',"`"$맵`"",'--mode','arena')
switch ($무엇) {
    '작살'   { $안내 = @(
                 '① Game.exe 와 Game2.exe 로 둘이 접속한다(둘 다 LAN 목록의 MY_SERVER).',
                 '② 한쪽은 워리어여야 한다 — 작살은 워리어 능력 3번이다.',
                 '③ 워리어가 상대를 겨누고 능력 3번을 쓴다.',
                 '',
                 '볼 것 — 끌려오는 쪽이 주인을 지나쳐 튕기나, 아니면 부드럽게 끌려오나.',
                 '        우리 서버는 튕긴다(그게 규칙대로인지 정품과 견주는 것이다).') }
    '주문칸' { $안내 = @(
                 '① Game.exe 로 접속한다(위저드나 컨저러가 좋다).',
                 '② B 를 눌러 주문책을 열고, 주문 하나를 아래 A 칸에 끌어놓는다.',
                 '',
                 '볼 것 — 칸에 주문이 들어가나. 들어가면 그 키로 시전된다.') }
    default  { $무엇 = '소리'
               $안내 = @(
                 '① Game.exe 로 워리어를 골라 접속한다.',
                 '② 능력 1~5 를 눌러 본다(광전사·함성·작살·살금살금·늑대의 눈).',
                 '③ 쉬는 중에 또 누르면 거절 소리가 나야 한다.',
                 '',
                 '볼 것 — 능력마다 소리가 나나. 소리가 아예 없으면 ❌.') }
}

Write-Host ""
Write-Host "[$무엇] 서버를 띄운다 — 맵 Estate · 아레나 · 포트 18590" -ForegroundColor Cyan
$p = Start-Process -FilePath $서버 -ArgumentList $인자 -PassThru
Start-Sleep 3
if ($p.HasExited) { throw '서버가 바로 꺼졌다 — 서버 창의 글을 볼 것' }
Write-Host "서버 떴다 (프로세스 $($p.Id)). 끄려면 그 창을 닫으면 된다." -ForegroundColor Green
Write-Host ""
$안내 | ForEach-Object { Write-Host "  $_" }
Write-Host ""
Write-Host "  클라 = 게임 폴더의 Game.exe → Multiplay → Network(LAN) → MY_SERVER" -ForegroundColor Yellow
Write-Host "  들어가면 왼클릭 한 번 (관전 풀기)" -ForegroundColor Yellow
Write-Host ""