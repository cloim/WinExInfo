cmake_minimum_required(VERSION 3.29.2)

project(WinExInfo LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "WinExInfo requires MSVC")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "WinExInfo requires an x64 toolchain")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_testing()

add_executable(WinExInfoTests
  src/common/utf8.cpp
  src/host/command_line.cpp
  src/probe/report_writer.cpp
  src/probe/target_validator.cpp
  src/probe/uia_probe.cpp
  src/probe/win32_probe.cpp
  tests/command_line_tests.cpp
  tests/contracts_tests.cpp
  tests/target_validation_tests.cpp
  tests/test_framework.cpp
  tests/test_main.cpp
  tests/uia_contract_tests.cpp
)

target_include_directories(WinExInfoTests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)

target_compile_definitions(WinExInfoTests PRIVATE
  NOMINMAX
  UNICODE
  WIN32_LEAN_AND_MEAN
  _UNICODE
)

target_compile_options(WinExInfoTests PRIVATE
  /W4
  /WX
  /permissive-
  /utf-8
  /guard:cf
)

target_link_options(WinExInfoTests PRIVATE /guard:cf)

target_link_libraries(WinExInfoTests PRIVATE
  crypt32
  version
  wintrust
)

set_target_properties(WinExInfoTests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME WinExInfo.Unit COMMAND WinExInfoTests --unit)

add_executable(WinExInfoHost
  src/common/utf8.cpp
  src/host/command_line.cpp
  src/host/main.cpp
  src/probe/probe_runner.cpp
  src/probe/report_writer.cpp
  src/probe/target_validator.cpp
  src/probe/uia_probe.cpp
  src/probe/win32_probe.cpp
)

target_include_directories(WinExInfoHost PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(WinExInfoHost PRIVATE
  NOMINMAX
  UNICODE
  WIN32_LEAN_AND_MEAN
  _UNICODE
)

target_compile_options(WinExInfoHost PRIVATE
  /W4
  /WX
  /permissive-
  /utf-8
  /guard:cf
)

target_link_options(WinExInfoHost PRIVATE /guard:cf)

target_link_libraries(WinExInfoHost PRIVATE
  crypt32
  version
  wintrust
)

set_target_properties(WinExInfoHost PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
