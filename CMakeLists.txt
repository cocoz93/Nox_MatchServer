cmake_minimum_required(VERSION 3.20)
project(nox_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()

add_library(nox_proto STATIC
  server/src/Opcode.cpp
)
target_include_directories(nox_proto PUBLIC server/include)

add_executable(nox_server server/src/main.cpp)
target_link_libraries(nox_server PRIVATE nox_proto)

add_executable(proto_test server/tests/proto_test.cpp)
target_link_libraries(proto_test PRIVATE nox_proto)

enable_testing()
add_test(NAME proto_test COMMAND proto_test)
add_test(NAME server_selftest COMMAND nox_server --selftest)
