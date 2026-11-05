# ui_apply.ps1 — 게임 목록 화면 여섯째 칸(10007) 글자를 「랭크 매칭」으로 (랭크 매칭 조각 1 · Docs/RANK_MATCH_PLAN.md)
#                · 필터 창에 「완료」 버튼(★ 매칭 손보기 8 · Docs/MATCH_UX_REVIEW.md)
#                · ★ 10-31 그 버튼을 셋째 칸(y 169 — 숨긴 「리스트」 자리)으로
#                · ★ 11-04 안 되는 단추 글자 「(미구현)」 · 「서버」 · 「호출!」 숨김 · 입장 문구(Docs/MO_UX_FIX_PLAN_1104.md)
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
#   ★ 11-04 — 대화방 · 찾기 · 게임 목록의 글자 묶음
#   window\wolchat.wnd   1913 감정표현 · 1926 클랜 · 1927 랭킹 확인(옛 래더 — 칸 폭도 넓힘) · 1902 찾기 글자를 새 열쇠로 · 1914 「서버」 · ★ 11-05 1904 옵션 숨김
#   window\wolfind.wnd   1932 「호출!」 숨김
#   window\noxworld.wnd  10003 퀘스트 호스트 글자를 새 열쇠로(누름은 DLL 이 막음 · ⑩)
#   nox.csf              새 열쇠 다섯을 끝에 더함 · 기존 열쇠 cdecode.c:EnterChat 값만 「방에 들어왔습니다.」로 바꿈(코드가 그 열쇠로 찾아 창 파일로는 못 돌림)
#                        다른 기존 열쇠는 값을 안 바꾼다 — Noxworld.c:Ladder 처럼 다른 화면도 같은 열쇠를 쓰기 때문(`noxworld.c:515` · 원본 기준)
#   ★ 11-04 플랜2 — window\MoRecord.wnd 새 파일(대전 기록 창 · 되돌리면 지움) · 입구 단추 글 「대전 기록」 · 새 열쇠 MoRecord.wnd:* 열네 개
#   ★ 11-04 전적 검색 — window\MoSearch.wnd 새 파일(되돌리면 지움 · 내용은 MoRecord.wnd 에서 지어냄) · 1902 글자 「전적 검색」 · 새 열쇠 셋(옛 열쇠 WolChat.wnd:FindTodo 는 끝 차례를 지키려고 남김)
#   ★ 11-04 플랜1 — window\MoStart.wnd 새 파일(시작 단추 · 되돌리면 지움) · 새 열쇠 MoStart.wnd:Start · 값 바꾸는 열쇠 WolFind.wnd:WindowTitle(찾기 창 제목)
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
# ★ 11-04 — 글자 파일 끝에 더할 열쇠. 이 차례 그대로 붙는다(10-30 판이 적용된 파일은 RankMatch 하나만 있어 그 뒤를 잇는다)
$csfAdds = @(
  @{ name = $label; text = $labelText },
  @{ name = 'Noxworld.wnd:HostQuestTodo'; text = '퀘스트 호스트 (미구현)' },
  @{ name = 'WolChat.wnd:ClansTodo'; text = '클랜 (미구현)' },
  @{ name = 'WolChat.wnd:Record'; text = '대전 기록' },
  @{ name = 'WolChat.wnd:ActionTodo'; text = '감정표현 (미구현)' },
  @{ name = 'WolChat.wnd:FindTodo'; text = '찾기 (미구현)' },
  @{ name = 'MoStart.wnd:Start'; text = '게임시작' },
  @{ name = 'MoRecord.wnd:Title'; text = '대전 기록' },
  @{ name = 'MoRecord.wnd:Name'; text = '이름' },
  @{ name = 'MoRecord.wnd:Rating'; text = '점수' },
  @{ name = 'MoRecord.wnd:Rank'; text = '순위' },
  @{ name = 'MoRecord.wnd:Ranked'; text = '랭크' },
  @{ name = 'MoRecord.wnd:Normal'; text = '일반' },
  @{ name = 'MoRecord.wnd:Result'; text = '결과' },
  @{ name = 'MoRecord.wnd:Kind'; text = '종류' },
  @{ name = 'MoRecord.wnd:Delta'; text = '증감' },
  @{ name = 'MoRecord.wnd:Map'; text = '맵' },
  @{ name = 'MoRecord.wnd:KillDeath'; text = '킬/데스' },
  @{ name = 'MoRecord.wnd:Time'; text = '시간' },
  @{ name = 'MoRecord.wnd:Date'; text = '날짜' },
  @{ name = 'MoRecord.wnd:Blank'; text = ' ' },
  @{ name = 'WolChat.wnd:Search'; text = '전적 검색' },
  @{ name = 'MoSearch.wnd:Title'; text = '전적 검색' },
  @{ name = 'MoSearch.wnd:Go'; text = '검색' })
# 값을 바꾸는 기존 열쇠 — 값이 둘 다 아니면(다른 판 정품) 모름으로 멈춘다
$csfSwaps = @(
  @{ name = 'cdecode.c:EnterChat'; orig = '채팅실 들어가는 중...'; new = '방에 들어왔습니다.' },
  # ★ 11-06 플랜1 조각 6 — 게임 목록의 핑 칸 자리에 맵 이름을 넣는다(DLL ㉓ · 핑은 이 서버에서 늘 `--`). 이 열쇠는 noxworld.wnd 머리 칸 하나만 쓴다(필터 창은 Noxworld.wnd:FilterPing)
  @{ name = 'Noxworld.wnd:Ping'; orig = '핑'; new = '맵' },
  # ★ 11-05 — 서버가 판 안 이름을 로비 이름으로 덮으면 방에 들 때 클라가 한 번 찍는 문구(`cdecode.c:1470` · 실물 _out/qa_1105/o1/o0_room_2s_c1.png). 왜 바뀌었는지 알게
  @{ name = 'Cdecode.c:NameChange'; orig = '서버에서 당신의 이름을 %s로 바꾸었습니다.'; new = '게임 안에서는 계정 이름 %s(으)로 표시됩니다.' },
  @{ name = 'WolFind.wnd:WindowTitle'; orig = '찾기/호출'; new = '유저 찾기' })   # ★ 11-04 플랜1 — 「호출!」 단추를 숨겨 제목만 남아 있었음. wolfind.wnd 만 쓰는 열쇠
