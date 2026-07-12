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
  src/host/explorer_controller.cpp
  src/hook/dll_main.cpp
  src/hook/explorer_layout.cpp
  src/hook/hook_entry.cpp
  src/hook/runtime.cpp
  src/hook/status_pane.cpp
  src/injection/hook_platform.cpp
  src/injection/thread_hook_injector.cpp
  src/ipc/named_pipe.cpp
  src/ipc/protocol.cpp
  src/probe/event_observer.cpp
  src/probe/observer_runtime.cpp
  src/probe/observer_runtime_contract.cpp
  src/probe/observer_production_runtime.cpp
  src/probe/probe_runner.cpp
  src/probe/report_writer.cpp
  src/probe/shell_probe.cpp
  src/probe/tab_identity.cpp
  src/probe/target_validator.cpp
  src/probe/uia_probe.cpp
  src/probe/uia_observer_worker.cpp
  src/probe/win32_probe.cpp
  tests/active_view_tests.cpp
  tests/com_apartment_tests.cpp
  tests/command_line_tests.cpp
  tests/contracts_tests.cpp
  tests/event_filter_tests.cpp
  tests/explorer_controller_tests.cpp
  tests/explorer_layout_tests.cpp
  tests/host_manifest_tests.cpp
  tests/hook_injector_tests.cpp
  tests/hook_controller_mode.cpp
  tests/hook_runtime_tests.cpp
  tests/hook_target_mode.cpp
  tests/hook_test_modes.cpp
  tests/ipc_protocol_tests.cpp
  tests/ipc_production_tests.cpp
  tests/observer_runtime_tests.cpp
  tests/observer_runtime_contract_tests.cpp
  tests/observer_shell_reconcile_tests.cpp
  tests/status_pane_placement_tests.cpp
  tests/target_validation_tests.cpp
  tests/tab_identity_tests.cpp
  tests/test_framework.cpp
  tests/test_main.cpp
  tests/uia_contract_tests.cpp
  tests/uia_observer_worker_tests.cpp
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
  advapi32
  comctl32
  crypt32
  ole32
  shell32
  uuid
  version
  wintrust
)

set_target_properties(WinExInfoTests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME WinExInfo.Unit COMMAND WinExInfoTests --unit)
add_test(
  NAME WinExInfo.CommandLineGateC
  COMMAND WinExInfoTests --unit --filter command_line.gate_c
)

add_library(WinExInfoHook SHARED
  src/common/utf8.cpp
  src/hook/dll_main.cpp
  src/hook/explorer_layout.cpp
  src/hook/hook_entry.cpp
  src/hook/hook_export.cpp
  src/hook/runtime.cpp
  src/hook/status_pane.cpp
  src/injection/thread_hook_injector.cpp
  src/ipc/named_pipe.cpp
  src/ipc/protocol.cpp
  src/hook/WinExInfoHook.def
)

target_include_directories(WinExInfoHook PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(WinExInfoHook PRIVATE
  NOMINMAX
  UNICODE
  WIN32_LEAN_AND_MEAN
  _UNICODE
)

target_compile_options(WinExInfoHook PRIVATE
  /W4
  /WX
  /permissive-
  /utf-8
  /guard:cf
)

target_link_options(WinExInfoHook PRIVATE /guard:cf)

target_link_libraries(WinExInfoHook PRIVATE
  advapi32
  comctl32
  ole32
  uuid
)

set_target_properties(WinExInfoHook PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(WinExInfoHost
  src/common/utf8.cpp
  src/host/command_line.cpp
  src/host/explorer_controller.cpp
  src/host/main.cpp
  src/host/WinExInfoHost.manifest
  src/injection/hook_platform.cpp
  src/injection/thread_hook_injector.cpp
  src/ipc/named_pipe.cpp
  src/ipc/protocol.cpp
  src/probe/event_observer.cpp
  src/probe/observer_runtime.cpp
  src/probe/observer_runtime_contract.cpp
  src/probe/observer_production_runtime.cpp
  src/probe/probe_runner.cpp
  src/probe/report_writer.cpp
  src/probe/shell_probe.cpp
  src/probe/tab_identity.cpp
  src/probe/target_validator.cpp
  src/probe/uia_probe.cpp
  src/probe/uia_observer_worker.cpp
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
  advapi32
  crypt32
  ole32
  shell32
  uuid
  version
  wintrust
)

set_target_properties(WinExInfoHost PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_dependencies(WinExInfoTests WinExInfoHost WinExInfoHook)
