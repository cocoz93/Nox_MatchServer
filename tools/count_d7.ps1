# count_d7.ps1 — 덤프에서 「갱신 대 d7」을 **양쪽에 똑같은 규칙**으로 센다.
#   d7 = 갱신 조각 맨 끝에 붙는 5바이트 `d7 <번호2> <지연2>` (Server.cpp:14060)
#   갱신 = 그 줄에 a4 머리가 하나라도 제대로 뜯기면 1
param([Parameter(Mandatory=$true)][string]$Dump, [string]$Name = '')
$L = [IO.File]::ReadAllLines($Dump, [Text.Encoding]::UTF8)
$recv = 0; $upd = 0; $d7 = 0; $t0 = $null; $t1 = $null
foreach ($ln in $L) {
  if ($ln -notmatch ' RECV ') { continue }
  $recv++
  if (-not $t0) { $t0 = ($ln -split '\s+')[0] }
  $t1 = ($ln -split '\s+')[0]
  $hex = ($ln -split '\|')[1]; if (-not $hex) { continue }
  $b = $hex.Trim() -split '\s+'
  $n = $b.Count
  # ⚠ 09-23 — 지연 두 바이트를 `00 00` 으로 못 박았더니 **대조군이 0 건으로 나왔다.**
  #   대조군은 `01 00`(=1)을 보낸다. 규칙이 한쪽 형식에만 맞으면 비교가 통째로 거짓이 된다
  if ($n -ge 5 -and $b[$n-5] -eq 'd7') { $d7++ }
  $ok = $false
  for ($i=0; $i -lt $n-12; $i++) {
    if ($b[$i] -ne 'a4') { continue }
    $k = $i + 1
    if ($b[$k] -eq 'ff') { $k += 5 } else { $k += 1 }
    $k += 4
    if ($k -ge $n) { break }
    $fl = [Convert]::ToInt32($b[$k],16); $k += 1
    if ($fl -band 0x80) { $k += 1 }
    $k += 1
    if ($b[$k] -eq '00') { $k += 1 }
    if ($k -lt $n) { $ok = $true; break }
  }
  if ($ok) { $upd++ }
}
$ratio = if ($d7 -gt 0) { [math]::Round($upd / $d7, 1) } else { 0 }
"{0,-10} RECV {1,6} · {2} ~ {3} · 갱신 {4,5} · d7 {5,4} · 갱신:d7 = 1:{6}" -f $Name, $recv, $t0, $t1, $upd, $d7, $ratio