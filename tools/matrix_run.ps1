# matrix_run.ps1 — 3직업 × 3모드 아홉 조합을 내리 돌리고 요약을 한 파일로 남긴다.
#
# 왜 만드나 — 「유저가 봐서 이상함 없음」 판정의 마지막 관문. 고친 것(크리처 애니·
# 정상 배치·깃발 나르기)이 **아홉 조합 전부에서** 판끝까지 가는지 한 번에 확인한다.
#
#   .\tools\matrix_run.ps1            아홉 조합 전부
#   결과  _evidence\matrix_result.txt (한 조합 한 줄)
#
# ★★★ **맵을 반드시 넘긴다** (2026-09-12 — 아홉 조합이 **전부 맵 없이** 돌던 것을 잡았다).
#   `two_genuine.ps1:133` 에 맵 빠짐 방어가 있지만 조건이 `$ServerArgs -match '--standalone'`
#   이라, `ServerArgs` 를 **아예 안 넘기면 방어를 비켜 간다.** 매트릭스가 그랬다.
#   맵이 없으면 —
#     · 벽이 없다(충돌 미검증)   · **주문 마나 표가 비어 모든 주문이 공짜**
#     · 문·오벨리스크·아이템이 없다
#   그래도 클라는 **녹음된 접속 묶음 216바이트**로 붙으므로 판은 돌아간다. 그래서 안 들켰다.
#   ⭕ 지문 — 서버 로그에 `[맵]` 과 `[주문] 마나 표 N개` 줄이 **둘 다 없다.**
param(
  [string]$MapPath = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map'
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$resFile = Join-Path $repo '_evidence\matrix_result.txt'
[IO.File]::WriteAllText($resFile, ("시작 {0}`r`n" -f (Get-Date -Format 'HH:mm:ss')), (New-Object Text.UTF8Encoding($false)))

# 조각마다 — 아레나는 싸움, 깃발·퀘스트는 판 뛰기(정상 배치)
#
# ★ **퀘스트만 시간을 늘렸다** (2026-09-12). 맵을 싣고 돌렸더니 셋 다 판끝을 못 봤는데,
#   벽에 막힌 게 아니라(「벽을 피해 당김」 0개) **다섯 스테이지에 280초가 모자랐다** —
#   warrior 는 스테이지 4 에서 **한 마리 남기고** 끝났고 wizard 는 스테이지 5 까지 갔다.
#   ⚠ 아레나·깃발은 **건드리지 않는다** — 지금 값으로 판끝까지 간다(깃발은 셋 다 득점 3).
$combos = @(
  @{ c='warrior';  m='arena'; fight=220; play=0   },
  @{ c='warrior';  m='ctf';   fight=0;   play=280 },
  @{ c='warrior';  m='quest'; fight=0;   play=480 },
  @{ c='wizard';   m='arena'; fight=220; play=0   },
  @{ c='wizard';   m='ctf';   fight=0;   play=280 },
  @{ c='wizard';   m='quest'; fight=0;   play=480 },
  @{ c='conjurer'; m='arena'; fight=220; play=0   },
  @{ c='conjurer'; m='ctf';   fight=0;   play=280 },
  @{ c='conjurer'; m='quest'; fight=0;   play=480 }
)

foreach ($it in $combos) {
  # ⚠ 첫 글자만 쓰면 warrior/wizard 가 둘 다 'w' 라 로그가 덮인다(2026-09-10 실측)
  $tag = ('M_{0}_{1}' -f $it.c, $it.m)
  $t0 = Get-Date
  # ⚠ **배열 스플래팅 금지** — PS 5.1 에서 이름 결합이 어긋나 'default' 가
  #   WaitMenu(int) 로 떨어지며 아홉 조합이 전부 0초에 죽었다(2026-09-10).
  #   해시테이블 스플래팅은 이름으로만 묶여 이 함정이 없다.
  # ★ `ServerArgs` 를 반드시 준다 — 안 주면 맵 없이 돌고, 하네스의 방어도 안 걸린다(위 주석).
  $args2 = @{ Tag = $tag; Prelude = 'default'; Class = $it.c; Mode = $it.m; WatchSeconds = 6
              ServerArgs = ('--standalone --map "' + $MapPath + '"') }
  if ($it.fight -gt 0) { $args2.FightSeconds = $it.fight }
  if ($it.play  -gt 0) { $args2.PlaySeconds  = $it.play }
  # ⚠ `2>&1` 로는 Write-Host 가 안 잡힌다 — PS5 에서 Write-Host 는 **정보 스트림(6번)**이다.
  #   그래서 첫 판이 실제로는 「판끝 예」인데 $out 이 비어 판끝X 로 찍혔다(2026-09-10).
  #   `*>&1` 로 모든 스트림을 합친다.
  $out = & (Join-Path $PSScriptRoot 'two_genuine.ps1') @args2 *>&1 | Out-String

  # 판정 추출 — 수치·생존은 마지막 틱, **판끝은 전체 틱**
  $last = ''
  foreach ($ln in ($out -split "`r?`n")) { if ($ln -match '마지막 틱') { $last = $ln } }
  $alive = if ($out -match '생존 감시.*A 끝까지 살아있음 · B 끝까지 살아있음') { '생존O' } else { '생존X' }
  $nums = ''
  foreach ($k in @('걸음','근접','주문','피해')) {
    if ($last -match ($k + ' (\d+)')) { $nums += (' {0} {1}' -f $k, $Matches[1]) }
  }
  # ★★★ **판끝을 마지막 틱으로 재면 안 된다** (2026-09-12 — 아홉 조합이 전부 `판끝X` 로
  #   찍혔는데 **실제로는 일곱이 끝나 있었다**).
  #   까닭 — 판이 끝나면 `kRoundRestartTicks` 뒤에 **재시작한다**(`Server.cpp`).
  #   그래서 「판끝 예」는 **여덟 틱 동안만 참**이고, 그 창을 지나면 마지막 틱은 늘
  #   「판끝 아니오」다. **창으로만 참인 값은 누적으로 재야 한다.**
  #   ⚠ 같은 함정을 전에도 밟았다 — 「0 은 없었다와 못 읽었다를 안 가른다」와 같은 꼴.
  #     다른 점은 0 이 아니라 `X` 였다는 것뿐이다.
  # ★ 그리고 **생존O 는 「판이 도는가」를 안 본다** — 둘을 같이 봐야 판정이 선다.
  $slog = Join-Path $repo ('_evidence\srv_{0}.log' -f $tag)
  $over = '판끝X'
  $ctf  = ''
  if (Test-Path $slog) {
    # ⚠ 서버가 아직 물고 있을 수 있으므로 공유 읽기로 연다.
    $fs = New-Object IO.FileStream($slog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try { $slt = (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Dispose() }
    $tk = @($slt -split "`r?`n" | Where-Object { $_ -match '^\[틱\]' })
    $nOver = @($tk | Where-Object { $_ -match '판끝 예' }).Count
    if ($nOver -gt 0) { $over = ('판끝O({0}틱)' -f $nOver) }
    # 득점·스테이지도 **최고점**으로 — 재시작이 값을 0 으로 되돌리기 때문이다.
    $mx = 0
    foreach ($t in $tk) {
      if ($t -match '득점 (\d+):(\d+)') {
        $s = [int]$Matches[1] + [int]$Matches[2]
        if ($s -gt $mx) { $mx = $s; $ctf = (' 최고득점 {0}:{1}' -f $Matches[1], $Matches[2]) }
      }
    }
    $st = @($slt -split "`r?`n" | Where-Object { $_ -match '\[스테이지\] (\d+)/' })
    if ($st.Count -gt 0) {
      $top = 0
      foreach ($s in $st) { if ($s -match '\[스테이지\] (\d+)/') { if ([int]$Matches[1] -gt $top) { $top = [int]$Matches[1] } } }
      $ctf += (' 최고스테이지 {0}' -f $top)
    }
  } else {
    $over = '판끝?(로그없음)'
  }
  $sec = [int]((Get-Date) - $t0).TotalSeconds
  $line = ('{0,-9} {1,-6} {2} {3}{4}{5} · {6}초' -f $it.c, $it.m, $over, $alive, $nums, $ctf, $sec)
  [IO.File]::AppendAllText($resFile, $line + "`r`n", (New-Object Text.UTF8Encoding($false)))
}
[IO.File]::AppendAllText($resFile, ("끝 {0}`r`n" -f (Get-Date -Format 'HH:mm:ss')), (New-Object Text.UTF8Encoding($false)))
# 자리 비우기
Get-Process nox_server, nox_proxy, deskhost, Game3, Game4 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
