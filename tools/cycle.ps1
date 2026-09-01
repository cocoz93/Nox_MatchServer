param([string]$Sp, [switch]$KeepAlive, [string[]]$GameArgs = @('-serveronly'), [int]$Port = 18590)
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
# 조준 계수 3.06 은 마우스 가속이 꺼진 상태에서 잰 값이다. 조작 구간에서만 끄고 끝나면 되돌린다.
$accelOff = $false
try { & "$Sp\mouseaccel.ps1" -Off | Out-Null; $accelOff = $true } catch { }
[void](Step "마우스 가속 끄기" $accelOff "")
$p = Start-Process -FilePath "$dir\Game.exe" -ArgumentList $GameArgs -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 26
if (-not (Step "게임 기동" (-not $p.HasExited) "PID=$($p.Id)")) { if ($accelOff) { & "$Sp\mouseaccel.ps1" -Restore | Out-Null }; return }
$steps = @(
  @{ n='멀티플레이';      x=595; y=337 },
  @{ n='네트워크LAN';     x=660; y=540 },
  @{ n='게임호스트하기';  x=228; y=130 },
  @{ n='캐릭터불러오기';  x=647; y=759 }
)
$i = 0
$clickOk = $true
foreach ($s in $steps) {
  $i++
  $out = Join-Path $Sp ("cyc_{0}.png" -f $i)
  $r = & "$Sp\nav.ps1" -Pid_ $p.Id -Cx $s.x -Cy $s.y -Click -Out $out 2>&1
  $joined = ($r | Out-String)
  $ok = $joined -match 'shot ok'
  [void](Step ("클릭 " + $s.n) $ok ($(if ($ok) { "" } else { "-> " + ($joined -replace "`r?`n", " | ").Trim() })))
  if (-not $ok) { $clickOk = $false; break }
  Start-Sleep -Seconds 2
}
$bound = $false
$waited = 0
if ($clickOk) {
  for ($t = 0; $t -lt 12; $t++) {
    Start-Sleep -Seconds 2
    $waited += 2
    if (Get-NetUDPEndpoint -LocalPort $Port -OwningProcess $p.Id -ErrorAction SilentlyContinue) { $bound = $true; break }
  }
}
[void](Step "UDP $Port 개방" $bound "대기 $waited 초")
$got = $false
if ($bound) {
  $r = & "$Sp\probe.ps1" -Targets @('127.0.0.1') -Port $Port -WaitMs 3000 2>&1
  $txt = ($r | Out-String)
  $got = ($txt -match 'REPLY #1') -and ($txt -match 'len=82')
  [void](Step "프로브 왕복 82바이트" $got "")
  if ($got) { Write-Host ("   " + (($txt -split "`r?`n" | Where-Object { $_ -match 'HEX:' } | Select-Object -First 1).Trim())) }
}
if (-not $KeepAlive) {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  [void](Step "게임 종료" (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) "")
}
if ($accelOff) { try { & "$Sp\mouseaccel.ps1" -Restore | Out-Null } catch { }
  [void](Step "마우스 가속 복원" $true "") }
$sec = [int]((Get-Date) - $t0).TotalSeconds
$fails = ($script:log | Where-Object { $_ -match '^\[FAIL' }).Count
Write-Host ""
Write-Host "===== 한 바퀴: $(if ($fails -eq 0) { '전부 통과' } else { "실패 $fails 건" }) / $sec 초 ====="