# ★ 11-04 플랜1 — 새 창 파일. 원본 = 없음 · 적용됨 = 이 바이트 그대로 · 다르면 모름(남의 파일을 지우지 않게)
#   ENABLEDIMAGE = NULL — 정품은 꺼져도 앞 그림(ENABLED)을 그린다. 꺼진 그림은 뒤 칸만 바꿔(`widget_button.go` buttonDrawImg · 원본 기준) 앞 그림이 있으면 빨강 그대로(11-04 캡처 s2)
#   글자 · 칸 색은 정품 창과 같은 꼴 · 머리 좌표 0 0(DLL 이 옮김 — 판 밖 좌표는 파일로 못 넣음) · 그림은 대화방 「뒤로」와 같은 큰 단추(시안)
$newFiles = @(
  @{ file = 'window\MoStart.wnd'; text = (@(
    'ENABLEDCOLOR = TRANSPARENT;', 'HILITECOLOR = TRANSPARENT;', 'DISABLEDCOLOR = TRANSPARENT;', 'SELECTEDCOLOR = TRANSPARENT;', 'TEXTCOLOR = 240 180 42;', 'BACKGROUNDCOLOR = TRANSPARENT;', '',
    'WINDOW', '  9500 0 0 140 40 USER;', '  STATUS = ENABLED+ABOVE;', '  CHILD', '    WINDOW', '      9501 0 0 140 40 PUSHBUTTON;', '      STATUS = ENABLED+IMAGE+NOFOCUS;', '      STYLE = MOUSETRACK;', '      IMAGEOFFSET = 0 5;',
    '      SELECTEDIMAGE = NoxWorldChatButton8;', '      HILITEIMAGE = NoxWorldChatButton8;', '      BACKGROUNDIMAGE = NoxWorldChatButton8;', '      DISABLEDIMAGE = NoxWorldMapButton1Dis;', '      ENABLEDIMAGE = NULL;',
    '      TEXT = MoStart.wnd:Start;', '    END', '  END', 'END', '') -join "`r`n") },
  # ★ 11-04 플랜2 — 대전 기록 창(정품 게임 목록과 같은 바탕 + 왼쪽 칸 · 열마다 목록 칸 · 뒤로). 글자는 열쇠만 · 값은 DLL 이 줄을 받아 넣는다(`RecDraw`)
  #   왼쪽 칸이 길다(시안의 나무 띠는 그림을 늘려 합성한 것이라 창 파일로 못 넣음 — 바탕의 빈 단추 셋을 덮는다) · 머리 칸은 그림을 못 늘려 글자 칸 + 정품 테두리
  @{ file = 'window\MoRecord.wnd'; text = (@(
    'ENABLEDCOLOR = 0 150 0;', 'HILITECOLOR = 0 200 0;', 'DISABLEDCOLOR = 128 128 128;', 'SELECTEDCOLOR = 230 128 128;', 'TEXTCOLOR = 240 180 42;', 'BACKGROUNDCOLOR = 0 0 0;',
    '', 'WINDOW', '  9600 0 0 640 480 USER;', '  STATUS = ENABLED+IMAGE+ABOVE+NOFOCUS;', '  BACKGROUNDIMAGE = NoxWorldBkg;', '  CHILD',
    '', '    WINDOW', '      9602 40 37 158 358 USER;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      ENABLEDCOLOR = 200 150 60;', '      BACKGROUNDCOLOR = 0 0 0;',
    '      CHILD', '', '        WINDOW', '          9603 0 12 158 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;', '          BACKGROUNDCOLOR = TRANSPARENT;',
    '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 230 230 230;', '          DATA = 1 0 MoRecord.wnd:Title;', '          FONT = default;',
    '        END', '', '        WINDOW', '        9604 8 32 142 1 USER;', '        STATUS = ENABLED+NOFOCUS;', '        ENABLEDCOLOR = 200 150 60;',
    '        BACKGROUNDCOLOR = 200 150 60;', '        END', '', '        WINDOW', '          9620 10 44 40 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 170 170 170;', '          DATA = 0 0 MoRecord.wnd:Name;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9630 52 44 100 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 240 180 42;', '          DATA = 0 0 MoRecord.wnd:Blank;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9621 10 68 40 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 170 170 170;', '          DATA = 0 0 MoRecord.wnd:Rating;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9631 52 68 100 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 240 180 42;', '          DATA = 0 0 MoRecord.wnd:Blank;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9622 10 92 40 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 170 170 170;', '          DATA = 0 0 MoRecord.wnd:Rank;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9632 52 92 100 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 240 180 42;', '          DATA = 0 0 MoRecord.wnd:Blank;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9623 10 132 40 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 170 170 170;', '          DATA = 0 0 MoRecord.wnd:Ranked;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9633 52 132 100 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 240 180 42;', '          DATA = 0 0 MoRecord.wnd:Blank;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9624 10 156 40 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 170 170 170;', '          DATA = 0 0 MoRecord.wnd:Normal;',
    '          FONT = default;', '        END', '', '        WINDOW', '          9634 52 156 100 15 STATICTEXT;', '          STATUS = ENABLED+NOFOCUS;',
    '          BACKGROUNDCOLOR = TRANSPARENT;', '          DISABLEDCOLOR = TRANSPARENT;', '          ENABLEDCOLOR = TRANSPARENT;', '          HILITECOLOR = TRANSPARENT;', '          TEXTCOLOR = 240 180 42;', '          DATA = 0 0 MoRecord.wnd:Blank;',
    '          FONT = default;', '        END', '', '        WINDOW', '        9605 8 120 142 1 USER;', '        STATUS = ENABLED+NOFOCUS;',
    '        ENABLEDCOLOR = 200 150 60;', '        BACKGROUNDCOLOR = 200 150 60;', '        END', '', '      END', '    END',
    '', '    WINDOW', '      9640 224 30 34 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Result;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9641 259 30 36 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Kind;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9642 296 30 38 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Rating;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9643 335 30 34 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Delta;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9644 370 30 68 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Map;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9645 439 30 50 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:KillDeath;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9646 490 30 34 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Time;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9647 525 30 72 20 STATICTEXT;', '      STATUS = ENABLED+BORDER+NOFOCUS;', '      BACKGROUNDCOLOR = 96 16 8;', '      ENABLEDCOLOR = 96 16 8;',
    '      DISABLEDCOLOR = 96 16 8;', '      HILITECOLOR = 96 16 8;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 1 MoRecord.wnd:Date;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9610 224 54 34 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9611 259 54 36 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9612 296 54 38 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9613 335 54 34 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9614 370 54 68 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9615 439 54 50 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9616 490 54 34 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9617 525 54 72 390 SCROLLLISTBOX;', '      STATUS = ENABLED+NOFOCUS+ONE_LINE;', '      DATA = 40 10 0 0 0 0 0;', '      ENABLEDCOLOR = TRANSPARENT;',
    '      BACKGROUNDCOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      SELECTEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9601 224 220 373 15 STATICTEXT;', '      STATUS = ENABLED+NOFOCUS;', '      BACKGROUNDCOLOR = TRANSPARENT;', '      DISABLEDCOLOR = TRANSPARENT;',
    '      ENABLEDCOLOR = TRANSPARENT;', '      HILITECOLOR = TRANSPARENT;', '      TEXTCOLOR = 240 180 42;', '      DATA = 1 0 MoRecord.wnd:Blank;', '      FONT = default;', '    END',
    '', '    WINDOW', '      9690 40 408 150 32 PUSHBUTTON;', '      STATUS = ENABLED+IMAGE+NOFOCUS;', '      STYLE = MOUSETRACK;', '      BACKGROUNDIMAGE = NULL;',
    '      SELECTEDIMAGE = NoxWorldMapButton7;', '      ENABLEDIMAGE = NULL;', '      HILITEIMAGE = NoxWorldMapButton7;', '      DISABLEDIMAGE = NULL;', '      TEXT = Noxworld.wnd:Back;', '    END',
    '', '  END', 'END', '') -join "`r`n") })
