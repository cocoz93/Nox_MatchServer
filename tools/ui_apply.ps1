# ui_apply.ps1 — 게임 목록 화면 여섯째 칸(10007) 글자를 「랭크 매칭」으로 (랭크 매칭 조각 1 · Docs/RANK_MATCH_PLAN.md)
#                · 필터 창에 「완료」 버튼(★ 매칭 손보기 8 · Docs/MATCH_UX_REVIEW.md)
#                · ★ 10-31 그 버튼을 셋째 칸(y 169 — 숨긴 「리스트」 자리)으로
#
#   .\tools\ui_apply.ps1                  → 적용(이미 적용이면 그대로)
#   .\tools\ui_apply.ps1 -Mode Restore    → 되돌림(적용 전 사본과 바이트 대조)
#   .\tools\ui_apply.ps1 -Mode Check      → 상태만(원본 · 적용됨 · 모름)
#   -GameDir <폴더>                        → 다른 게임 폴더(다른 PC 로 옮길 꾸러미 쪽에 넣을 때)
#
# 고치는 것 셋 — 정품 파일이라 리포엔 안 싣고 이 스크립트가 그 자리에서 바꾼다(공개 리포에 게임 파일을 넣지 않게)
#   window\noxworld.wnd  10007(원래 「찾기」) 버튼의 글자 열쇠 Noxworld.wnd:Find → Noxworld.wnd:RankMatch
#   nox.csf              새 열쇠 Noxworld.wnd:RankMatch = 「랭크 매칭」 한 묶음을 끝에 더하고 머리의 열쇠 수 · 글 수를 +1
#   window\filter.wnd    ★ 매칭 손보기 8(`Docs/MATCH_UX_REVIEW.md`) — 필터 창(10022) 아래에 「완료」 버튼 10090 하나(누름은 DLL 이 「리스트」 길로).
#                        그림 · 글자는 정품 것(UIButtonLg 셋 · WolFind.wnd:Done = 「완료」 — 글자 파일 손 안 댐) · 자리 = 창 384x424 아래 가운데 157,372 70x20(10-30 사용자가 본 시안 `filter_done_button.png` 자리).
#                        사용자 필터 묶음(10012 · 처음엔 숨김)의 높이 280 → 220 — 내용은 y200+10 에서 끝나는데 상자가 창 끝(405)까지라 버튼 자리와 겹침.
#                        OpenNox 기준 그리기는 나중에 붙은 자식부터(`window.go` drawRecursive) — 겹치면 먼저 쓴 10012 가 버튼 위에 그려질 수 있어 겹침을 없앰
# 왜 이 꼴인가
#   · 정품은 창 파일 · 글자 파일을 디스크에서 읽는다(10-30 파일럿 — mainmenu.wnd 첫 버튼 열쇠를 바꾸니 화면 글자가 바뀜 · 원본으로 되돌리니 그대로)
#   · 글자 파일 열쇠는 읽은 뒤 정렬하고(Game.exe 0x40f3c1 qsort) 이진 탐색으로 찾는다(0x40f160) → 끝에 더해도 된다(파일 안 차례는 원래도 정렬 안 됨)
#   · 숨김은 안 푼다 — 정품이 웹(WOL) 모드 게임 목록을 만들 때 10007 을 스스로 보이게 한다(0x437948 · 숨김 비트 0x10 끔).
#     창 파일에서 풀면 LAN 모드에도 나와 원래 「사용자 찾기」 창이 뜬다. 우리 로비에서 누름을 랭크 매칭으로 돌리는 것은 DLL(조각 2)
# 되돌리기 — 적용할 때 원본을 _out\ui_backup\<이름>.<SHA256 앞 12자>.orig 로 둔다(같은 원본이면 한 번만 — 날짜 이름이면 두 번째 사본이 첫 사본을 덮는다).
#   되돌림은 거꾸로 고친 뒤 그 사본과 바이트를 대조한다(사본을 그대로 덮지 않는다 — 그 사이 GOG 가 바꾼 다른 칸이 있으면 대조에서 걸린다)
# GOG 업데이트가 두 파일을 덮으면(ddraw.dll 과 같은 처지) 이 스크립트를 다시 돌리면 된다
param(
  [ValidateSet('Apply', 'Restore', 'Check')][string]$Mode = 'Apply',
  [string]$GameDir = 'C:\Program Files\GOG Galaxy\Games\Nox')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$backupDir = Join-Path $repo '_out\ui_backup'
