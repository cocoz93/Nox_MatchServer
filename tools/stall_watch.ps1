# 게임 스레드 멈춤 감시 — server.txt 의 [구간] 줄을 게임 스레드마다 따로 보고, 한 스레드의 초 값이 문턱 동안 안 바뀌면 그 스레드를 돌려준다.
#   tools/load_run.ps1 이 점-소싱해 쓴다 — 측정 판이 전부 잡기 판(M-1 · 11-18 사용자 「그렇게 하자」). 시험 = tools/stall_watch_test.ps1
#
#   왜 스레드마다: 게임 스레드가 여럿이면(--game-threads N) 줄마다 「게임 스레드 k」 칸이 붙는다. 마지막 줄 하나의 초만 보면
#   한 스레드가 멈춰도 다른 스레드 줄이 계속 와 못 잡는다(tools/stall_hunt.ps1 Read-New 는 게임 스레드 하나인 서버만 본다).
#   왜 새로 붙은 바이트만 읽나: stall_hunt.ps1 Read-New 와 같음 — 파일 끝 몇 KB 만 읽으면 판 결과 줄이 몰릴 때 [구간] 줄이 안 보여 헛잡힌다.
#   줄 끝(\n)까지만 읽은 것으로 친다 — 덜 쓴 줄과 한글 바이트가 잘리지 않게 다음에 다시 읽는다.

function New-StallWatch([string]$Path, [int]$StallSec) {
  return @{ Path = $Path; StallSec = $StallSec; Pos = [int64]0; Bytes = [int64]0; Lines = 0; Threads = @{} }
}

# 글 한 덩이(줄 끝까지 읽은 것)를 넣는다 — 파일 없이 시험하려고 읽기와 나눴다
function Add-StallText($w, [string]$Text, [double]$Now) {
  $secRx = [regex]'^\[구간\] 초 (\d+) '
  $threadRx = [regex]' · 게임 스레드 (\d+)(?: · 방 \d+\(덜 참 \d+\))?$'   # tools/load_report.ps1 $rxThread 와 같은 꼴
  foreach ($raw in $Text.Split("`n")) {
    $ln = $raw.TrimEnd("`r")   # 서버 표준 출력은 글 모드라 줄 끝이 \r\n
    $m = $secRx.Match($ln)
    if (-not $m.Success) { continue }
    $w.Lines++
    $tm = $threadRx.Match($ln)
    $k = if ($tm.Success) { [int]$tm.Groups[1].Value } else { 0 }   # 칸이 없는 옛 줄 = 0(load_report.ps1 과 같음)
    $sec = [int]$m.Groups[1].Value
    $t = $w.Threads[$k]
    if ($null -eq $t) { $t = @{ Sec = -1; ChangedAt = $Now; Lines = 0 }; $w.Threads[$k] = $t }
    $t.Lines++
    if ($sec -ne $t.Sec) { $t.Sec = $sec; $t.ChangedAt = $Now }
  }
}

function Update-StallWatch($w, [double]$Now) {
  if (-not (Test-Path -LiteralPath $w.Path)) { return }
  $got = 0
  $fs = [IO.File]::Open($w.Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  try {
    $len = $fs.Length
    if ($len -gt $w.Pos) {
      $n = [int][math]::Min($len - $w.Pos, 16MB)
      $buf = New-Object byte[] $n
      [void]$fs.Seek($w.Pos, [IO.SeekOrigin]::Begin)
      $got = $fs.Read($buf, 0, $n)
    }
  } finally { $fs.Dispose() }
  if ($got -le 0) { return }
  $end = [Array]::LastIndexOf($buf, [byte]10, $got - 1)
  if ($end -lt 0) { return }
  $w.Pos += $end + 1
  $w.Bytes += $end + 1
  Add-StallText $w ([Text.Encoding]::UTF8.GetString($buf, 0, $end + 1)) $Now
}

# 멈춘 스레드 — 본 적 있는 게임 스레드 중 초 값이 문턱 넘게 안 바뀐 것(가장 오래 안 바뀐 것 하나) · 없으면 $null · 문턱 ≤ 0 이면 늘 $null
function Get-StallVerdict($w, [double]$Now) {
  if ($w.StallSec -le 0) { return $null }
  $hit = $null
  foreach ($k in @($w.Threads.Keys | Sort-Object)) {
    $t = $w.Threads[$k]
    $since = $Now - $t.ChangedAt
    if ($since -ge $w.StallSec -and ($null -eq $hit -or $since -gt $hit.Since)) { $hit = @{ Thread = $k; Sec = $t.Sec; Since = $since } }
  }
  return $hit
}

# 지나간 양 — 「안 잡힘」이 「못 읽음」이 아님을 가른다(읽은 [구간] 줄 수 · 스레드별 · 읽은 바이트)
function Get-StallLines($w) {
  $per = @($w.Threads.Keys | Sort-Object | ForEach-Object { "게임 스레드 $_ $($w.Threads[$_].Lines)" })
  return ('읽은 [구간] 줄 {0}{1} · {2:F0}KB' -f $w.Lines, $(if ($per.Count) { '(' + ($per -join ' · ') + ')' } else { '' }), ($w.Bytes / 1KB))
}

function Get-ShortHash([string]$Path) {
  if (Test-Path -LiteralPath $Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Substring(0, 16) }
  return '없음'
}

function Get-StackSummary([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return '못 읽음 — 파일 없음' }
  $l = @([IO.File]::ReadAllLines($Path) | Where-Object { $_.StartsWith('[요약]') -or $_.StartsWith('[덤프]') })
  if ($l.Count) { return ($l -join ' / ') }
  return '못 읽음 — [요약] 줄 없음'
}

# 잡혔을 때 — nox_stack 두 묶음(10초 사이 · 둘째에 덤프 · stall_hunt.ps1 잡힘 갈래와 같은 꼴). 돌려주는 값 = 요약 두 줄.
#   ⚠ nox_stack [요약] 줄의 「게임 스레드」는 CGameThread::Run 이 있는 스레드 중 가장 먼저 생긴 것 — 게임 스레드가 여럿이면
#   멈춘 스레드가 아닐 수 있다. 스레드마다의 사슬 · CPU 몫은 stack1 · stack2 본문에 다 있다
function Invoke-StallSnap([string]$StackExe, [int]$ProcId, [string]$Dir, [string]$Sym) {
  $s1 = Join-Path $Dir 'stack1.txt'; $s2 = Join-Path $Dir 'stack2.txt'
  & $StackExe --pid $ProcId --samples 5 --gap-ms 200 --out $s1 --sym $Sym | Out-Null
  $rc1 = $LASTEXITCODE
  Start-Sleep -Seconds 10   # 둘째 묶음도 같은 함수면 「오래 걸리는 일」이 아니라 「갇힘」
  & $StackExe --pid $ProcId --samples 5 --gap-ms 200 --out $s2 --dump (Join-Path $Dir 'server.dmp') --sym $Sym | Out-Null
  $rc2 = $LASTEXITCODE
  return @("코드 $rc1 · $(Get-StackSummary $s1)", "코드 $rc2 · $(Get-StackSummary $s2)")
}
