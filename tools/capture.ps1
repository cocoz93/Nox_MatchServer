# capture.ps1 — 클라를 조작하면서 **무엇을 언제 했는지** 남긴다.
#
# 왜 표식(`say MARK …`)을 안 쓰나 —
#   09-06 에는 사람이 손으로 조작해서 **언제 무엇을 했는지 기록이 없었다.** 그래서 게임 안 채팅으로
#   눈금을 박으려 했고, 한/영 전환에 걸려 한 건도 안 들어갔다.
#   09-07 에 rcon `say` 도 시험했는데 **콘솔에 에코될 뿐 와이어로는 안 나간다**(덤프의 `0xa8` 0건).
#
#   ★ 무인 조작은 애초에 표식이 필요 없다 — **조작 시각을 남기면 된다.**
#     덤프 줄이 `14:47:19.329 SEND …` 로 시각을 달고 있으므로 그대로 맞춰진다.
#
#   .\tools\capture.ps1 -Steps idle,move,spellA,spellS,melee -Out cap_log.txt
#   .\tools\capture.ps1 -List                # 할 수 있는 동작을 보여 준다
#
# ⚠ 클라 창은 게임 안이어야 한다(MOTD 모달을 닫은 뒤). `nox.cfg` 의
#   `SendMessageOfTheDay = 0` 으로 꺼 두면 모달이 안 뜬다.
param(
  [string[]]$Steps = @(),
  [string]$Out = '',
  [string]$Exe = 'Game2',
  [int]$Gap = 1500,          # 동작 사이 기다림(ms). 덤프에서 구간이 갈리게 넉넉히
  [int]$Repeat = 2,          # 같은 동작을 몇 번 하나 (한 번만 하면 우연과 구분이 안 된다)
  [switch]$List
)
$ErrorActionPreference = 'Continue'
$Sp = $PSScriptRoot

# 동작표 — 이름 → 무엇을 하나
$ACTIONS = [ordered]@{
  'idle'    = @{ 설명 = '아무것도 안 한다 (기준선)';         kind = 'wait';  arg = 4000 }
  'move'    = @{ 설명 = '오른버튼을 눌러 걷는다';            kind = 'walk';  arg = @(1100, 500) }
  'moveNW'  = @{ 설명 = '반대쪽으로 걷는다';                  kind = 'walk';  arg = @(300, 300) }
  'melee'   = @{ 설명 = '왼버튼 = Action (근접 공격·줍기)';   kind = 'click'; arg = @(700, 450) }
  'spellA'  = @{ 설명 = 'Spell Set 의 A 키';                  kind = 'key';   arg = 'a' }
  'spellS'  = @{ 설명 = 'Spell Set 의 S 키';                  kind = 'key';   arg = 's' }
  'spellD'  = @{ 설명 = 'Spell Set 의 D 키';                  kind = 'key';   arg = 'd' }
  'spellF'  = @{ 설명 = 'Spell Set 의 F 키';                  kind = 'key';   arg = 'f' }
  'spellG'  = @{ 설명 = 'Spell Set 의 G 키';                  kind = 'key';   arg = 'g' }
  'inv'     = @{ 설명 = 'I = 인벤토리 열고 닫기';             kind = 'key';   arg = 'i' }
  'map'     = @{ 설명 = 'M = 지도';                           kind = 'key';   arg = 'm' }
  'jump'    = @{ 설명 = 'SPACE = 점프';                       kind = 'key';   arg = 'SPACE' }
  'taunt'   = @{ 설명 = 'T = 도발';                           kind = 'key';   arg = 't' }
}

if ($List) {
  "할 수 있는 동작:"
  foreach ($k in $ACTIONS.Keys) { "  {0,-8} {1}" -f $k, $ACTIONS[$k].설명 }
  return
}
if (-not $Steps -or $Steps.Count -eq 0) { "쓸 동작이 없다. -List 로 목록을 보라."; return }

$p = Get-Process -Name $Exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "[FAIL] 클라($Exe)가 안 떠 있다"; return }

$log = New-Object System.Collections.ArrayList
function Note($what, $detail) {
  # ★ 덤프 줄과 같은 꼴로 찍는다 — 14:47:19.329
  $t = (Get-Date).ToString('HH:mm:ss.fff')
  $line = "{0}  {1,-10} {2}" -f $t, $what, $detail
  [void]$log.Add($line)
  Write-Host $line
}

Note 'BEGIN' "클라 PID=$($p.Id) · 동작 $($Steps.Count)개 · 반복 $Repeat · 간격 ${Gap}ms"

foreach ($s in $Steps) {
  if (-not $ACTIONS.Contains($s)) { Note 'SKIP' "모르는 동작: $s"; continue }
  $a = $ACTIONS[$s]
  for ($r = 1; $r -le $Repeat; $r++) {
    Note $s "$($a.설명) [$r/$Repeat]"
    switch ($a.kind) {
      'wait'  { Start-Sleep -Milliseconds $a.arg }
      'walk'  { & "$Sp\nav.ps1" -Pid_ $p.Id -Cx $a.arg[0] -Cy $a.arg[1] -Click -Right -Hold 1200 -Out "$env:TEMP\cap_$s$r.png" 2>&1 | Out-Null }
      'click' { & "$Sp\nav.ps1" -Pid_ $p.Id -Cx $a.arg[0] -Cy $a.arg[1] -Click -Out "$env:TEMP\cap_$s$r.png" 2>&1 | Out-Null }
      'key'   { & "$Sp\keys.ps1" -Pid_ $p.Id -Keys $a.arg -Out "$env:TEMP\cap_$s$r.png" -Delay 300 2>&1 | Out-Null }
    }
    Note "$s.done" ''
    Start-Sleep -Milliseconds $Gap
  }
}

Note 'END' '끝'

if ($Out) {
  [IO.File]::WriteAllLines($Out, $log, (New-Object System.Text.UTF8Encoding($false)))
  "기록 → $Out"
}
