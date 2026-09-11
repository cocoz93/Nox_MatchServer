# spell_bisect.ps1 — **주문을 쓰면 클라가 게임에서 나가는** 조건과 원인을 가른다.
#
# 왜 (2026-09-11) — 졸업시험 g9 에서 —
#   시전은 **성공한다**(`주문 +1`, 마나 차감). 그 **직후** 클라가 `0xae`(나가기)를 보내고
#   서버가 세션을 걷는다. 사용자가 말한 「주문 쓰니까 튕김」이 이것이다.
#   ⚠ **프로세스는 멀쩡하다** — 그래서 한동안 하네스가 「클라 살아있음」이라 찍었다.
#     지문은 서버 로그의 **`조용해서 내보냄`** 이다.
#
# ★★ 지금까지 밝혀진 것 —
#   · 소환(87)만이 아니라 **치유(41)에서도** 나갔다 → 소환수 소개는 범인이 아니다
#   · `0x70` 만 끄니 붙어 있었지만 **대조군(켠 채)도 붙어 있었다** → 이분법 불성립
#   · 두 회차 모두 **걷지 않고 바로 시전**이었다. 나간 회차(g9)는 **걷고 나서** 시전했다
#   → 그래서 `-Walk` 가 있다. **조건부터 세운 뒤에** 원인을 가른다.
#
# ★ 이 스크립트는 **켠 스위치를 맨 위에 찍고, 걸음이 실제로 들어갔는지도 찍는다.**
#   앞서 `-Walk` 를 줬는데 그 파라미터가 없어 PowerShell 이 조용히 넘겼고,
#   하마터면 「걷기도 조건이 아니다」로 잘못 읽을 뻔했다. 스위치는 **먹었는지 보여야** 한다.
#
#   .\tools\spell_bisect.ps1 -Walk                # 걷고 나서 시전 (재현 시도)
#   .\tools\spell_bisect.ps1 -Walk -NoSpellStart  # 같은 조건에서 0x70 만 빼고
#
# 종료 코드 — 0 붙어 있음 · 3 게임에서 나감 · 6 접속 실패
param(
  # 시전 전에 걷는다. 나간 회차(g9)의 조건을 재현하려고.
  [switch]$Walk,
  # 시전 알림 `0x70` 을 **끈다**. 서버 기본은 **보내는** 쪽이다.
  [switch]$NoSpellStart,
  [string]$Tag = 'bis',
  # 누를 주문칸. 기본 G(0x22) = 41 약한치유 — **소환이 아니다.**
  [int]$Slot = 0x22,
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$slog = Join-Path $root ("_evidence\srv_{0}.log" -f $Tag)
$di   = Join-Path $PSScriptRoot 'di.ps1'

# ★ **켠 것을 먼저 찍는다.** 스위치가 먹었는지 눈으로 확인할 수 있어야 한다.
Write-Host ("[조건] 걷기={0} · 0x70={1} · 주문칸=0x{2:x2} · 꼬리표={3}" -f
            $(if ($Walk) { '함' } else { '안 함' }),
            $(if ($NoSpellStart) { '끔' } else { '보냄(기본)' }),
            $Slot, $Tag) -ForegroundColor Cyan

function Cleanup {
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}
Cleanup
Start-Sleep -Seconds 3
if (Test-Path $slog) { [IO.File]::Delete($slog) }

# ⚠ 서버 기본이 「보냄」이므로 끄려면 `--no-spell-start` 를 줘야 한다.
$extra = ''
if ($NoSpellStart) { $extra = ' --no-spell-start' }
$exe = Join-Path $root 'build\Release\nox_server.exe'
$cmd = 'cmd /c ""' + $exe + '" --port 18590 --standalone --map "' + $Map + '" --mode arena' + $extra + ' > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $cmd) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
Write-Host '서버 떴음'

& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Tag -CharRow 1 |
  Select-String -Pattern '픽 확인' | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
$p = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
if ($p.Count -eq 0) { Write-Host '[멈춤] 클라가 안 떴다' -ForegroundColor Red; Cleanup; exit 6 }
$cpid = $p[0].Id
& $di -Target $cpid -Cmd @('inject 1') | Out-Null
Start-Sleep -Seconds 3

if ($Walk) {
  Write-Host '  -> 먼저 걷는다 (오른버튼 1.8초)'
  & $di -Target $cpid -Cmd @('mb 1 1','sleep 1800','mb 1 0','sleep 400','mb 1 0','sleep 1200') | Out-Null
}

Write-Host ("  -> 주문칸 0x{0:x2} 를 누른다" -f $Slot)
& $di -Target $cpid -Cmd @(("k {0} 1" -f $Slot), 'sleep 120', ("k {0} 0" -f $Slot), 'sleep 3000') | Out-Null

Write-Host ''
Write-Host '=== 결과 ==='
$f = New-Object IO.FileStream($slog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try { $t = (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }

# ★ 걸음이 실제로 들어갔나 — `-Walk` 를 줬는데 안 걸었으면 **조건을 못 세운 회차**다.
$steps = 0
$tickLines = @(($t -split "`r?`n") | Where-Object { $_ -match '\[틱\]' })
if ($tickLines.Count -gt 0 -and $tickLines[-1] -match '걸음 (\d+)') { $steps = [int]$Matches[1] }
Write-Host ("  걸음 {0}" -f $steps)
if ($Walk -and $steps -le 0) {
  Write-Host '  ⚠ 걷기를 시켰는데 걸음이 0 이다 — 이 회차는 조건을 못 세웠다' -ForegroundColor Yellow
}
($t -split "`r?`n") | Where-Object { $_ -match '\[시전\]' } | Select-Object -First 2 |
  ForEach-Object { Write-Host ("  " + $_) }

$gone = @([regex]::Matches($t, '조용해서 내보냄')).Count
if ($gone -gt 0) {
  Write-Host ("  [나감] 클라가 게임에서 나갔다 ({0}건)" -f $gone) -ForegroundColor Red
} else {
  Write-Host '  [붙음] 클라가 붙어 있다' -ForegroundColor Green
}
Cleanup
exit $(if ($gone -gt 0) { 3 } else { 0 })