$latin1 = [Text.Encoding]::GetEncoding(28591)   # 바이트 그대로 글자로(창 파일은 ASCII)

$wndPath = Join-Path $GameDir 'window\noxworld.wnd'
$csfPath = Join-Path $GameDir 'nox.csf'
$blockHead = '10007 40 349 150 32 PUSHBUTTON (Find);'
# ★ 10-31 — 「랭크 매칭」을 여섯째 칸(y 349)에서 셋째 칸(y 169 — 「리스트」 10004 자리 · DLL ⑫ 가 숨김)으로(사용자 「3번째로 올리자」).
#   - 상태 셋: 원본(옛 머리 + 옛 열쇠) · 글만(옛 머리 + 새 열쇠 — 10-31 전 이 스크립트가 만든 꼴, 적용하면 자리만 더 옮김) · 적용됨(새 머리 + 새 열쇠)
#   - DLL 없는 인터넷 모드면 「리스트」와 겹침(그때 이 버튼은 정품 「찾기」라 우리 로비에서 쓸 일 없음)
$blockHeadNew = '10007 40 169 150 32 PUSHBUTTON (Find);'
$textOrig = 'TEXT = Noxworld.wnd:Find;'
$textNew = 'TEXT = Noxworld.wnd:RankMatch;'
$label = 'Noxworld.wnd:RankMatch'
$labelText = '랭크 매칭'
# ★ 매칭 손보기 8 — 필터 창. 버튼 칸은 10012 칸이 끝난 뒤 · 10022 의 자식 묶음 끝(END 둘) 앞에 — 파일이 그 세 줄로 끝나야 원본
$filterPath = Join-Path $GameDir 'window\filter.wnd'
$filterHeadOrig = '10012 30 125 340 280 USER (Parent);'
$filterHeadNew = '10012 30 125 340 220 USER (Parent);'
$filterTail = "`tEND`r`n  END`r`nEND`r`n"
$doneBlock = (@(
  "`tWINDOW",
  "`t  10090 157 372 70 20 PUSHBUTTON (Done);",
  "`t  STATUS = ENABLED+IMAGE;",
  "`t  STYLE = MOUSETRACK;",
  "`t  SELECTEDIMAGE = UIButtonLgLit;",
  "`t  HILITEIMAGE = UIButtonLgLit;",
  "`t  BACKGROUNDIMAGE = UIButtonLg;",
  "`t  ENABLEDIMAGE = NULL;",
  "`t  DISABLEDIMAGE = UIButtonLgDis;",
  "`t  TEXT = WolFind.wnd:Done;",
  "`tEND") -join "`r`n") + "`r`n"
$doneId = '10090 157 372 70 20 PUSHBUTTON (Done);'

function Count-Of([string]$hay, [string]$needle) { ([regex]::Matches($hay, [regex]::Escape($needle))).Count }