# ★ 11-04 전적 검색 — 전적 검색 창 = 대전 기록 창 + 왼쪽 칸 줄을 아래로 + 이름 칸 · 검색 단추(둘 다 왼쪽 칸 9602 의 자식 — 맨 위 창의 자식은 9602 에 가려짐 · r17 캡처)
#   자식 번호가 같아야 DLL 이 그리기를 나눠 쓴다(`RecDraw`) · 두 파일을 따로 적으면 칸이 갈라지므로 여기서 지어낸다 · 이 칸 줄이 대전 기록 창에 없으면 멈춘다
#   이름 칸 DATA = 글자 수 10(끝 글자 빼고 9 = 계정 이름 상한 kAccountNameMax) · 칸 그림 폭 138(칸 폭과 같게 — 작으면 오른쪽으로 붙는다 · r18 캡처) · 0 · 2(영문 · 숫자만 — 숫자 · 영문 제한이 있어야 한국어 판의 문자 입력 길을 안 탄다 · `GAME2_2.c` 원본 기준)
#   검색 단추 NOFOCUS = 단추에 마우스만 올려도 포커스가 옮겨 가 글자가 끊긴다(원본 기준)
function Make-Search([string]$t) {
  $swaps = @(
    @('DATA = 1 0 MoRecord.wnd:Title;', 'DATA = 1 0 MoSearch.wnd:Title;'),
    @('9604 8 32 142 1 USER;', '9604 8 96 142 1 USER;'),
    @('9620 10 44 40 15 STATICTEXT;', '9620 10 108 40 15 STATICTEXT;'), @('9630 52 44 100 15 STATICTEXT;', '9630 52 108 100 15 STATICTEXT;'),
    @('9621 10 68 40 15 STATICTEXT;', '9621 10 132 40 15 STATICTEXT;'), @('9631 52 68 100 15 STATICTEXT;', '9631 52 132 100 15 STATICTEXT;'),
    @('9622 10 92 40 15 STATICTEXT;', '9622 10 156 40 15 STATICTEXT;'), @('9632 52 92 100 15 STATICTEXT;', '9632 52 156 100 15 STATICTEXT;'),
    @('9605 8 120 142 1 USER;', '9605 8 184 142 1 USER;'),
    @('9623 10 132 40 15 STATICTEXT;', '9623 10 196 40 15 STATICTEXT;'), @('9633 52 132 100 15 STATICTEXT;', '9633 52 196 100 15 STATICTEXT;'),
    @('9624 10 156 40 15 STATICTEXT;', '9624 10 220 40 15 STATICTEXT;'), @('9634 52 156 100 15 STATICTEXT;', '9634 52 220 100 15 STATICTEXT;'))
  foreach ($s in $swaps) {
    if (([regex]::Matches($t, [regex]::Escape($s[0]))).Count -ne 1) { throw "MoRecord.wnd 에 칸 줄이 하나가 아님: $($s[0])" }
    $t = $t.Replace($s[0], $s[1])
  }
  $add = (@(
    '        WINDOW', '          9650 10 36 138 25 ENTRYFIELD;', '          STATUS = ENABLED+BORDER+ONE_LINE;', '          STYLE = MOUSETRACK+TABSTOP;',
    '          ENABLEDCOLOR = 200 150 60;', '          TEXTCOLOR = 240 180 42;', '          TEXT = WolChat.wnd:Empty;', '          DATA = 10 138 0 2;', '        END', '',
    '        WINDOW', '          9651 44 68 70 20 PUSHBUTTON;', '          STATUS = ENABLED+IMAGE+NOFOCUS;', '          STYLE = MOUSETRACK;',
    '          SELECTEDIMAGE = UIButtonLgLit;', '          HILITEIMAGE = UIButtonLgLit;', '          BACKGROUNDIMAGE = UIButtonLg;', '          ENABLEDIMAGE = NULL;', '          DISABLEDIMAGE = UIButtonLgDis;',
    '          TEXT = MoSearch.wnd:Go;', '        END', '', '') -join "`r`n")
  $at = "        WINDOW`r`n          9620 10 108 40 15 STATICTEXT;"
  if (([regex]::Matches($t, [regex]::Escape($at))).Count -ne 1) { throw 'MoRecord.wnd 에 넣을 자리가 하나가 아님' }
  return $t.Replace($at, $add + $at)
}
$newFiles += @{ file = 'window\MoSearch.wnd'; text = (Make-Search ($newFiles | Where-Object { $_.file -eq 'window\MoRecord.wnd' }).text) }

