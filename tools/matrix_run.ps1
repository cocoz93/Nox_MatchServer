# matrix_run.ps1 — 3직업 × 3모드 아홉 조합을 내리 돌리고 요약을 한 파일로 남긴다.
#
# 왜 만드나 — 「유저가 봐서 이상함 없음」 판정의 마지막 관문. 고친 것(크리처 애니·
# 정상 배치·깃발 나르기)이 **아홉 조합 전부에서** 판끝까지 가는지 한 번에 확인한다.
#
#   .\tools\matrix_run.ps1            아홉 조합 전부
#   결과  _evidence\matrix_result.txt (한 조합 한 줄)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$resFile = Join-Path $repo '_evidence\matrix_result.txt'
[IO.File]::WriteAllText($resFile, ("시작 {0}`r`n" -f (Get-Date -Format 'HH:mm:ss')), (New-Object Text.UTF8Encoding($false)))

# 조합마다 — 아레나는 싸움, 깃발·퀘스트는 판 뛰기(정상 배치)
$combos = @(
  @{ c='warrior';  m='arena'; fight=220; play=0   },
  @{ c='warrior';  m='ctf';   fight=0;   play=280 },
  @{ c='warrior';  m='quest'; fight=0;   play=280 },
  @{ c='wizard';   m='arena'; fight=220; play=0   },
  @{ c='wizard';   m='ctf';   fight=0;   play=280 },
  @{ c='wizard';   m='quest'; fight=0;   play=280 },
  @{ c='conjurer'; m='arena'; fight=220; play=0   },
  @{ c='conjurer'; m='ctf';   fight=0;   play=280 },
  @{ c='conjurer'; m='quest'; fight=0;   play=280 }
)

foreach ($it in $combos) {
  # ⚠ 첫 글자만 쓰면 warrior/wizard 가 둘 다 'w' 라 로그가 덮인다(2026-09-10 실측)
  $tag = ('M_{0}_{1}' -f $it.c, $it.m)
  $t0 = Get-Date
  # ⚠ **배열 스플래팅 금지** — PS 5.1 에서 이름 결합이 어긋나 'default' 가
  #   WaitMenu(int) 로 떨어지며 아홉 조합이 전부 0초에 죽었다(2026-09-10).
  #   해시테이블 스플래팅은 이름으로만 묶여 이 함정이 없다.
  $args2 = @{ Tag = $tag; Prelude = 'default'; Class = $it.c; Mode = $it.m; WatchSeconds = 6 }
  if ($it.fight -gt 0) { $args2.FightSeconds = $it.fight }
  if ($it.play  -gt 0) { $args2.PlaySeconds  = $it.play }
  # ⚠ `2>&1` 로는 Write-Host 가 안 잡힌다 — PS5 에서 Write-Host 는 **정보 스트림(6번)**이다.
  #   그래서 첫 판이 실제로는 「판끝 예」인데 $out 이 비어 판끝X 로 찍혔다(2026-09-10).
  #   `*>&1` 로 모든 스트림을 합친다.
  $out = & (Join-Path $PSScriptRoot 'two_genuine.ps1') @args2 *>&1 | Out-String

  # 판정 추출 — 마지막 틱 줄에서 판끝·수치, 생존
  $last = ''
  foreach ($ln in ($out -split "`r?`n")) { if ($ln -match '마지막 틱') { $last = $ln } }
  $over  = if ($last -match '판끝 예') { '판끝O' } else { '판끝X' }
  $alive = if ($out -match '생존 감시.*A 끝까지 살아있음 · B 끝까지 살아있음') { '생존O' } else { '생존X' }
  $nums = ''
  foreach ($k in @('걸음','근접','주문','맞음')) {
    if ($last -match ($k + ' (\d+)')) { $nums += (' {0} {1}' -f $k, $Matches[1]) }
  }
  $ctf = ''
  if ($last -match '득점 (\d+):(\d+)') { $ctf = (' 득점 {0}:{1}' -f $Matches[1], $Matches[2]) }
  if ($last -match '잡음 (\d+)')      { $ctf += (' 잡음 {0}' -f $Matches[1]) }
  $sec = [int]((Get-Date) - $t0).TotalSeconds
  $line = ('{0,-9} {1,-6} {2} {3}{4}{5} · {6}초' -f $it.c, $it.m, $over, $alive, $nums, $ctf, $sec)
  [IO.File]::AppendAllText($resFile, $line + "`r`n", (New-Object Text.UTF8Encoding($false)))
}
[IO.File]::AppendAllText($resFile, ("끝 {0}`r`n" -f (Get-Date -Format 'HH:mm:ss')), (New-Object Text.UTF8Encoding($false)))
# 자리 비우기
Get-Process nox_server, nox_proxy, deskhost, Game3, Game4 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
