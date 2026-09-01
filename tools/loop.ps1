# 루프 운전 장치 — 시도 횟수를 세어 3회 실패면 멈추고, 진행 기록에 한 줄씩 붙인다.
#   .\loop.ps1 -Unit U0.1 -Attempt          시도 등록 (3회 초과면 exit 2)
#   .\loop.ps1 -Unit U0.1 -Result OK   -Detail "..."   결과 기록 (시도 횟수 초기화)
#   .\loop.ps1 -Unit U0.1 -Result FAIL -Detail "..."   결과 기록 (횟수 유지)
#   .\loop.ps1 -Status                      현재 상태 보기
param(
  [string]$Unit,
  [ValidateSet('OK','FAIL','HOLD','')] [string]$Result = '',
  [string]$Detail = '',
  [switch]$Attempt,
  [switch]$Status,
  [string]$Repo = 'C:\Users\USER\Desktop\MyGit\Nox_Server'
)
$plans = Join-Path $env:USERPROFILE 'notes\plans'
$prog  = Join-Path $plans 'nox-impl-progress.md'
$state = Join-Path $plans 'nox-loop-state.json'
$MAX = 3

function Get-State {
  if (Test-Path $state) {
    try { return (Get-Content $state -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { }
  }
  return (New-Object PSObject)
}
function Save-State($o) {
  ($o | ConvertTo-Json -Depth 5) | Set-Content $state -Encoding UTF8
}
function Get-Count($o, $u) {
  $pr = $o.PSObject.Properties[$u]
  if ($pr) { return [int]$pr.Value }
  return 0
}
function Set-Count($o, $u, $v) {
  if ($o.PSObject.Properties[$u]) { $o.$u = $v } else { $o | Add-Member -NotePropertyName $u -NotePropertyValue $v }
}

if ($Status) {
  $o = Get-State
  Write-Host "=== 시도 횟수 ==="
  $any = $false
  foreach ($pr in $o.PSObject.Properties) { Write-Host ("  {0} : {1}/{2}" -f $pr.Name, $pr.Value, $MAX); $any = $true }
  if (-not $any) { Write-Host "  (없음)" }
  Write-Host "=== 진행 기록 마지막 5줄 ==="
  if (Test-Path $prog) { Get-Content $prog -Encoding UTF8 | Where-Object { $_ -match '^- \d\d-\d\d' } | Select-Object -Last 5 | ForEach-Object { Write-Host "  $_" } }
  return
}

if (-not $Unit) { Write-Host "Unit 이 필요합니다"; exit 1 }

if ($Attempt) {
  $o = Get-State
  $n = (Get-Count $o $Unit) + 1
  Set-Count $o $Unit $n
  Save-State $o
  if ($n -gt $MAX) {
    Write-Host "STOP: $Unit 시도 $n 회 — 한도 $MAX 초과. 루프를 멈추고 사람 확인을 기다린다."
    exit 2
  }
  Write-Host "ATTEMPT ${Unit}: $n/$MAX"
  exit 0
}

if ($Result) {
  $hash = ''
  try { $hash = (& git -C $Repo rev-parse --short HEAD 2>$null) } catch { }
  $global:LASTEXITCODE = 0   # 커밋 0개인 리포에서 git 이 128 을 흘리는 것을 막는다
  if (-not $hash) { $hash = '-' }
  $o = Get-State
  $tries = Get-Count $o $Unit
  if ($Result -eq 'OK') { Set-Count $o $Unit 0; Save-State $o }
  $mark = switch ($Result) { 'OK' { '통과' } 'FAIL' { '실패' } 'HOLD' { '관측대기' } }
  $line = "- {0} / {1} / {2}{3} / ``{4}``" -f (Get-Date -Format 'MM-dd'), $Unit, $mark, $(if ($Detail) { " — $Detail" } else { "" }), $hash
  if (-not (Test-Path $prog)) { "# 녹스 서버 진행 기록`n`n## 진행`n" | Set-Content $prog -Encoding UTF8 }
  Add-Content -Path $prog -Value $line -Encoding UTF8
  Write-Host "기록: $line"
  if ($Result -ne 'OK' -and $tries -ge $MAX) { Write-Host "STOP: 한도 도달"; exit 2 }
  exit 0
}
Write-Host "할 일을 지정하세요 (-Attempt / -Result / -Status)"
exit 1