# 창 파일 줄 바꿈 — head 칸(머리 줄 ~ 다음 END) 안의 from 줄을 to 로. 파일마다 전부 원본이거나 전부 적용이어야 한다(섞이면 일부 — 적용 · 되돌림으로 맞춤)
$lineEdits = @(
  @{ file = 'window\noxworld.wnd'; head = '10003 40 109 150 32 PUSHBUTTON'; from = 'TEXT = Noxworld.wnd:HostQuest;'; to = 'TEXT = Noxworld.wnd:HostQuestTodo;' },
  # ★ 11-06 플랜1 조각 6 — 게임 목록 선택색. 정품 초록 바탕이면 상태 칸 초록 글자(DLL ㉓)가 묻힌다 · 회색 값은 제 판단. 줄 배경은 선택한 줄에만 칠한다(`GAME3.c:1275` · 원본 기준)
  @{ file = 'window\noxworld.wnd'; head = '10038 16 0 100 400 SCROLLLISTBOX'; from = 'SELECTEDCOLOR = 0 150 0;'; to = 'SELECTEDCOLOR = 70 70 70;' },
  @{ file = 'window\noxworld.wnd'; head = '10039 116 0 50 400 SCROLLLISTBOX'; from = 'SELECTEDCOLOR = 0 150 0;'; to = 'SELECTEDCOLOR = 70 70 70;' },
  @{ file = 'window\noxworld.wnd'; head = '10040 166 0 75 400 SCROLLLISTBOX'; from = 'SELECTEDCOLOR = 0 150 0;'; to = 'SELECTEDCOLOR = 70 70 70;' },
  @{ file = 'window\noxworld.wnd'; head = '10041 241 0 50 400 SCROLLLISTBOX'; from = 'SELECTEDCOLOR = 0 150 0;'; to = 'SELECTEDCOLOR = 70 70 70;' },
  @{ file = 'window\noxworld.wnd'; head = '10042 291 0 90 400 SCROLLLISTBOX'; from = 'SELECTEDCOLOR = 0 150 0;'; to = 'SELECTEDCOLOR = 70 70 70;' },
  @{ file = 'window\wolchat.wnd'; head = '1926 151 433 82 26 PUSHBUTTON'; from = 'TEXT = WolChat.wnd:Clans;'; to = 'TEXT = WolChat.wnd:ClansTodo;' },
  # 1927 — 「랭킹 확인 (미구현)」이 폭 82 를 넘어 두 줄로 꺾여 창 밖으로 잘렸다(11-04 캡처). 글자는 칸 폭을 넘으면 꺾이므로(`widget_button.go` DrawStringWrapped · 원본 기준)
  #        칸을 98 로 넓힘 · 오른쪽 끝이 「뒤로」 머리에 닿아(11-04 캡처) 왼쪽으로 3 더 옮겨 가운데 189(판 가운데 192) · 그림 자리는 그대로 되게 IMAGEOFFSET X 를 11 더함
  @{ file = 'window\wolchat.wnd'; head = 'PUSHBUTTON (Ladder);'; from = 'TEXT = Noxworld.c:Ladder;'; to = 'TEXT = WolChat.wnd:Record;' },
  @{ file = 'window\wolchat.wnd'; head = 'PUSHBUTTON (Ladder);'; from = '1927 151 453 82 26 PUSHBUTTON'; to = '1927 140 453 98 26 PUSHBUTTON' },
  @{ file = 'window\wolchat.wnd'; head = 'PUSHBUTTON (Ladder);'; from = 'IMAGEOFFSET = 1 2;'; to = 'IMAGEOFFSET = 12 2;' },
  @{ file = 'window\wolchat.wnd'; head = '1913 40 430 109 30 PUSHBUTTON'; from = 'TEXT = WolChat.wnd:Action;'; to = 'TEXT = WolChat.wnd:ActionTodo;' },
  # 1902 — mid = 앞 판이 적용한 꼴(「찾기 (미구현)」) · 적용 · 되돌림 어느 쪽으로든 지나간다
  @{ file = 'window\wolchat.wnd'; head = '1902 448 375 109 25 PUSHBUTTON'; from = 'TEXT = WolChat.wnd:FindPage;'; to = 'TEXT = WolChat.wnd:Search;'; mid = 'TEXT = WolChat.wnd:FindTodo;' },
  @{ file = 'window\wolchat.wnd'; head = '1914 42 453 109 30 PUSHBUTTON'; from = 'STATUS = ENABLED+IMAGE+NOFOCUS;'; to = 'STATUS = ENABLED+IMAGE+HIDDEN+NOFOCUS;' },
  # 1904 옵션 — 「웨스트우드 온라인 옵션」 창이 그대로 뜨는데 욕설 필터 · 찾기 · 호출 · 위치 변경 모두 서버가 버려 뜻이 없다(Lobby.cpp SETOPT · 11-05 전수검사 #5)
  @{ file = 'window\wolchat.wnd'; head = '1904 238 361 109 55 PUSHBUTTON'; from = 'STATUS = ENABLED+IMAGE+NOFOCUS;'; to = 'STATUS = ENABLED+IMAGE+HIDDEN+NOFOCUS;' },
  @{ file = 'window\wolfind.wnd'; head = '1932 100 30 70 20 PUSHBUTTON'; from = 'STATUS = ENABLED+IMAGE;'; to = 'STATUS = ENABLED+IMAGE+HIDDEN;' })
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
function Label-Bytes([string]$name, [string]$text) {
  $n = [Text.Encoding]::ASCII.GetBytes($name)
  $u = [Text.Encoding]::Unicode.GetBytes($text)
  $inv = [byte[]]($u | ForEach-Object { (-bnot $_) -band 0xFF })
  $ms = New-Object IO.MemoryStream
  $w = New-Object IO.BinaryWriter($ms)
  $w.Write([Text.Encoding]::ASCII.GetBytes(' LBL')); $w.Write([uint32]1); $w.Write([uint32]$n.Length); $w.Write($n)
  $w.Write([Text.Encoding]::ASCII.GetBytes(' rtS')); $w.Write([uint32]($u.Length / 2)); $w.Write($inv)
  $w.Flush()
  return ,$ms.ToArray()
}
function Same-At([byte[]]$b, [int]$at, [byte[]]$x) {
  if ($at -lt 0 -or $at + $x.Length -gt $b.Length) { return $false }
  for ($i = 0; $i -lt $x.Length; $i++) { if ($b[$at + $i] -ne $x[$i]) { return $false } }
  return $true
}
function Csf-Str([byte[]]$b, [int]$at, [int]$chars) {   # 글 묶음의 글자(비트 뒤집은 UTF-16)
  $u = New-Object byte[] (2 * $chars)
  for ($i = 0; $i -lt $u.Length; $i++) { $u[$i] = (-bnot $b[$at + $i]) -band 0xFF }
  return [Text.Encoding]::Unicode.GetString($u)
}