# 창 파일 — 10007 칸 안의 글자 줄이 원래 열쇠면 원본 · 새 열쇠면 적용됨. 파일 전체에서 각 줄이 몇 번인지도 본다(다른 칸에 같은 줄이 생기면 모름)
function Wnd-State([byte[]]$b) {
  $t = $latin1.GetString($b)
  $nHO = Count-Of $t $blockHead; $nHN = Count-Of $t $blockHeadNew
  if ($nHO + $nHN -ne 1) { return @{ state = '모름'; why = "10007 칸 머리 줄 — 옛 자리 $nHO · 새 자리 $nHN(합이 하나여야 함)" } }
  $moved = $nHN -eq 1
  $h = $t.IndexOf($(if ($moved) { $blockHeadNew } else { $blockHead }), [StringComparison]::Ordinal)
  $end = $t.IndexOf('END', $h, [StringComparison]::Ordinal)
  $nOrig = Count-Of $t $textOrig; $nNew = Count-Of $t $textNew
  $inOrig = $t.IndexOf($textOrig, $h, [StringComparison]::Ordinal); $inNew = $t.IndexOf($textNew, $h, [StringComparison]::Ordinal)
  $isOrig = $nOrig -eq 1 -and $nNew -eq 0 -and $inOrig -gt $h -and $inOrig -lt $end
  $isNew = $nOrig -eq 0 -and $nNew -eq 1 -and $inNew -gt $h -and $inNew -lt $end
  if (-not $moved -and $isOrig) { return @{ state = '원본'; text = $t } }
  if (-not $moved -and $isNew) { return @{ state = '글만'; text = $t } }
  if ($moved -and $isNew) { return @{ state = '적용됨'; text = $t } }
  return @{ state = '모름'; why = "새 자리 $moved · 원래 열쇠 줄 $nOrig · 새 열쇠 줄 $nNew(10007 칸 안이어야 함 · 새 자리에 옛 열쇠는 이 스크립트 꼴 아님)" }
}
# 10007 칸 머리 · 글 줄을 그 상태에서 원본 꼴로(되돌림) 또는 적용됨 꼴로(적용)
function Wnd-To([string]$t, [string]$from, [string]$to) {
  if ($from -eq '원본' -and $to -eq '적용됨') { return Swap-Once (Swap-Once $t $textOrig $textNew) $blockHead $blockHeadNew }
  if ($from -eq '글만' -and $to -eq '적용됨') { return Swap-Once $t $blockHead $blockHeadNew }
  if ($from -eq '적용됨' -and $to -eq '원본') { return Swap-Once (Swap-Once $t $textNew $textOrig) $blockHeadNew $blockHead }
  if ($from -eq '글만' -and $to -eq '원본') { return Swap-Once $t $textNew $textOrig }
  throw "noxworld.wnd — $from → $to 길 없음"
}

# 글자 파일 — 머리 24바이트(' FSC' · 판 · 열쇠 수 · 글 수 · ? · 언어) 뒤 열쇠 묶음(' LBL' · 글 수 · 이름 길이 · 이름 · 글 묶음들)을 끝까지 걷는다.
#   글 묶음 = ' rtS'(또는 'WrtS' — 뒤에 소리 이름이 하나 더) · 글자 수 · 비트 뒤집은 UTF-16. 끝에서 정확히 멈춰야 읽은 것
function Label-Bytes() {
  $name = [Text.Encoding]::ASCII.GetBytes($label)
  $u = [Text.Encoding]::Unicode.GetBytes($labelText)
  $inv = [byte[]]($u | ForEach-Object { (-bnot $_) -band 0xFF })
  $ms = New-Object IO.MemoryStream
  $w = New-Object IO.BinaryWriter($ms)
  $w.Write([Text.Encoding]::ASCII.GetBytes(' LBL')); $w.Write([uint32]1); $w.Write([uint32]$name.Length); $w.Write($name)
  $w.Write([Text.Encoding]::ASCII.GetBytes(' rtS')); $w.Write([uint32]($u.Length / 2)); $w.Write($inv)
  $w.Flush()
  return ,$ms.ToArray()
}

