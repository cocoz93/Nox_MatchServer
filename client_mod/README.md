# client_mod — 플레이어 PC 의 정품 게임 폴더에 들어가는 것만 둔다

| 폴더 | 하는 일 |
|---|---|
| `wsock32_proxy` | 클라 DLL — 정품 실행 파일은 그대로 두고 게임 폴더에 이 `wsock32.dll` 하나를 더한다. 입장 토큰을 싣고, 정품이 못 하는 방 만들기를 메모리 몇 칸으로 메운다(까닭은 `wsock32_proxy.cpp` 머리 「왜 이 DLL 이 있나」). 빌드 `build.bat` · 시험 `test.bat` |
| `mo_client` | 플레이어 실행 스크립트 — `1_SETUP.bat`(한 번) · `2_PLAY.bat`(할 때마다) · `README_MO.txt`. `tools\pack_client.ps1` 이 꾸러미에 넣는다 |

내 PC 에서만 쓰는 시험 도구는 `tools\` 에 둔다 — 정품을 건드려도 꾸러미에 안 들어가면 여기가 아니다(예: 한 PC 에 클라 둘을 띄울 때 입력을 가르는 `tools\dinput_proxy`).