# ★ 11-04 — 상태 넷: 원본(더한 열쇠 0 · 값 원래) · 적용됨(전부 · 값 새것) · 일부(그 사이 — 10-30 판 파일이 여기) · 모름
function Csf-State([byte[]]$b) {
  if ($b.Length -lt 24 -or $latin1.GetString($b, 0, 4) -ne ' FSC') { return @{ state = '모름'; why = '머리가 FSC 가 아님' } }
  $nLabels = [BitConverter]::ToUInt32($b, 8); $nStrings = [BitConverter]::ToUInt32($b, 12)
  $pos = 24; $seenL = 0; $seenS = 0; $seen = @{}; $swaps = @()
  while ($pos -lt $b.Length) {
    if ($latin1.GetString($b, $pos, 4) -ne ' LBL') { return @{ state = '모름'; why = "열쇠 머리가 아닌 자리 $pos(열쇠 $seenL 개 읽음)" } }
    $cnt = [BitConverter]::ToUInt32($b, $pos + 4); $nlen = [BitConverter]::ToUInt32($b, $pos + 8)
    $name = $latin1.GetString($b, $pos + 12, $nlen)
    $at = $pos
    foreach ($add in $csfAdds) { if ($name -ieq $add.name) { $seen[$add.name] = $at } }
    $swap = $null
    foreach ($c in $csfSwaps) { if ($name -ieq $c.name) { $swap = $c } }
    $pos += 12 + $nlen; $seenL++
    for ($i = 0; $i -lt $cnt; $i++) {
      $tag = $latin1.GetString($b, $pos, 4).ToUpper()
      $slen = [BitConverter]::ToUInt32($b, $pos + 4)
      if ($swap) {
        if ($cnt -ne 1 -or $tag -ne ' RTS') { return @{ state = '모름'; why = "$($swap.name) 이 글 하나짜리 꼴이 아님" } }
        $s = Csf-Str $b ($pos + 8) $slen
        if ($s -ceq $swap.orig) { $on = $false } elseif ($s -ceq $swap.new) { $on = $true } else { return @{ state = '모름'; why = "$($swap.name) 값이 원래 값도 새 값도 아님" } }
        $swaps += @{ at = $pos; chars = $slen; on = $on; orig = $swap.orig; new = $swap.new }
      }
      $pos += 8 + 2 * $slen; $seenS++
      if ($tag -eq 'WRTS') { $pos += 4 + [BitConverter]::ToUInt32($b, $pos) }
    }
  }
  if ($pos -ne $b.Length) { return @{ state = '모름'; why = "끝을 넘음($pos / $($b.Length))" } }
  if ($seenL -ne $nLabels -or $seenS -ne $nStrings) { return @{ state = '모름'; why = "머리 수(열쇠 $nLabels · 글 $nStrings)와 읽은 수(열쇠 $seenL · 글 $seenS)가 다름" } }
  if ($swaps.Count -ne $csfSwaps.Count) { return @{ state = '모름'; why = "값 바꿀 열쇠 $($csfSwaps.Count) 개 중 $($swaps.Count) 개만 있음" } }
  $swapOnCount = @($swaps | Where-Object { $_.on }).Count
  # 더한 열쇠는 csfAdds 앞에서부터 k 개가 이 차례 그대로 파일 끝에 있어야 한다(되돌릴 때 잘라 내므로)
  $k = $seen.Count; $tail = 0; $tailOk = $true
  for ($i = 0; $i -lt $k; $i++) { if (-not $seen.ContainsKey($csfAdds[$i].name)) { $tailOk = $false } else { $tail += (Label-Bytes $csfAdds[$i].name $csfAdds[$i].text).Length } }
  $at = $b.Length - $tail
  for ($i = 0; $tailOk -and $i -lt $k; $i++) {
    $lb = Label-Bytes $csfAdds[$i].name $csfAdds[$i].text
    if ($seen[$csfAdds[$i].name] -ne $at -or -not (Same-At $b $at $lb)) { $tailOk = $false }
    $at += $lb.Length
  }
  if (-not $tailOk) { return @{ state = '모름'; why = "더한 열쇠 $k 개가 맨 끝의 이 스크립트 꼴이 아님(되돌림 못 함)" } }
  $st = if ($k -eq 0 -and $swapOnCount -eq 0) { '원본' } elseif ($k -eq $csfAdds.Count -and $swapOnCount -eq $swaps.Count) { '적용됨' } else { '일부' }
  return @{ state = $st; labels = $seenL; strings = $seenS; k = $k; tail = $tail; swaps = $swaps }
}

