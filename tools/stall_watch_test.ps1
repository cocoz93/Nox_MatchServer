# 멈춤 감시(tools/stall_watch.ps1) 시험 — 함수만(가) + load_run.ps1 를 가짜 서버로 통째(나)
#   (가) 글을 직접 넣어 갈래마다 본다 — 계속 바뀜 · 문턱 경계 · 스레드 둘 중 하나 멈춤 · 판 결과 줄만 옴 · 줄 없음 · 같은 초 되풀이 · 문턱 0 · 파일 읽기(덜 쓴 줄 · \r\n)
#   (나) load_run.ps1 -FakeServerArgs — 서버 = nox_stack --victim fakeserver · 더미 = nox_stack --victim sleep. PC 검사는 문턱을 풀어 지나가게(잔열 두 번 읽기 약 50초는 남는다)
#        잡힘 · 안 잡힘 · 감시 끔 · 서버 죽음(멈춤으로 안 셈) 넷. 판 폴더는 _out\load_run\<시각>_stallwatch_*
#
# 사용: powershell -NoProfile -ExecutionPolicy Bypass -File tools\stall_watch_test.ps1 [-OnlyUnit]
# 종료 코드: 0 = 다 통과 · 10 = 틀림 있음
param([switch]$OnlyUnit)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stall_watch.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$script:pass = 0; $script:bad = 0
function Check([string]$Name, [bool]$Ok, [string]$Got) {
  Write-Host ('{0} {1} — {2}' -f $(if ($Ok) { '[통과]' } else { '[틀림]' }), $Name, $Got)
  if ($Ok) { $script:pass++ } else { $script:bad++ }
}
function Sec([int]$s, [int]$k = 0) { return "[구간] 초 $s · 틱 30 · 바쁨 50.0% · CPU 조정 0 · 게임 스레드 $k · 방 3(덜 참 0)`r`n" }
function VerdictText($v) { if ($null -eq $v) { return '없음' } return ('스레드 {0} · 초 {1} · {2:F1}초' -f $v.Thread, $v.Sec, $v.Since) }

# ── (가) 함수 ─────────────────────────────────────────────
# 1 계속 바뀜 — 30초 동안 매초 새 초 값
$w = New-StallWatch '' 10
for ($t = 1; $t -le 30; $t++) { Add-StallText $w (Sec $t) $t }
$v = Get-StallVerdict $w 30
Check '가1 계속 바뀜 → 안 잡힘' ($null -eq $v -and $w.Lines -eq 30) "$(VerdictText $v) · 줄 $($w.Lines)"

# 2 문턱 경계 — 5초까지 오고 끊김. 14초(9초 지남)엔 안 잡힘 · 15초(10초 지남)엔 잡힘
$w = New-StallWatch '' 10
for ($t = 1; $t -le 5; $t++) { Add-StallText $w (Sec $t) $t }
$v14 = Get-StallVerdict $w 14; $v15 = Get-StallVerdict $w 15
Check '가2 9초 지남 → 안 잡힘 · 10초 지남 → 잡힘(스레드 0 · 초 5)' ($null -eq $v14 -and $null -ne $v15 -and $v15.Thread -eq 0 -and $v15.Sec -eq 5) "14초 $(VerdictText $v14) · 15초 $(VerdictText $v15)"

# 3 스레드 둘 중 1번만 멈춤 — 0번 줄은 계속 온다(마지막 줄 하나만 보면 못 잡는 꼴)
$w = New-StallWatch '' 10
for ($t = 1; $t -le 30; $t++) { $txt = Sec $t 0; if ($t -le 5) { $txt += Sec $t 1 }; Add-StallText $w $txt $t }
$v = Get-StallVerdict $w 30
Check '가3 스레드 둘 중 1번 멈춤 → 1번 잡힘' ($null -ne $v -and $v.Thread -eq 1 -and $v.Sec -eq 5) "$(VerdictText $v) · 스레드 $(@($w.Threads.Keys | Sort-Object) -join ',')"

# 4 판 결과 줄만 옴 — [구간] 아닌 줄은 「바뀜」으로 안 친다
$w = New-StallWatch '' 10
for ($t = 1; $t -le 5; $t++) { Add-StallText $w (Sec $t) $t }
for ($t = 6; $t -le 20; $t++) { Add-StallText $w "[판 결과] 1-$t · 방 3 · estate(arena) · score_player · 승자 #1 · 82초`r`n" $t }
$v = Get-StallVerdict $w 20
Check '가4 판 결과 줄만 옴 → 잡힘' ($null -ne $v -and $v.Thread -eq 0 -and $w.Lines -eq 5) "$(VerdictText $v) · 줄 $($w.Lines)"

