# spell_bisect.ps1 — **주문을 쓰면 클라가 게임에서 나가는** 원인을 하나씩 꺼서 가른다.
#
# 왜 (2026-09-11) — g8·g9 에서 밝혀진 것 —
#   시전은 **성공한다**(`주문 +1`, 비용 차감). 그 **직후** 클라가 `0xae`(나가기)를 보내고
#   서버가 세션을 걷는다. 사용자가 말한 「주문 쓰니까 튕김」이 이것이다.
#   소환(87)만이 아니라 **치유(41)에서도** 나갔으므로 소환수 소개는 범인이 아니다.
#
# ⚠ 프로세스는 멀쩡하다 — 그래서 하네스가 한동안 「클라 살아있음」이라 찍었다.
#   지문은 서버 로그의 **`조용해서 내보냄`** 이다.
#
#   .\tools\spell_bisect.ps1            # 있는 그대로
#   .\tools\spell_bisect.ps1 -NoSpellStart   # 시전 알림(0x70)만 끄고
param(
  # 이제 기본이 「안 보냄」이므로 이 스위치는 **켜는** 쪽이다.
  [switch]$SpellStart,
  [string]$Tag = 'bis',
  [int]$Slot = 0x22,   # G = 41 약한치유 (소환 아님)
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$slog = Join-Path $root ("_evidence\srv_{0}.log" -f $Tag)

function Cleanup {
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}
Cleanup
Start-Sleep -Seconds 3
if (Test-Path $slog) { [IO.File]::Delete($slog) }

$extra = ''
if ($SpellStart) { $extra = ' --spell-start' }
$exe = Join-Path $root 'build\Release\nox_server.exe'
$cmd = 'cmd /c ""' + $exe + '" --port 18590 --standalone --map "' + $Map + '" --mode arena' + $extra + ' > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $cmd) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
Write-Host ("서버 떴음{0}" -f $(if ($SpellStart) { ' (시전 알림 0x70 **켬**)' } else { ' (기본 — 0x70 안 보냄)' }))

& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Tag -CharRow 1 |
  Select-String -Pattern '픽 확인' | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
$p = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
if ($p.Count -eq 0) { Write-Host '[멈춤] 클라가 안 떴다' -ForegroundColor Red; Cleanup; exit 6 }
$di = Join-Path $PSScriptRoot 'di.ps1'
& $di -Target $p[0].Id -Cmd @('inject 1') | Out-Null
Start-Sleep -Seconds 3

Write-Host ("주문칸 0x{0:x2} 를 누른다" -f $Slot)
& $di -Target $p[0].Id -Cmd @(("k {0} 1" -f $Slot), 'sleep 120', ("k {0} 0" -f $Slot), 'sleep 3000') | Out-Null

Write-Host ''
Write-Host '=== 결과 ==='
$f = New-Object IO.FileStream($slog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try { $t = (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }
($t -split "`r?`n") | Where-Object { $_ -match '\[시전\]' } | Select-Object -First 2 |
  ForEach-Object { Write-Host ("  " + $_) }
$gone = @([regex]::Matches($t, '조용해서 내보냄')).Count
if ($gone -gt 0) {
  Write-Host ("  ⛔ 클라가 게임에서 나갔다 ({0}건){1}" -f $gone,
              $(if ($SpellStart) { ' — 0x70 을 켜면 나간다(범인 확정)' } else { ' — 기본인데도 나갔다면 다른 원인' })) -ForegroundColor Red
} else {
  Write-Host ("  ★ 클라가 붙어 있다{0}" -f
              $(if ($SpellStart) { ' — 0x70 을 켜도 멀쩡하다?' } else { ' — 기본 설정에서 붙어 있다 ⭕' })) -ForegroundColor Green
}
Cleanup
exit $(if ($gone -gt 0) { 3 } else { 0 })
