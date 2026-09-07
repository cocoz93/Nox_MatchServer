# proxy2replay.ps1 — 프록시 덤프를 대조 장치(nox_replay) 가 읽는 형식으로 바꾼다.
#
#   프록시  : 19:05:19.711 S->C 172.17.192.1:51564 len=218 op=0xed MSG_...
#             c4 45 ed 48 ...            ← 다음 줄에 hex
#   replay  : 19:05:19.711 SEND 172.17.192.1:51564 len=218 | 80 01 a9 04 ... |
#
# 방향 — S->C 는 서버가 보낸 것이므로 SEND, C->S 는 RECV.
#
# ★★ 프록시는 **와이어 그대로** 적는다 — 접속 수락 뒤로는 암호문이다.
#    대조 장치는 평문을 기대하므로(SetInboundPlain) 여기서 풀어서 넘긴다.
#    키는 수락 조각(10바이트, 평문)의 마지막 바이트에 실려 온다. 상대별로 따로 잡는다.
#
#   .\tools\proxy2replay.ps1 -In _evidence\CAP_ref_0907.txt -Out _evidence\ref_replay.txt
param(
  [Parameter(Mandatory=$true)][string]$In,
  [Parameter(Mandatory=$true)][string]$Out
)
$repo = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($In))  { $In  = Join-Path $repo $In }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repo $Out }

# ⚠ 프록시가 아직 물고 있을 수 있다 — 공유 읽기로 연다
$fs = [IO.File]::Open($In, 'Open', 'Read', 'ReadWrite')
$sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
$src = ($sr.ReadToEnd() -split "`r?`n")
$sr.Close(); $fs.Close()
$sb   = New-Object Text.StringBuilder
$keys = @{}          # 상대 → XOR 키
$n = 0; $dec = 0
for ($i = 0; $i -lt $src.Count; $i++) {
  $ln = $src[$i]
  if ($ln -notmatch '^(\S+)\s(C->S|S->C)\s(\S+)\slen=(\d+)\s') { continue }
  $time = $Matches[1]; $dir = $Matches[2]; $peer = $Matches[3]; $len = [int]$Matches[4]
  if ($i + 1 -ge $src.Count) { break }
  $hexLine = $src[$i + 1].Trim()
  if ($hexLine -eq '') { continue }
  $b = @($hexLine -split '\s+' | ForEach-Object { [Convert]::ToInt32($_, 16) })

  # 수락 조각이면 키를 얻는다 (평문). 10바이트 · [2]=0x1f · [4]=0x01
  if ($dir -eq 'S->C' -and $b.Count -eq 10 -and $b[2] -eq 0x1f) {
    $keys[$peer] = $b[9]
  } elseif ($dir -eq 'C->S' -and $keys.ContainsKey($peer)) {
    # ★ RECV(클라→서버)만 푼다. SEND 는 대조 장치가 스스로 풀므로 여기서 풀면 두 번 풀린다
    $k = $keys[$peer]
    if ($k -ne 0) { $b = @($b | ForEach-Object { $_ -bxor $k }); $dec++ }
  }

  $hex = ($b | ForEach-Object { '{0:x2}' -f $_ }) -join ' '
  $tag = if ($dir -eq 'S->C') { 'SEND' } else { 'RECV' }
  [void]$sb.AppendLine("$time $tag $peer len=$len | $hex |")
  $n++
}
[IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object Text.UTF8Encoding($false)))
Write-Host "$n 줄 변환 (그중 $dec 줄 복호화) → $Out"
Write-Host ("키: " + (($keys.GetEnumerator() | ForEach-Object { "{0}=0x{1:x2}" -f $_.Key, $_.Value }) -join ' · '))
