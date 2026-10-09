cmake_minimum_required(VERSION 3.20)
project(nox_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()

add_library(nox_proto STATIC
  server/src/Opcode.cpp
  server/src/MsgLen.cpp
  server/src/SpellNames.cpp
  server/src/core/SoundNames.cpp
  server/src/ThingFlags.cpp
  server/src/Server.cpp
  server/src/UdpServer.cpp
  server/src/SyncHost.cpp
  server/src/DumpSniff.cpp
  server/src/UpdateStream.cpp
  server/src/core/GameBalance.cpp
  server/src/core/NoxCrypt.cpp
  server/src/core/ThingDb.cpp
  server/src/core/MapFile.cpp
  server/src/core/JoinBundle.cpp
  server/src/core/SnapshotTail.cpp
  server/src/core/Gamedata.cpp
)
target_include_directories(nox_proto PUBLIC server/include)

add_executable(nox_server server/src/main.cpp server/src/WorldBuild.cpp)
target_link_libraries(nox_server PRIVATE nox_proto)

add_executable(nox_proxy server/src/proxy.cpp)
target_link_libraries(nox_proxy PRIVATE nox_proto)

# 대조 장치 — 덤프를 재생해 서버를 바이트로 채점한다. 소켓을 쓰지 않는다.
add_executable(nox_replay server/src/replay.cpp)
target_link_libraries(nox_replay PRIVATE nox_proto)

# 상태 갱신 판독기 — 내 파서가 덤프를 끝까지 읽는지 성공률로 잰다.
add_executable(nox_opcount server/src/opcount.cpp)
target_link_libraries(nox_opcount PRIVATE nox_proto)

add_executable(nox_verify server/src/verify_update.cpp)
target_link_libraries(nox_verify PRIVATE nox_proto)

# 게임 데이터 파일 속을 보는 도구 — 어떤 수치가 파일에 있나(P6 기획데이터 전수검사)
add_executable(nox_datadump server/src/datadump.cpp)
target_link_libraries(nox_datadump PRIVATE nox_proto)

# 로비 서버(MO 2단계, Docs/LOBBY_DESIGN.md) — 게임 규약 라이브러리(nox_proto)에 기대지 않는다.
add_library(nox_lobby_core STATIC
  server/src/lobby/WolGameInfo.cpp
  server/src/lobby/Lobby.cpp
)
target_include_directories(nox_lobby_core PUBLIC server/include)

add_executable(nox_lobby server/src/lobby_main.cpp)
target_link_libraries(nox_lobby PRIVATE nox_lobby_core)

add_executable(lobby_test server/tests/lobby_test.cpp)
target_link_libraries(lobby_test PRIVATE nox_lobby_core)

add_executable(proto_test server/tests/proto_test.cpp)
target_link_libraries(proto_test PRIVATE nox_proto)
target_compile_definitions(proto_test PRIVATE NOX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME proto_test COMMAND proto_test)
add_test(NAME server_selftest COMMAND nox_server --selftest)
add_test(NAME lobby_unit COMMAND lobby_test)

# 로비 녹음 대조 — 1단계 녹음(정품 클라 ↔ XWIS)이 정답지. 남의 이름·IP 가 들어 있어
# `_evidence/` 에만 두므로(git 제외) 있을 때만 건다.
set(NOX_WOL_RECORDING "${CMAKE_SOURCE_DIR}/_evidence/_wol_record_20261006/relay_log_20261006_233255.txt")
if (EXISTS "${NOX_WOL_RECORDING}")
  add_test(NAME lobby_gameinfo COMMAND lobby_test --gameinfo "${NOX_WOL_RECORDING}")
  add_test(NAME lobby_replay COMMAND lobby_test --replay "${NOX_WOL_RECORDING}")
else()
  message(STATUS "로비 녹음이 없어 로비 녹음 대조는 걸지 않는다: ${NOX_WOL_RECORDING}")
endif()

# 대조 장치 회귀 — 접속 절차가 정품과 바이트까지 같은지 매번 다시 잰다.
# 덤프는 `_evidence/` 에 있고 git 에 안 들어가므로(역분석 산물), 있을 때만 건다.
set(NOX_REPLAY_DUMP "${CMAKE_SOURCE_DIR}/_evidence/20260905_server_bidir.txt")
if (EXISTS "${NOX_REPLAY_DUMP}")
  add_test(NAME replay_connect COMMAND nox_replay "${NOX_REPLAY_DUMP}")
  add_test(NAME verify_update COMMAND nox_verify "${NOX_REPLAY_DUMP}")
else()
  message(STATUS "덤프가 없어 대조 장치 회귀는 걸지 않는다: ${NOX_REPLAY_DUMP}")
endif()

# 소켓 왕복 — 대조 장치는 소켓을 안 타므로 bind·recvfrom·sendto 는 여기서만 확인된다.
# ⚠ 인자를 콤마로 넘기지 말 것. PowerShell 이 배열로 오파싱한다.
find_program(NOX_POWERSHELL powershell)
if (NOX_POWERSHELL AND WIN32)
  add_test(NAME socket_smoke
           COMMAND ${NOX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                   -File ${CMAKE_SOURCE_DIR}/tools/smoke.ps1)
  # 같은 왕복을 독립 모드(맵을 직접 읽는다)로 — 사람 확인과 같은 실행 조건이다.
  # 맵은 게임 설치본에 있고 git 에 안 들어가므로 있을 때만 건다. 포트는 위와 겹치지 않게.
  set(NOX_SMOKE_MAP "C:/Program Files/GOG Galaxy/Games/Nox/maps/estate/Estate.map" CACHE FILEPATH "독립 모드 소켓 왕복에 쓸 맵")
  # 로비 소켓 왕복 — 로비 로직의 꼴은 lobby_replay 가 보고, 여기서는 accept·줄 나누기·보내기·끊기만 본다.
  add_test(NAME lobby_smoke
           COMMAND ${NOX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                   -File ${CMAKE_SOURCE_DIR}/tools/lobby_smoke.ps1 -Exe $<TARGET_FILE:nox_lobby>)
  if (EXISTS "${NOX_SMOKE_MAP}")
    add_test(NAME socket_smoke_standalone
             COMMAND ${NOX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                     -File ${CMAKE_SOURCE_DIR}/tools/smoke.ps1 -Port 18598 -Map "${NOX_SMOKE_MAP}")
  else()
    message(STATUS "맵이 없어 독립 모드 소켓 왕복은 걸지 않는다: ${NOX_SMOKE_MAP}")
  endif()
endif()
