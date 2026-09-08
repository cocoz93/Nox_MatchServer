# cap2sniff.ps1 — 프록시 덤프(CAP_*)를 서버 덤프(SNAP_*) 형식으로 바꾼다.
#
# 왜 필요한가 — 두 덤프의 생김새가 다르다. 분석 도구(`nox_opcount`)는 서버 덤프
# 형식만 읽으므로, 프록시로 뜬 것을 그대로 넣으면 **0줄로 읽히고 「0건」이 나온다**.
# 0 이 「없었다」가 아니라 「못 읽었다」인 대표적인 자리다.
#
#   프록시 :  13:31:57.455 C->S 127.0.0.1:58101 len=52 op=0x6f MSG_...
#             (다음 줄)  51 53 6f 55 ...
#   서버   :  13:31:57.455 RECV 127.0.0.1:58101 len=52 | 51 53 6f 55 ...
#
# 방향은 **서버 입장**으로 뒤집는다 — `C->S`(클라가 보냄) = 서버가 받음 = RECV.
#
#   .\tools\cap2sniff.ps1 -In _evidence\CAP_onox_ref.txt -Out _evidence\SNAP_onox_ref.txt
param(
  [Parameter(Mandatory=$true)][string]$In,
  [Parameter(Mandatory=$true)][string]$Out
)
$ErrorActionPreference = 'Stop'

# ⚠ 프록시가 **아직 쓰고 있는** 파일을 읽는 일이 잦다. 공유 읽기로 열지 않으면
#   「다른 프로세스가 쓰고 있다」로 튕긴다.
$fs = [IO.File]::Open($In, 'Open', 'Read', 'ReadWrite')
$sr = New-Object IO.StreamReader($fs)
$lines = @($sr.ReadToEnd() -split "`r?`n")
$sr.Close(); $fs.Close()
$sb = New-Object Text.StringBuilder
$n = 0
for ($i = 0; $i -lt $lines.Count; $i++) {
  $m = [regex]::Match($lines[$i], '^(\S+) (C->S|S->C) (\S+) len=(\d+)')
  if (-not $m.Success) { continue }
  # 바이트는 다음 줄에 있다
  if ($i + 1 -ge $lines.Count) { break }
  $hex = $lines[$i + 1].Trim()
  if ($hex -eq '') { continue }
  $dir = if ($m.Groups[2].Value -eq 'C->S') { 'RECV' } else { 'SEND' }
  [void]$sb.AppendLine(('{0} {1} {2} len={3} | {4}' -f `
    $m.Groups[1].Value, $dir, $m.Groups[3].Value, $m.Groups[4].Value, $hex))
  $n++
  $i++
}
[IO.File]::WriteAllText($Out, $sb.ToString())
Write-Host ("옮김 {0}줄 → {1}" -f $n, $Out)