# 원본 꼴(want = 원본) 또는 적용됨 꼴로 지은 새 바이트
function Csf-To([byte[]]$b, $cs, [string]$want) {
  $ms = New-Object IO.MemoryStream
  $body = $b.Length - $cs.tail   # 더한 열쇠 앞까지
  $from = 0
  foreach ($sw in @($cs.swaps | Sort-Object { $_.at })) {   # 파일 차례대로 — 앞 값의 길이가 달라도 뒤 자리는 원본 기준
    $text = if ($want -eq '적용됨') { $sw.new } else { $sw.orig }
    $u = [Text.Encoding]::Unicode.GetBytes($text)
    $inv = [byte[]]($u | ForEach-Object { (-bnot $_) -band 0xFF })
    $ms.Write($b, $from, $sw.at + 4 - $from)
    $ms.Write([BitConverter]::GetBytes([uint32]($u.Length / 2)), 0, 4)
    $ms.Write($inv, 0, $inv.Length)
    $from = $sw.at + 8 + 2 * $sw.chars
  }
  $ms.Write($b, $from, $body - $from)
  $delta = -$cs.k
  if ($want -eq '적용됨') {
    foreach ($add in $csfAdds) { $lb = Label-Bytes $add.name $add.text; $ms.Write($lb, 0, $lb.Length) }
    $delta = $csfAdds.Count - $cs.k
  }
  $out = $ms.ToArray()
  Set-Counts $out $delta
  return ,$out
}