function Csf-State([byte[]]$b) {
  if ($b.Length -lt 24 -or $latin1.GetString($b, 0, 4) -ne ' FSC') { return @{ state = '모름'; why = '머리가 FSC 가 아님' } }
  $nLabels = [BitConverter]::ToUInt32($b, 8); $nStrings = [BitConverter]::ToUInt32($b, 12)
  $pos = 24; $seenL = 0; $seenS = 0; $mine = -1; $mineAt = -1
  while ($pos -lt $b.Length) {
    if ($latin1.GetString($b, $pos, 4) -ne ' LBL') { return @{ state = '모름'; why = "열쇠 머리가 아닌 자리 $pos(열쇠 $seenL 개 읽음)" } }
    $cnt = [BitConverter]::ToUInt32($b, $pos + 4); $nlen = [BitConverter]::ToUInt32($b, $pos + 8)
    $name = $latin1.GetString($b, $pos + 12, $nlen)
    if ($name -ieq $label) { $mine++; $mineAt = $pos }
    $pos += 12 + $nlen; $seenL++
    for ($i = 0; $i -lt $cnt; $i++) {
      $tag = $latin1.GetString($b, $pos, 4).ToUpper()
      $slen = [BitConverter]::ToUInt32($b, $pos + 4)
      $pos += 8 + 2 * $slen; $seenS++
      if ($tag -eq 'WRTS') { $pos += 4 + [BitConverter]::ToUInt32($b, $pos) }
    }
  }
  if ($pos -ne $b.Length) { return @{ state = '모름'; why = "끝을 넘음($pos / $($b.Length))" } }
  if ($seenL -ne $nLabels -or $seenS -ne $nStrings) { return @{ state = '모름'; why = "머리 수(열쇠 $nLabels · 글 $nStrings)와 읽은 수(열쇠 $seenL · 글 $seenS)가 다름" } }
  if ($mine -lt 0) { return @{ state = '원본'; labels = $seenL; strings = $seenS } }
  $lb = Label-Bytes
  $tailOk = $mine -eq 0 -and $mineAt -eq $b.Length - $lb.Length
  for ($i = 0; $tailOk -and $i -lt $lb.Length; $i++) { if ($b[$mineAt + $i] -ne $lb[$i]) { $tailOk = $false } }
  if ($tailOk) { return @{ state = '적용됨'; labels = $seenL; strings = $seenS } }
  return @{ state = '모름'; why = "$label 이 맨 끝의 이 스크립트 꼴이 아님(되돌림 못 함)" }
}

# ★ 매칭 손보기 8 — 필터 창. 원본 = 10012 원래 머리 줄 하나 · 「완료」 칸 없음 · 파일 끝이 그 세 줄 /
#   적용됨 = 새 머리 줄 하나 · 「완료」 칸 하나가 끝 두 줄 바로 앞(이 스크립트가 넣은 꼴 그대로) / 그 밖은 모름
$filterRest = "  END`r`nEND`r`n"
function Filter-State([byte[]]$b) {
  $t = $latin1.GetString($b)
  $nO = Count-Of $t $filterHeadOrig; $nN = Count-Of $t $filterHeadNew; $nD = Count-Of $t $doneId
  if ($nO -eq 1 -and $nN -eq 0 -and $nD -eq 0 -and $t.EndsWith($filterTail, [StringComparison]::Ordinal)) { return @{ state = '원본'; text = $t } }
  if ($nO -eq 0 -and $nN -eq 1 -and $nD -eq 1 -and $t.EndsWith("`tEND`r`n" + $doneBlock + $filterRest, [StringComparison]::Ordinal)) { return @{ state = '적용됨'; text = $t } }
  return @{ state = '모름'; why = "10012 원래 머리 줄 $nO · 새 머리 줄 $nN · 「완료」 칸 $nD(끝 꼴이 이 스크립트 것이 아니거나 수가 다름)" }
}
function Swap-Once([string]$t, [string]$from, [string]$to) {   # 한 번 있는 줄을 바꿈(State 함수가 수를 이미 봄)
  $i = $t.IndexOf($from, [StringComparison]::Ordinal)
  return $t.Substring(0, $i) + $to + $t.Substring($i + $from.Length)
}

