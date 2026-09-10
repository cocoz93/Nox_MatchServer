# matrix_judge.ps1 — 재주행 아홉 조합의 판정을 **서버 로그에서 직접** 뽑는다.
#
# 왜 따로 두나 — 러너(matrix_run.ps1)의 요약이 Write-Host 스트림 함정으로 헛것을
# 찍은 적이 있다(2026-09-10, 판끝 예를 판끝X 로). 산출물(서버 로그·생존 로그)은
# 어차피 파일로 남으니, 판정은 **그 파일에서** 다시 뽑는 게 믿을 만하다.
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$ev = Join-Path $repo '_evidence'

function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  $r = New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)
  $t = $r.ReadToEnd(); $r.Close(); $f.Close(); return $t
}

# 새 태그(직업 이름 전체)와 옛 태그 둘 다 본다
$combos = @('warrior_arena','warrior_ctf','warrior_quest','wizard_arena','wizard_ctf','wizard_quest',
            'conjurer_arena','conjurer_ctf','conjurer_quest',
            'w_arena','w_ctf','w_quest','c_arena','c_ctf','c_quest')
foreach ($c in $combos) {
  $slog = Join-Path $ev ("srv_M_{0}.log" -f $c)
  $alog = Join-Path $ev ("alive_M_{0}.log" -f $c)
  if (-not (Test-Path $slog)) { Write-Host ("{0,-9} (아직)" -f $c); continue }
  $t = ReadShared $slog
  $ticks = @(($t -split "`r?`n") | Where-Object { $_ -match '\[틱\]' })
  if ($ticks.Count -eq 0) { Write-Host ("{0,-9} 틱 없음" -f $c); continue }
  $last = $ticks[-1]
  $over = if ($last -match '판끝 예') { '판끝O' } else { '판끝X' }
  $nums = ''
  foreach ($k in @('걸음','근접','주문','피해')) {
    if ($last -match ($k + ' (\d+)')) { if ([int]$Matches[1] -gt 0) { $nums += (' {0} {1}' -f $k, $Matches[1]) } }
  }
  if ($last -match '득점 (\d+):(\d+)') { $nums += (' 득점 {0}:{1}' -f $Matches[1], $Matches[2]) }
  if ($last -match '잡음 (\d+)')      { $nums += (' 잡음 {0}' -f $Matches[1]) }
  $alive = '생존?'
  if (Test-Path $alog) {
    $al = @((ReadShared $alog) -split "`r?`n" | Where-Object { $_ -match 'A=' })
    if ($al.Count -gt 0) {
      $alive = if ($al[-1] -match 'A=1 B=1') { '생존O' } else { '생존X(' + $al[-1] + ')' }
    }
  }
  Write-Host ("{0,-9} {1} {2}{3}" -f $c, $over, $alive, $nums)
}