# ★ 11-04 — 창 파일 줄 바꿈. 칸 머리부터 다음 END 사이의 줄만 본다(같은 줄이 다른 칸에도 있다 — STATUS)
function Lines-State([string]$t, $edits) {
  $on = 0
  foreach ($e in $edits) {
    $n = Count-Of $t $e.head
    if ($n -ne 1) { return @{ state = '모름'; why = "칸 머리 「$($e.head)」 $n 번" } }
    $h = $t.LastIndexOf("`n", $t.IndexOf($e.head, [StringComparison]::Ordinal)) + 1   # 칸 = 머리가 든 줄의 첫 글자부터(머리 줄의 좌표도 바꾸는 줄이 있다)
    $end = $t.IndexOf('END', $h, [StringComparison]::Ordinal)
    $block = $t.Substring($h, $end - $h)
    $f = Count-Of $block $e.from; $g = Count-Of $block $e.to
    $m = if ($e.mid) { Count-Of $block $e.mid } else { 0 }
    if ($f + $g + $m -ne 1) { return @{ state = '모름'; why = "「$($e.head)」 칸 안 — 원래 줄 $f · 앞 판 줄 $m · 새 줄 $g(합이 하나여야 함)" } }
    if ($g -eq 1) { $on++ }
  }
  $st = if ($on -eq 0) { '원본' } elseif ($on -eq $edits.Count) { '적용됨' } else { '일부' }
  return @{ state = $st }
}
function Lines-To([string]$t, $edits, [string]$want) {
  foreach ($e in $edits) {
    $h = $t.LastIndexOf("`n", $t.IndexOf($e.head, [StringComparison]::Ordinal)) + 1   # 칸 = 머리가 든 줄의 첫 글자부터(머리 줄의 좌표도 바꾸는 줄이 있다)
    $end = $t.IndexOf('END', $h, [StringComparison]::Ordinal)
    $block = $t.Substring($h, $end - $h)
    $x = if ($want -eq '적용됨') { $block.Replace($e.from, $e.to) } else { $block.Replace($e.to, $e.from) }
    if ($e.mid) { $x = $x.Replace($e.mid, $(if ($want -eq '적용됨') { $e.to } else { $e.from })) }
    $t = $t.Substring(0, $h) + $x + $t.Substring($end)
  }
  return $t
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

$otherFiles = @($lineEdits | ForEach-Object { $_.file } | Where-Object { $_ -ne 'window\noxworld.wnd' } | Select-Object -Unique)
$wndLineEdits = @($lineEdits | Where-Object { $_.file -eq 'window\noxworld.wnd' })
foreach ($p in @($wndPath, $csfPath, $filterPath) + @($otherFiles | ForEach-Object { Join-Path $GameDir $_ })) { if (-not (Test-Path -LiteralPath $p)) { throw "없는 파일: $p" } }
$wndB = [IO.File]::ReadAllBytes($wndPath)
$csfB = [IO.File]::ReadAllBytes($csfPath)
$filB = [IO.File]::ReadAllBytes($filterPath)
$ws = Wnd-State $wndB
$wl = Lines-State ($latin1.GetString($wndB)) $wndLineEdits
$cs = Csf-State $csfB
$fs = Filter-State $filB
$others = @{}
foreach ($f in $otherFiles) {
  $bytes = [IO.File]::ReadAllBytes((Join-Path $GameDir $f))
  $others[$f] = @{ bytes = $bytes; st = (Lines-State ($latin1.GetString($bytes)) @($lineEdits | Where-Object { $_.file -eq $f })) }
}
function Say-Others($map) { (@($otherFiles | ForEach-Object { "{0} = {1}{2}" -f (Split-Path -Leaf $_), $map[$_].st.state, $(if ($map[$_].st.why) { " ($($map[$_].st.why))" }) }) -join ' · ') }
Write-Host ("[{0}] noxworld.wnd = {1}{2} · 그 안 11-04 줄 = {3}{4} · nox.csf = {5}{6} · filter.wnd = {7}{8} · {9}" -f $Mode, $ws.state, $(if ($ws.why) { " ($($ws.why))" }), $wl.state, $(if ($wl.why) { " ($($wl.why))" }), $cs.state,
  $(if ($cs.why) { " ($($cs.why))" } else { " (열쇠 $($cs.labels) · 글 $($cs.strings))" }), $fs.state, $(if ($fs.why) { " ($($fs.why))" }), (Say-Others $others))
# ★ 11-04 플랜1 — 새 창 파일 상태
function New-State($nf) {
  $p = Join-Path $GameDir $nf.file
  if (-not (Test-Path -LiteralPath $p)) { return '원본' }
  if ($latin1.GetString([IO.File]::ReadAllBytes($p)) -ceq $nf.text) { return '적용됨' }
  return '모름'
}
$newStates = @($newFiles | ForEach-Object { New-State $_ })
Write-Host ("  새 파일 " + ((0..($newFiles.Count - 1) | ForEach-Object { "{0} = {1}" -f (Split-Path -Leaf $newFiles[$_].file), $newStates[$_] }) -join ' · '))
$allStates = @($ws.state, $wl.state, $cs.state, $fs.state) + @($otherFiles | ForEach-Object { $others[$_].st.state }) + $newStates
if ($Mode -eq 'Check') {
  if (@($allStates | Where-Object { $_ -ne '적용됨' }).Count -eq 0) { exit 0 }
  exit 1
}
if ($allStates -contains '모름') { throw '모르는 상태 — 손대지 않음' }

# 파일의 새 바이트를 다 지은 뒤에 쓴다 — 하나만 바뀐 채 멈추지 않게(10-30 시험: 글자 파일 사본이 없을 때 창 파일만 되돌아갔다)
$want = if ($Mode -eq 'Apply') { '적용됨' } else { '원본' }
$wndOut = $null; $csfOut = $null; $filOut = $null; $wndNote = ''; $csfNote = ''; $filNote = ''; $otherOut = @{}; $otherNote = @{}
# noxworld.wnd 원본 꼴 = 10007 칸도 11-04 줄도 원본 — 사본 · 대조는 언제나 이 꼴로
$wndText = $latin1.GetString($wndB)
$wndOrigText = Lines-To $wndText $wndLineEdits '원본'
if ($ws.state -ne '원본') { $wndOrigText = Wnd-To $wndOrigText $ws.state '원본' }
$csfOrig = Csf-To $csfB $cs '원본'
if ($Mode -eq 'Apply') {
  if ($ws.state -ne '적용됨' -or $wl.state -ne '적용됨') {   # ★ 10-31 — 원본 · 글만 → 적용됨 / ★ 11-04 줄도 같이
    $x = if ($ws.state -ne '적용됨') { Wnd-To $wndText $ws.state '적용됨' } else { $wndText }
    $wndOut = $latin1.GetBytes((Lines-To $x $wndLineEdits '적용됨'))
    # 사본은 원본 꼴로 — 글만 · 옛 적용 상태면 되돌린 바이트를 사본으로(되돌림 대조가 원본과 하게)
    $wndNote = "적용(10007 $($ws.state) · 11-04 줄 $($wl.state) →) — 원본 사본 $(Save-Backup 'noxworld.wnd' $latin1.GetBytes($wndOrigText))"
  }
  if ($cs.state -ne '적용됨') {
    $csfOut = Csf-To $csfB $cs '적용됨'
    $csfNote = "적용($($cs.state) →) — 원본 사본 $(Save-Backup 'nox.csf' $csfOrig) (+$($csfOut.Length - $csfB.Length) 바이트)"
  }
  if ($fs.state -eq '원본') {   # ★ 매칭 손보기 8 — 10012 높이 · 끝 두 줄 앞에 「완료」 칸
    $t = Swap-Once $fs.text $filterHeadOrig $filterHeadNew
    $filOut = $latin1.GetBytes($t.Substring(0, $t.Length - $filterRest.Length) + $doneBlock + $filterRest)
    $filNote = "적용 — 원본 사본 $(Save-Backup 'filter.wnd' $filB) (+$($filOut.Length - $filB.Length) 바이트)"
  }
  foreach ($f in $otherFiles) {
    if ($others[$f].st.state -eq '적용됨') { continue }
    $edits = @($lineEdits | Where-Object { $_.file -eq $f })
    $tx = $latin1.GetString($others[$f].bytes)
    $otherOut[$f] = $latin1.GetBytes((Lines-To $tx $edits '적용됨'))
    $otherNote[$f] = "적용 — 원본 사본 $(Save-Backup (Split-Path -Leaf $f) $latin1.GetBytes((Lines-To $tx $edits '원본')))"
  }
} else {
  if ($ws.state -ne '원본' -or $wl.state -ne '원본') {   # ★ 10-31 — 적용됨 · 글만 → 원본
    $wndOut = $latin1.GetBytes($wndOrigText)
    $bk = Find-Backup 'noxworld.wnd' $wndOut
    if (-not $bk) { throw 'noxworld.wnd — 거꾸로 고친 것과 같은 원본 사본이 없다(모든 파일 손대지 않음)' }
    $wndNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
  if ($cs.state -ne '원본') {
    $csfOut = $csfOrig
    $bk = Find-Backup 'nox.csf' $csfOut
    if (-not $bk) { throw 'nox.csf — 거꾸로 고친 것과 같은 원본 사본이 없다(모든 파일 손대지 않음)' }
    $csfNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
  if ($fs.state -eq '적용됨') {
    $t = $fs.text
    $t = $t.Substring(0, $t.Length - ($doneBlock.Length + $filterRest.Length)) + $filterRest
    $filOut = $latin1.GetBytes((Swap-Once $t $filterHeadNew $filterHeadOrig))
    $bk = Find-Backup 'filter.wnd' $filOut
    if (-not $bk) { throw 'filter.wnd — 거꾸로 고친 것과 같은 원본 사본이 없다(모든 파일 손대지 않음)' }
    $filNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
  foreach ($f in $otherFiles) {
    if ($others[$f].st.state -eq '원본') { continue }
    $edits = @($lineEdits | Where-Object { $_.file -eq $f })
    $otherOut[$f] = $latin1.GetBytes((Lines-To ($latin1.GetString($others[$f].bytes)) $edits '원본'))
    $bk = Find-Backup (Split-Path -Leaf $f) $otherOut[$f]
    if (-not $bk) { throw "$f — 거꾸로 고친 것과 같은 원본 사본이 없다(모든 파일 손대지 않음)" }
    $otherNote[$f] = "되돌림 — 사본과 바이트 같음 $bk"
  }
}
if ($wndOut) { [IO.File]::WriteAllBytes($wndPath, $wndOut); Write-Host "  noxworld.wnd $wndNote" }
if ($csfOut) { [IO.File]::WriteAllBytes($csfPath, $csfOut); Write-Host "  nox.csf $csfNote" }
if ($filOut) { [IO.File]::WriteAllBytes($filterPath, $filOut); Write-Host "  filter.wnd $filNote" }
foreach ($f in $otherOut.Keys) { [IO.File]::WriteAllBytes((Join-Path $GameDir $f), $otherOut[$f]); Write-Host "  $f $($otherNote[$f])" }
foreach ($nf in $newFiles) {   # ★ 11-04 플랜1 — 위 판정에서 모름이면 여기까지 안 옴
  $p = Join-Path $GameDir $nf.file
  if ($Mode -eq 'Apply') { [IO.File]::WriteAllBytes($p, $latin1.GetBytes($nf.text)) }
  elseif (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
}
$wndB2 = [IO.File]::ReadAllBytes($wndPath)
$ws2 = Wnd-State $wndB2
$wl2 = Lines-State ($latin1.GetString($wndB2)) $wndLineEdits
$cs2 = Csf-State ([IO.File]::ReadAllBytes($csfPath))
$fs2 = Filter-State ([IO.File]::ReadAllBytes($filterPath))
$others2 = @{}
foreach ($f in $otherFiles) { $others2[$f] = @{ st = (Lines-State ($latin1.GetString([IO.File]::ReadAllBytes((Join-Path $GameDir $f)))) @($lineEdits | Where-Object { $_.file -eq $f })) } }
Write-Host ("  지금 noxworld.wnd = {0} · 그 안 11-04 줄 = {1} · nox.csf = {2} · filter.wnd = {3} · {4}" -f $ws2.state, $wl2.state, $cs2.state, $fs2.state, (Say-Others $others2))
$end = @($ws2.state, $wl2.state, $cs2.state, $fs2.state) + @($otherFiles | ForEach-Object { $others2[$_].st.state }) + @($newFiles | ForEach-Object { New-State $_ })
if (@($end | Where-Object { $_ -ne $want }).Count -ne 0) { throw "끝 상태가 $want 이 아님" }
Write-Host "[OK] $Mode"