function Set-Counts([byte[]]$b, [int]$delta) {
  foreach ($off in 8, 12) {
    $v = [BitConverter]::GetBytes([uint32]([BitConverter]::ToUInt32($b, $off) + $delta))
    [Array]::Copy($v, 0, $b, $off, 4)
  }
}

function Sha([byte[]]$b) {
  $s = [Security.Cryptography.SHA256]::Create()
  return ([BitConverter]::ToString($s.ComputeHash($b))).Replace('-', '')
}

# 원본 사본 — 같은 바이트면 다시 안 씀(SHA256 앞 12자가 이름에 듦)
function Save-Backup([string]$name, [byte[]]$b) {
  New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
  $p = Join-Path $backupDir ("{0}.{1}.orig" -f $name, (Sha $b).Substring(0, 12))
  if (-not (Test-Path -LiteralPath $p)) { [IO.File]::WriteAllBytes($p, $b) }
  return $p
}

function Find-Backup([string]$name, [byte[]]$restored) {
  $p = Join-Path $backupDir ("{0}.{1}.orig" -f $name, (Sha $restored).Substring(0, 12))
  if (-not (Test-Path -LiteralPath $p)) { return $null }
  $o = [IO.File]::ReadAllBytes($p)
  if ($o.Length -ne $restored.Length) { return $null }
  for ($i = 0; $i -lt $o.Length; $i++) { if ($o[$i] -ne $restored[$i]) { return $null } }
  return $p
}

foreach ($p in $wndPath, $csfPath, $filterPath) { if (-not (Test-Path -LiteralPath $p)) { throw "없는 파일: $p" } }
$wndB = [IO.File]::ReadAllBytes($wndPath)
$csfB = [IO.File]::ReadAllBytes($csfPath)
$filB = [IO.File]::ReadAllBytes($filterPath)
$ws = Wnd-State $wndB
$cs = Csf-State $csfB
$fs = Filter-State $filB
Write-Host ("[{0}] noxworld.wnd = {1}{2} · nox.csf = {3}{4} · filter.wnd = {5}{6}" -f $Mode, $ws.state, $(if ($ws.why) { " ($($ws.why))" }), $cs.state,
  $(if ($cs.why) { " ($($cs.why))" } else { " (열쇠 $($cs.labels) · 글 $($cs.strings))" }), $fs.state, $(if ($fs.why) { " ($($fs.why))" }))
if ($Mode -eq 'Check') {
  if ($ws.state -eq '적용됨' -and $cs.state -eq '적용됨' -and $fs.state -eq '적용됨') { exit 0 }
  exit 1
}
if ($ws.state -eq '모름' -or $cs.state -eq '모름' -or $fs.state -eq '모름') { throw '모르는 상태 — 손대지 않음' }

