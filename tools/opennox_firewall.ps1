# opennox_firewall.ps1 — OpenNox 실행파일의 바깥 통신을 막고, 윈도우가 멋대로 만든 허용 규칙을 지운다.
#
# 왜 — 2026-09-04 실측.
#   ① opennox 서버가 xwis.net:4000 (로비) 과 api.ipify.org 로 나갔다
#   ② UPnP 로 공유기에 포트를 뚫으려 했다
#   ③ ★ 윈도우가 자동으로 "들어오는 허용" 규칙을 만들어 뒀다 (공용 프로필).
#      이 PC 는 공인 IP 를 직접 물고 있어서 그게 곧 인터넷 노출이다.
#
# 로컬 실험에는 바깥 통신이 필요 없다. 루프백(127.0.0.1)은 방화벽을 안 탄다.
# 규칙은 실행파일 "경로"로 매칭하므로, 빌드 결과물을 옮기면 다시 걸어야 한다.
#
# 관리자 권한으로 실행할 것. 실행 기록은 opennox_firewall.log 에 남는다.
#   .\opennox_firewall.ps1            규칙 정리 + 차단 규칙 생성
#   .\opennox_firewall.ps1 -Remove    내가 만든 차단 규칙 제거
#   .\opennox_firewall.ps1 -Status    현재 상태 보기 (권한 불필요)

param([switch]$Remove, [switch]$Status)

$ErrorActionPreference = 'Continue'
$logFile = Join-Path $PSScriptRoot 'opennox_firewall.log'
function Say($m) {
    Write-Host $m
    try { Add-Content -Path $logFile -Value ((Get-Date -Format 'HH:mm:ss') + '  ' + $m) -Encoding UTF8 } catch {}
}

$tag  = 'OpenNox 차단'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\third_party') -ErrorAction SilentlyContinue).Path
$dirs = @()
foreach ($n in @('opennox', 'opennox-build')) {
    $d = Join-Path $root $n
    if (Test-Path $d) { $dirs += $d }
}

Say "=== 시작 ==="
Say ("대상 폴더 " + $dirs.Count + "개: " + ($dirs -join ' | '))

if ($Status) {
    Say "--- 내 차단 규칙 ---"
    $r = Get-NetFirewallRule -DisplayName "$tag*" -ErrorAction SilentlyContinue
    if ($r) { Say ("  " + @($r).Count + "개") } else { Say '  없음' }
    Say "--- opennox 를 가리키는 다른 규칙 (윈도우가 만든 것 포함) ---"
    $all = Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -notlike "$tag*" }
    $hits = $all | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue | Where-Object { $_.Program -like '*opennox*' }
    if ($hits) { $hits | ForEach-Object { $rl = $_ | Get-NetFirewallRule; Say ("  [" + $rl.Action + "/" + $rl.Direction + "] " + $rl.DisplayName + "  ->  " + $_.Program) } }
    else { Say '  없음' }
    exit 0
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say '관리자 권한이 없다. PowerShell 을 관리자로 열고 다시 실행할 것.'
    Read-Host '엔터를 누르면 닫힘'
    exit 1
}
Say ('관리자 권한: 예 (' + $id.Name + ')')

# 1) 내가 전에 만든 차단 규칙 제거
$old = Get-NetFirewallRule -DisplayName "$tag*" -ErrorAction SilentlyContinue
if ($old) { $old | Remove-NetFirewallRule -ErrorAction SilentlyContinue; Say ('내 옛 규칙 ' + @($old).Count + '개 제거') }

# 2) ★ 윈도우가 자동으로 만든 opennox 관련 규칙 전부 제거 (허용 규칙이 여기 섞여 있다)
$auto = @()
foreach ($rl in (Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -notlike "$tag*" })) {
    $f = $rl | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue
    if ($f -and $f.Program -like '*opennox*') { $auto += $rl }
}
if ($auto) {
    foreach ($rl in $auto) { Say ('  자동생성 규칙 제거 [' + $rl.Action + '/' + $rl.Direction + '] ' + $rl.DisplayName) }
    $auto | Remove-NetFirewallRule -ErrorAction SilentlyContinue
    Say ('자동생성 규칙 ' + @($auto).Count + '개 제거')
} else { Say '자동생성 규칙 없음' }

if ($Remove) { Say '제거만 하고 끝'; Read-Host '엔터를 누르면 닫힘'; exit 0 }
if ($dirs.Count -eq 0) { Say 'third_party 밑에 opennox 폴더가 없음 - 중단'; Read-Host '엔터를 누르면 닫힘'; exit 1 }

# 3) 차단 규칙 생성 (폴더별로 전부)
$n = 0
foreach ($d in $dirs) {
    $label = Split-Path $d -Leaf
    foreach ($exe in Get-ChildItem "$d\*.exe" -ErrorAction SilentlyContinue) {
        try {
            New-NetFirewallRule -DisplayName "$tag - $label\$($exe.Name) (나가는)" -Direction Outbound -Program $exe.FullName -Action Block -Profile Any -ErrorAction Stop | Out-Null
            New-NetFirewallRule -DisplayName "$tag - $label\$($exe.Name) (들어오는)" -Direction Inbound  -Program $exe.FullName -Action Block -Profile Any -ErrorAction Stop | Out-Null
            Say ("  차단 2개 - " + $label + "\" + $exe.Name)
            $n += 2
        } catch {
            Say ("  실패 - " + $exe.Name + " : " + $_.Exception.Message)
        }
    }
}
Say ("총 " + $n + "개 생성.")
Say "=== 끝 ==="
Read-Host '엔터를 누르면 닫힘'