# 5 [구간] 줄이 아직 없음 — 시작 전은 잡지 않는다
$w = New-StallWatch '' 10
Add-StallText $w "옵션: 구간 시간 켬`r`n[게임데이터] 읽음`r`n" 1
$v = Get-StallVerdict $w 100
Check '가5 [구간] 줄 없음 → 안 잡힘 · 줄 0' ($null -eq $v -and $w.Lines -eq 0) "$(VerdictText $v) · 줄 $($w.Lines)"

# 6 같은 초 값이 되풀이 — 줄은 오는데 초가 안 바뀜 = 멈춤으로 친다
$w = New-StallWatch '' 10
for ($t = 1; $t -le 5; $t++) { Add-StallText $w (Sec $t) $t }
for ($t = 6; $t -le 20; $t++) { Add-StallText $w (Sec 5) $t }
$v = Get-StallVerdict $w 20
Check '가6 같은 초 되풀이 → 잡힘(초 5 · 15초)' ($null -ne $v -and $v.Sec -eq 5 -and [math]::Abs($v.Since - 15) -lt 0.01) "$(VerdictText $v)"

# 7 문턱 0 = 끔
$w = New-StallWatch '' 0
for ($t = 1; $t -le 5; $t++) { Add-StallText $w (Sec $t) $t }
$v = Get-StallVerdict $w 1000
Check '가7 문턱 0 → 안 잡힘' ($null -eq $v) "$(VerdictText $v)"

# 8 파일 읽기 — 덜 쓴 줄은 다음에 · \r\n 줄 끝에서 스레드 칸을 읽는다(스레드 1)
$tmp = Join-Path $env:TEMP "stall_watch_test_$PID.txt"
$u8 = New-Object Text.UTF8Encoding($false)
$part = Sec 2 1
[IO.File]::WriteAllText($tmp, (Sec 1 1) + $part.Substring(0, 20), $u8)
$w = New-StallWatch $tmp 10
Update-StallWatch $w 1
$l1 = $w.Lines
[IO.File]::AppendAllText($tmp, $part.Substring(20), $u8)
Update-StallWatch $w 2
$keys = @($w.Threads.Keys | Sort-Object) -join ','
Check '가8 파일 — 덜 쓴 줄 다음에 · 스레드 칸 1' ($l1 -eq 1 -and $w.Lines -eq 2 -and $keys -eq '1' -and $w.Threads[1].Sec -eq 2) "첫 읽기 줄 $l1 · 둘째 $($w.Lines) · 스레드 $keys · $(Get-StallLines $w)"
[IO.File]::Delete($tmp)

if ($OnlyUnit) {
  Write-Host "시험 $script:pass/$($script:pass + $script:bad) 통과(함수만)"
  if ($script:bad) { exit 10 } else { exit 0 }
}

# ── (나) load_run.ps1 통째 · 가짜 서버 ──────────────────────
$loadRun = Join-Path $PSScriptRoot 'load_run.ps1'
$plan = ((powercfg /getactivescheme) -replace '.*GUID:\s*([0-9a-fA-F-]+).*', '$1').Trim()
$wu = [string](Get-Service wuauserv).Status
$loose = @('-UptimeMaxHours', '100000', '-IdleMaxPercent', '100', '-ResidualMaxPercent', '100', '-ResidualMaxReads', '2', '-PowerPlanGuid', $plan, '-UpdateServiceStatus', $wu)
function Run-Fake([string]$Tag, [string]$Fake, [int]$Sec, [int]$Stall) {
  $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $loadRun -Tag $Tag -Sec $Sec -StallSec $Stall -FakeServerArgs $Fake @loose 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $dir = $null
  foreach ($ln in $out) { $m = [regex]::Match($ln, '^\[끝\] (.+?) · 서버 종료 코드'); if ($m.Success) { $dir = $m.Groups[1].Value } }
  $envMap = @{}
  if ($dir -and (Test-Path -LiteralPath (Join-Path $dir 'env.txt'))) {
    foreach ($ln in [IO.File]::ReadAllLines((Join-Path $dir 'env.txt'))) { $i = $ln.IndexOf(': '); if ($i -gt 0) { $envMap[$ln.Substring(0, $i)] = $ln.Substring($i + 2) } }
  }
  return @{ Code = $code; Dir = $dir; Env = $envMap; Out = $out }
}
function Has([string]$Dir, [string]$Leaf) { return ($Dir -and (Test-Path -LiteralPath (Join-Path $Dir $Leaf))) }