# 세 파일의 새 바이트를 다 지은 뒤에 쓴다 — 하나만 바뀐 채 멈추지 않게(10-30 시험: 글자 파일 사본이 없을 때 창 파일만 되돌아갔다)
$wndOut = $null; $csfOut = $null; $filOut = $null; $wndNote = ''; $csfNote = ''; $filNote = ''
if ($Mode -eq 'Apply') {
  if ($ws.state -ne '적용됨') { $wndOut = $latin1.GetBytes((Wnd-To $ws.text $ws.state '적용됨')) }   # ★ 10-31 — 원본 · 글만 → 적용됨
  if ($cs.state -eq '원본') {
    $lb = Label-Bytes
    $csfOut = New-Object byte[] ($csfB.Length + $lb.Length)
    [Array]::Copy($csfB, $csfOut, $csfB.Length)
    [Array]::Copy($lb, 0, $csfOut, $csfB.Length, $lb.Length)
    Set-Counts $csfOut 1
  }
  if ($fs.state -eq '원본') {   # ★ 매칭 손보기 8 — 10012 높이 · 끝 두 줄 앞에 「완료」 칸
    $t = Swap-Once $fs.text $filterHeadOrig $filterHeadNew
    $filOut = $latin1.GetBytes($t.Substring(0, $t.Length - $filterRest.Length) + $doneBlock + $filterRest)
  }
  # 사본은 원본 꼴로 — 글만 상태면 되돌린 바이트를 사본으로(되돌림 대조가 원본과 하게)
  if ($wndOut) { $wndNote = "적용($($ws.state) →) — 원본 사본 $(Save-Backup 'noxworld.wnd' $(if ($ws.state -eq '원본') { $wndB } else { $latin1.GetBytes((Wnd-To $ws.text $ws.state '원본')) }))" }
  if ($csfOut) { $csfNote = "적용 — 원본 사본 $(Save-Backup 'nox.csf' $csfB) (+$($csfOut.Length - $csfB.Length) 바이트)" }
  if ($filOut) { $filNote = "적용 — 원본 사본 $(Save-Backup 'filter.wnd' $filB) (+$($filOut.Length - $filB.Length) 바이트)" }
} else {
  if ($ws.state -ne '원본') {   # ★ 10-31 — 적용됨 · 글만 → 원본
    $wndOut = $latin1.GetBytes((Wnd-To $ws.text $ws.state '원본'))
    $bk = Find-Backup 'noxworld.wnd' $wndOut
    if (-not $bk) { throw 'noxworld.wnd — 거꾸로 고친 것과 같은 원본 사본이 없다(두 파일 다 손대지 않음)' }
    $wndNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
  if ($cs.state -eq '적용됨') {
    $lb = Label-Bytes
    $csfOut = New-Object byte[] ($csfB.Length - $lb.Length)
    [Array]::Copy($csfB, $csfOut, $csfOut.Length)
    Set-Counts $csfOut -1
    $bk = Find-Backup 'nox.csf' $csfOut
    if (-not $bk) { throw 'nox.csf — 거꾸로 고친 것과 같은 원본 사본이 없다(두 파일 다 손대지 않음)' }
    $csfNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
  if ($fs.state -eq '적용됨') {
    $t = $fs.text
    $t = $t.Substring(0, $t.Length - ($doneBlock.Length + $filterRest.Length)) + $filterRest
    $filOut = $latin1.GetBytes((Swap-Once $t $filterHeadNew $filterHeadOrig))
    $bk = Find-Backup 'filter.wnd' $filOut
    if (-not $bk) { throw 'filter.wnd — 거꾸로 고친 것과 같은 원본 사본이 없다(세 파일 다 손대지 않음)' }
    $filNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
}
if ($wndOut) { [IO.File]::WriteAllBytes($wndPath, $wndOut); Write-Host "  noxworld.wnd $wndNote" }
if ($csfOut) { [IO.File]::WriteAllBytes($csfPath, $csfOut); Write-Host "  nox.csf $csfNote" }
if ($filOut) { [IO.File]::WriteAllBytes($filterPath, $filOut); Write-Host "  filter.wnd $filNote" }
$ws2 = Wnd-State ([IO.File]::ReadAllBytes($wndPath))
$cs2 = Csf-State ([IO.File]::ReadAllBytes($csfPath))
$fs2 = Filter-State ([IO.File]::ReadAllBytes($filterPath))
Write-Host ("  지금 noxworld.wnd = {0} · nox.csf = {1} · filter.wnd = {2}" -f $ws2.state, $cs2.state, $fs2.state)
$want = if ($Mode -eq 'Apply') { '적용됨' } else { '원본' }
if ($ws2.state -ne $want -or $cs2.state -ne $want -or $fs2.state -ne $want) { throw "끝 상태가 $want 이 아님" }
Write-Host "[OK] $Mode"
