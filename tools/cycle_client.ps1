# cycle_client.ps1 — 정품 클라를 띄워 LAN 목록 화면까지 몰고 화면을 찍는다.
#
# 내 서버가 이미 18590 을 듣고 있어야 한다. 클라가 뿌리는 서버 찾기에
# 내 서버가 답하면 목록에 뜬다.
#
#   .\tools\cycle_client.ps1 -Sp (Resolve-Path .\tools) -Out C:\경로\shot.png
param([string]$Sp, [string]$Out, [switch]$KeepAlive)
$ErrorActionPreference = 'Continue'
$dir = "C:\Program Files\GOG Galaxy\Games\Nox"
$script:log = @()
function Step($name, $ok, $detail) {
  $mark = if ($ok) { "OK  " } else { "FAIL" }
  Write-Host "[$mark] $name  $detail"
  $script:log += "[$mark] $name $detail"
  return [bool]$ok
}

Get-Process -Name Game -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$t0 = Get-Date

# 조준 계수 3.06 은 마우스 가속이 꺼진 상태의 값이다.
$accelOff = $false
try { & "$Sp\mouseaccel.ps1" -Off | Out-Null; $accelOff = $true } catch { }
[void](Step "마우스 가속 끄기" $accelOff "")

# 클라로 띄운다 (-serveronly 를 주지 않는다)
$p = Start-Process -FilePath "$dir\Game.exe" -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 26
if (-not (Step "클라 기동" (-not $p.HasExited) "PID=$($p.Id)")) {
  if ($accelOff) { & "$Sp\mouseaccel.ps1" -Restore | Out-Null }
  return
}

$steps = @(
  @{ n='멀티플레이';   x=595; y=337 },
  @{ n='네트워크LAN';  x=660; y=540 }
)
$i = 0
$ok = $true
foreach ($s in $steps) {
  $i++
  $shot = Join-Path $Sp ("cli_{0}.png" -f $i)
  $r = & "$Sp\nav.ps1" -Pid_ $p.Id -Cx $s.x -Cy $s.y -Click -Out $shot 2>&1
  $ok = (($r | Out-String) -match 'shot ok')
  [void](Step ("클릭 " + $s.n) $ok "")
  if (-not $ok) { break }
  Start-Sleep -Seconds 3
}

if ($ok) {
  # 목록 화면에 들어가면 자동으로 한 번 뿌린다. 갱신을 두 번 더 눌러 확실히 한다.
  foreach ($k in 1..2) {
    $r = & "$Sp\nav.ps1" -Pid_ $p.Id -Cx 235 -Cy 605 -Click -Out (Join-Path $Sp "cli_refresh_$k.png") 2>&1
    [void](Step "리스트 갱신 $k" (($r | Out-String) -match 'shot ok') "")
    Start-Sleep -Seconds 3
  }
  & "$Sp\capwin.ps1" -Pid_ $p.Id -Out $Out | Out-Null
  [void](Step "화면 저장" (Test-Path $Out) $Out)
}

if ($accelOff) { & "$Sp\mouseaccel.ps1" -Restore | Out-Null; [void](Step "마우스 가속 복원" $true "") }
if (-not $KeepAlive) {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  [void](Step "클라 종료" (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) "")
}
$sec = [int]((Get-Date) - $t0).TotalSeconds
$fails = ($script:log | Where-Object { $_ -match '^\[FAIL' }).Count
Write-Host ""
Write-Host "===== 클라 한 바퀴: $(if ($fails -eq 0) { '전부 통과' } else { "실패 $fails 건" }) / $sec 초 ====="