# 나1 잡힘 — 8초에 갇힘 · 더미(sleep 60초)는 감시가 죽이고 판이 일찍 끝난다 · 서버는 끝내기 신호를 못 들어 강제 종료
$r = Run-Fake 'stallwatch_hit' '--stall-at 8' 60 10
$sw = [string]$r.Env['멈춤 감시']
$mAt = [regex]::Match($sw, '더미 띄운 뒤 (\d+)초')
Check '나1 잡힘 — 종료 코드 0 · 「멈춤 감시」 잡힘 · 게임 스레드 0 · 마지막 초 7~8' ($r.Code -eq 0 -and $sw -match '^잡힘 · 게임 스레드 0 · 마지막 \[구간\] 초 [78] ') "코드 $($r.Code) · $sw"
Check '나1 일찍 끝남(더미 띄운 뒤 < 60초)' ($mAt.Success -and [int]$mAt.Groups[1].Value -lt 60) "$($mAt.Value)"
$c1 = [string]$r.Env['멈춤 호출 사슬 1']; $c2 = [string]$r.Env['멈춤 호출 사슬 2']
$dmp = if ($r.Dir) { Join-Path $r.Dir 'server.dmp' } else { '' }
Check '나1 사슬 둘에 VictimSpin · 덤프 · .pdb 복사' ($c1.Contains('VictimSpin') -and $c2.Contains('VictimSpin') -and (Has $r.Dir 'server.dmp') -and (Get-Item -LiteralPath $dmp).Length -gt 0 -and (Has $r.Dir 'nox_stack.pdb')) "$c1 / $c2"
Check '나1 서버 강제 종료 · 「시간 안에 안 끝나」 줄 없음' ($r.Env['서버가 멈춤 이벤트로 끝남'] -eq 'False' -and -not ($r.Out -match '더미가 시간 안에 안 끝나')) "멈춤 이벤트로 끝남 $($r.Env['서버가 멈춤 이벤트로 끝남'])"

# 나2 안 잡힘 — 갇히지 않는 가짜 25초 · 지나간 양(읽은 [구간] 줄)이 0 이 아님
$r = Run-Fake 'stallwatch_none' '--stall-at 0' 25 10
$sw = [string]$r.Env['멈춤 감시']
$mL = [regex]::Match($sw, '읽은 \[구간\] 줄 (\d+)')
Check '나2 안 잡힘 · 읽은 줄 ≥ 15 · 사슬 파일 없음 · 멈춤 이벤트로 끝남' ($r.Code -eq 0 -and $sw.StartsWith('안 잡힘') -and $mL.Success -and [int]$mL.Groups[1].Value -ge 15 -and -not (Has $r.Dir 'stack1.txt') -and $r.Env['서버가 멈춤 이벤트로 끝남'] -eq 'True') "코드 $($r.Code) · $sw"

# 나3 감시 끔 — 갇혀도 안 잡는다(옛 꼴: 더미가 끝날 때까지 기다리고 서버는 강제 종료)
$r = Run-Fake 'stallwatch_off' '--stall-at 5' 20 0
$sw = [string]$r.Env['멈춤 감시']
Check '나3 끔 · 사슬 파일 없음 · 서버 강제 종료' ($r.Code -eq 0 -and $sw -eq '끔(-StallSec 0)' -and -not (Has $r.Dir 'stack1.txt') -and $r.Env['서버가 멈춤 이벤트로 끝남'] -eq 'False') "코드 $($r.Code) · $sw"

# 나4 서버 죽음 — 5초에 코드 3 으로 끝남 · [구간] 은 끊기지만 멈춤으로 안 센다
$r = Run-Fake 'stallwatch_die' '--die-at 5 --die-code 3' 20 10
$sw = [string]$r.Env['멈춤 감시']
Check '나4 서버 죽음 → 안 잡힘 · 서버 종료 코드 3 · 사슬 파일 없음' ($r.Code -eq 0 -and $sw.StartsWith('안 잡힘') -and $r.Env['서버 종료 코드'] -eq '3' -and -not (Has $r.Dir 'stack1.txt')) "코드 $($r.Code) · $sw · 서버 종료 코드 $($r.Env['서버 종료 코드'])"

Write-Host "시험 $script:pass/$($script:pass + $script:bad) 통과"
if ($script:bad) { exit 10 } else { exit 0 }
