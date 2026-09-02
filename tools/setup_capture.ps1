# setup_capture.ps1 — 캡처 환경을 세운다. 끝나면 전부 살려둔다.
#
#   [클라] -> [중간자 18590] -> [정품 호스트 18591]
#
# ★ 순서가 중요하다. 평범한 클라는 다른 인스턴스가 있으면 안 뜬다.
#    클라를 먼저, -serveronly 호스트를 두 번째로.
#
#   .\tools\setup_capture.ps1 -Sp .\tools -Repo . -Ev C:\경로 -Dump 이름
param([string]$Sp, [string]$Repo, [string]$Ev, [string]$Dump = 'dump.txt')
$ErrorActionPreference = 'Continue'
$dir = "C:\Program Files\GOG Galaxy\Games\Nox"
$script:log = @()
function Step($name, $ok, $detail) {
  $mark = if ($ok) { "OK  " } else { "FAIL" }
  Write-Host "[$mark] $name  $detail"
  $script:log += "[$mark] $name"
  return [bool]$ok
}
function Nav($procId, $x, $y, $tag) {
  $r = & "$Sp\nav.ps1" -Pid_ $procId -Cx $x -Cy $y -Click -Out (Join-Path $Ev "sc_$tag.png") 2>&1
  return (($r | Out-String) -match 'shot ok')
}

Get-Process -Name Game, Game2, nox_proxy, nox_server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
try { & "$Sp\mouseaccel.ps1" -Off | Out-Null } catch { }

# 1) 클라 먼저
$cli = Start-Process -FilePath "$dir\Game.exe" -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 26
if (-not (Step "클라 기동" (-not $cli.HasExited) "PID=$($cli.Id)")) { return }
if (-not (Step "클라: 멀티플레이" (Nav $cli.Id 595 337 'cli_mp') "")) { return }
Start-Sleep -Seconds 2
if (-not (Step "클라: 네트워크" (Nav $cli.Id 660 540 'cli_lan') "")) { return }
Start-Sleep -Seconds 3

# 2) 정품 호스트를 두 번째로
$hst = Start-Process -FilePath "$dir\Game.exe" -ArgumentList '-serveronly','-port','18591' -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 26
if (-not (Step "호스트 기동" (-not $hst.HasExited) "PID=$($hst.Id)")) { return }
foreach ($s in @(@(595,337,'h_mp'), @(660,540,'h_lan'), @(228,130,'h_host'), @(647,759,'h_load'))) {
  if (-not (Step ("호스트: " + $s[2]) (Nav $hst.Id $s[0] $s[1] $s[2]) "")) { return }
  Start-Sleep -Seconds 2
}
$bound = $false
foreach ($t in 1..12) {
  Start-Sleep -Seconds 2
  if (Get-NetUDPEndpoint -LocalPort 18591 -OwningProcess $hst.Id -ErrorAction SilentlyContinue) { $bound = $true; break }
}
if (-not (Step "호스트 18591 개방" $bound "")) { return }

# 3) 중간자
$dumpPath = Join-Path $Ev $Dump
Remove-Item $dumpPath -ErrorAction SilentlyContinue
$px = Start-Process -FilePath "$Repo\build\Release\nox_proxy.exe" -ArgumentList '--dump', $dumpPath, '--quiet' -PassThru -RedirectStandardOutput (Join-Path $Ev 'proxy_out.txt') -WindowStyle Hidden
Start-Sleep -Seconds 2
[void](Step "중간자 기동" ($null -ne (Get-NetUDPEndpoint -LocalPort 18590 -OwningProcess $px.Id -ErrorAction SilentlyContinue)) "PID=$($px.Id)")

# 4) 목록 갱신 (내 중간자를 거쳐 호스트가 보이게)
foreach ($k in 1..2) {
  [void](Step "클라: 리스트 갱신 $k" (Nav $cli.Id 235 605 "refresh$k") "")
  Start-Sleep -Seconds 4
}
& "$Sp\capwin.ps1" -Pid_ $cli.Id -Out (Join-Path $Ev 'sc_list.png') | Out-Null

Write-Host ""
Write-Host "준비 완료 — 클라=$($cli.Id)  호스트=$($hst.Id)  중간자=$($px.Id)"
Write-Host "기록 파일: $dumpPath"
Write-Host "(마우스 가속은 아직 꺼져 있다. 끝나면 mouseaccel.ps1 -Restore)"
