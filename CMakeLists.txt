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
  server/src/ThingFlags.cpp
  server/src/Server.cpp
  server/src/DumpSniff.cpp
  server/src/UpdateStream.cpp
)
target_include_directories(nox_proto PUBLIC server/include)

add_executable(nox_server server/src/main.cpp)
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

add_executable(proto_test server/tests/proto_test.cpp)
target_link_libraries(proto_test PRIVATE nox_proto)

enable_testing()
add_test(NAME proto_test COMMAND proto_test)
add_test(NAME server_selftest COMMAND nox_server --selftest)

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
endif()
