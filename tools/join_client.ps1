# join_client.ps1 — 클라를 띄워 목록의 첫 서버에 접속시킨다 (캐릭터 선택까지).
#   .\tools\join_client.ps1 -Sp .\tools -Ev C:\경로 -Exe Game2.exe -CharRow 2 -Tag c1
# CharRow: Load Character 목록의 몇 번째 줄인가 (1=첫 줄, 2=둘째 줄 …)
param([string]$Sp, [string]$Ev, [string]$Exe = 'Game2.exe', [int]$CharRow = 2, [string]$Tag = 'c')
$dir = "C:\Program Files\GOG Galaxy\Games\Nox"
function Step($n, $ok) { Write-Host ("[{0}] {1}" -f $(if ($ok) { "OK  " } else { "FAIL" }), $n); return [bool]$ok }
function Nav($procId, $x, $y, $tag, [switch]$Rel) {
  $args = @{ Pid_ = $procId; Cx = $x; Cy = $y; Click = $true; Out = (Join-Path $Ev "${Tag}_$tag.png") }
  if ($Rel) { $args.NoActivate = $true; $args.NoHome = $true }
  $r = & "$Sp\nav.ps1" @args 2>&1
  return (($r | Out-String) -match 'shot ok')
}
$p = Start-Process -FilePath "$dir\$Exe" -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 30
if (-not (Step "기동 PID=$($p.Id)" (-not $p.HasExited))) { return }
if (-not (Step "멀티플레이" (Nav $p.Id 595 337 'mp'))) { return }
Start-Sleep -Seconds 3
if (-not (Step "네트워크" (Nav $p.Id 660 540 'lan'))) { return }
Start-Sleep -Seconds 3
[void](Nav $p.Id 235 605 'ref1'); Start-Sleep -Seconds 5
if (-not (Step "서버 줄" (Nav $p.Id 527 114 'row'))) { return }
Start-Sleep -Seconds 2
# 팝업의 Join 은 상대이동으로 (커서가 팝업 밖으로 나가면 닫힌다)
if (-not (Step "참여" (Nav $p.Id 28 202 'join' -Rel))) { return }
Start-Sleep -Seconds 3
$rowY = 225 + ($CharRow - 1) * 33
if (-not (Step "캐릭터 $CharRow 줄" (Nav $p.Id 592 $rowY 'char'))) { return }
Start-Sleep -Seconds 2
if (-not (Step "불러오기" (Nav $p.Id 647 759 'load'))) { return }
Start-Sleep -Seconds 18
& "$Sp\capwin.ps1" -Pid_ $p.Id -Out (Join-Path $Ev "${Tag}_ingame.png") | Out-Null
Write-Host "PID=$($p.Id)"