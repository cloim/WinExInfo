# WinExInfo Feasibility Gates A-B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 실제 파일 탐색기를 변경하지 않는 읽기 전용 계약 probe(Gate A)와 전용 test process에서만 동작하는 hook load/unload harness(Gate B)를 구축해, WinExInfo의 핵심 기술 가정을 증명한다.

**Architecture:** `WinExInfoHost.exe`는 Explorer HWND·UIA·Shell COM 계약을 읽고 deterministic report를 출력한다. `WinExInfoTests.exe`는 단위 테스트와 별도 hook target/controller 역할을 함께 제공하며, `WinExInfoHook.dll`은 Gate B test process에서만 load/unload 수명주기를 검증한다. 실제 `explorer.exe` 주입, Git 상태 계산, watcher, 설치, 자동 실행은 이 계획의 범위가 아니다.

**Tech Stack:** C++20, Visual Studio Build Tools 17.14.36109.1 with the MSVC x64 toolset, CMake 3.29.2, Ninja 1.12.0, Windows SDK 10.0.26100.0, Win32, WRL COM, UI Automation, Shell COM, CTest, PowerShell 7.6.3.

**Source Spec:** `docs/superpowers/specs/2026-07-10-winexinfo-git-status-design.md`

## Global Constraints

- 대상 OS는 Windows 11 Pro 25H2 `10.0.26200.8655`, AMD64 하나다.
- 대상 `explorer.exe`, `ExplorerFrame.dll`, `shell32.dll`의
  `VS_FIXEDFILEINFO.dwFileVersionMS/LS` 버전은 모두 `10.0.26100.8655`다.
- Host application manifest는 Windows 10/11
  `supportedOS={8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}`를 내장한다.
- Release runtime은 static CRT와 Control Flow Guard를 사용한다.
- CLR과 제3자 runtime 또는 package dependency를 추가하지 않는다.
- lookup은 설계 문서의 exact class, AutomationId, interface, event ID만 사용한다.
- legacy class, 주소창 문자열, 창 제목, UIA Name을 대체 lookup으로 사용하지 않는다.
- 네트워크 호출, package download, `git fetch`, registry write, scheduled task, service, 자동 실행 등록을 수행하지 않는다.
- Gate A는 Explorer를 읽고 기존 탭·폴더를 전환해 event를 관찰할 수 있지만 DLL을 load하지 않는다.
- Gate B는 `WinExInfoTests.exe`가 만든 test process에만 hook DLL을 load한다.
- production Host에 arbitrary PID를 허용하는 test flag를 추가하지 않는다.
- Gate C 실제 Explorer UI 배치는 이 계획 종료 후 형님의 별도 승인 전까지 수행하지 않는다.
- 모든 실패는 exact `ErrorCode`로 반환하며 다른 key, class, event source, protocol version으로 보정하지 않는다.
- 함수 또는 상수는 세 곳 이상에서 실제로 참조될 때만 추출한다.
- planning document에는 구현 소스 코드를 넣지 않는 저장소 규칙을 따른다.

---

## Scope Boundary and Stop Conditions

이 계획은 다음 두 결과만 만든다.

1. **Gate A PASS/FAIL:** 현재 빌드에서 exact target identity/preflight, HWND/UIA selectors, active Shell view mapping, navigation/tab events가 재현되는지 증명한다.
2. **Gate B PASS/FAIL:** test target에서 hook load, attach result, successful unhook, child HWND cleanup, DLL unload를 100회 반복해도 resource가 증가하지 않는지 증명한다.

다음 조건 중 하나가 발생하면 해당 evidence를 남기고 이후 task를 시작하지 않는다.

- OS, Explorer file identity/signature/user/session/integrity/architecture/version/mitigation preflight 중 하나라도 불일치함
- controlled single-tab 또는 multi-tab Explorer 상태 중 하나라도 exact identity/selector evidence를 만들지 못함
- 존재하는 controlled top-level HWND의 `NavigateComplete2`, tab selection, 또는 tab structure remap 결과가 0개 또는 2개 이상으로 결정됨
- `WindowRegistered`, `WindowRevoked`, `NavigateComplete2`, exact TabView selection 또는 structure event 중 하나라도 현재 빌드에서 발생하지 않음
- UIA selector가 설계 문서 값과 다름
- Gate B에서 `UnhookWindowsHookEx` 성공 후 DLL module이 남음
- unhook 실패 fault에서 HookReleased event가 signal되거나 DLL unload가 호출됨
- 반복 실행 후 handle 또는 thread 수가 증가함

실패 시 external overlay, DirectUI patch, 주소창 parsing, remote-thread injection으로 전환하지 않는다.

---

## File Structure

### Build and project files

| Path | Responsibility |
|---|---|
| `.gitignore` | `out/`, `.vs/`, `CMakeUserPresets.json`, `docs/superpowers/evidence/raw/`만 제외 |
| `CMakeLists.txt` | C++20, x64, static CRT, CFG, warning policy, targets, CTest wiring |
| `CMakePresets.json` | exact Ninja Debug/Release configure와 build presets |
| `scripts/build.ps1` | exact VS Build Tools environment 진입, configure, build, optional CTest 실행 |
| `scripts/run-gate-b.ps1` | test target 시작, READY 검증, exact controller PID CLI 실행, normal/fault evidence orchestration |

### Shared contracts

| Path | Responsibility |
|---|---|
| `src/common/contracts.h` | target versions, class names, AutomationIds, message name, magic, timeout constants |
| `src/common/status.h` | Gate A/B `ErrorCode`, `Status`, exact error-name conversion |
| `src/common/win32_handle.h` | Host, probe, injection, hook에서 공통 사용하는 move-only HANDLE/HHOOK RAII |
| `src/common/utf8.h` / `src/common/utf8.cpp` | report와 test output에서 공통 사용하는 strict UTF-16↔UTF-8 변환 |

### Host and probe

| Path | Responsibility |
|---|---|
| `src/host/command_line.h` / `src/host/command_line.cpp` | exact `--probe snapshot`와 `--probe observe --duration-ms N` parsing |
| `src/host/main.cpp` | COM 초기화, command dispatch, documented exit code 반환 |
| `src/probe/probe_types.h` | HWND, UIA, Shell entry, event snapshot value types |
| `src/probe/target_validator.h` / `src/probe/target_validator.cpp` | exact OS 및 Explorer identity/signature/token/version/mitigation read-only preflight |
| `src/probe/win32_probe.h` / `src/probe/win32_probe.cpp` | `CabinetWClass` enumeration과 exact child HWND tree capture |
| `src/probe/uia_probe.h` / `src/probe/uia_probe.cpp` | StatusBar, Group, TabView, TabListView exact selector capture/validation |
| `src/probe/shell_probe.h` / `src/probe/shell_probe.cpp` | `IShellWindows`→`IServiceProvider`→`IShellBrowser`→active `IShellView` mapping |
| `src/probe/event_observer.h` / `src/probe/event_observer.cpp` | Shell registration, NavigateComplete2, UIA selection/structure event observation |
| `src/probe/report_writer.h` / `src/probe/report_writer.cpp` | deterministic UTF-8 key/value report 출력 |
| `src/probe/probe_runner.h` / `src/probe/probe_runner.cpp` | snapshot/observe orchestration과 Gate A result 계산 |

### IPC and hook lifecycle

| Path | Responsibility |
|---|---|
| `src/ipc/protocol.h` / `src/ipc/protocol.cpp` | `WXI1` frame header와 attach/detach payload validation |
| `src/ipc/named_pipe.h` / `src/ipc/named_pipe.cpp` | current-user-only, remote-rejected named pipe server/client |
| `src/injection/hook_platform.h` / `src/injection/hook_platform.cpp` | production Win32 hook/event/message/module operations |
| `src/injection/thread_hook_injector.h` / `src/injection/thread_hook_injector.cpp` | attach ID, HookReleased event, trigger, attach result, unhook state machine |
| `src/hook/dll_main.cpp` | process attach에서 module handle만 capture; static CRT thread notifications를 유지 |
| `src/hook/WinExInfoHook.def` | exact undecorated x64 hook export `WinExInfoCallWndProc` |
| `src/hook/hook_entry.cpp` | exported `WH_CALLWNDPROC` callback와 exact attach message validation |
| `src/hook/runtime.h` / `src/hook/runtime.cpp` | Starting→Running→Stopping→Stopped state와 cleanup/unload ordering |
| `src/hook/status_pane.h` / `src/hook/status_pane.cpp` | test target 안에서만 생성되는 `WinExInfo.StatusPane` child HWND |

### Tests

| Path | Responsibility |
|---|---|
| `tests/test_framework.h` / `tests/test_framework.cpp` | dependency-free test registry, assertions, prefix filter, summary |
| `tests/test_main.cpp` | `--unit`, test-only `--hook-target`, `--hook-controller <target PID>` exact mode dispatch |
| `tests/contracts_tests.cpp` | target constants, errors, UTF conversion, RAII contracts |
| `tests/command_line_tests.cpp` | Host exact CLI and invalid argument rejection |
| `tests/uia_contract_tests.cpp` | exact selectors, duplicates, missing/alternate values rejection |
| `tests/active_view_tests.cpp` | one/zero/multiple active Shell view candidate rules |
| `tests/event_filter_tests.cpp` | exact DWebBrowserEvents2 and UIA sender filtering |
| `tests/ipc_protocol_tests.cpp` | frame, size, version, payload, empty-field rules |
| `tests/hook_injector_tests.cpp` | set/send/unhook/timeout/failure state machine with test seam |
| `tests/hook_runtime_tests.cpp` | attach/detach ordering and unload preconditions |
| `tests/hook_target_mode.cpp` | separate test window process and ready handshake |
| `tests/hook_controller_mode.cpp` | test target injection, detach, module disappearance, 100-cycle orchestration |

### Evidence

| Path | Responsibility |
|---|---|
| `docs/superpowers/evidence/2026-07-10-winexinfo-gate-a.md` | exact machine snapshot, commands, event observations, PASS/FAIL |
| `docs/superpowers/evidence/2026-07-10-winexinfo-gate-b.md` | load/unload cycles, fault tests, handle/thread deltas, PASS/FAIL |

---

## Build and Test Command Contract

All tasks use these exact commands from repository root.

Configure and build Debug:

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug
```

Build one target:

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
```

Run build and all CTest tests:

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Run one unit-test prefix after a successful build:

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter <exact-prefix>
```

Release verification:

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Test
```

`scripts/build.ps1` must use only `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`, require `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, require installation version `17.14.36109.1` and Windows SDK directory `10.0.26100.0`, enter that VS 2022 Build Tools developer environment, and invoke the named Ninja preset. Missing or version-mismatched tools fail with an exact message and nonzero exit; the script does not install or search alternative tools.

The exact build-script failures are `BUILD_TOOL_NOT_FOUND: vswhere`, `BUILD_TOOL_NOT_FOUND: VS2022 VC x64`, `BUILD_TOOL_VERSION_MISMATCH: VS2022 Build Tools 17.14.36109.1`, `BUILD_TOOL_NOT_FOUND: Windows SDK 10.0.26100.0`, `BUILD_TOOL_NOT_FOUND: cmake 3.29.2`, and `BUILD_TOOL_NOT_FOUND: ninja 1.12.0`. A different installed version is a contract failure and is not selected as an alternative.

---

### Task 1: Native Build and Deterministic Test Foundation

**Files:**
- Create: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `scripts/build.ps1`
- Create: `src/common/contracts.h`
- Create: `src/common/status.h`
- Create: `src/common/win32_handle.h`
- Create: `src/common/utf8.h`
- Create: `src/common/utf8.cpp`
- Create: `tests/test_framework.h`
- Create: `tests/test_framework.cpp`
- Create: `tests/test_main.cpp`
- Create: `tests/contracts_tests.cpp`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| CMake target | `WinExInfoTests` executable in `out/build/<preset>/bin` |
| Test CLI | `WinExInfoTests.exe --unit [--filter <prefix>]` |
| Test output | one `[PASS]`/`[FAIL]` line per case; final `[SUMMARY] passed=N failed=M` |
| Test exit | `0` when failed=0; `1` when any case fails; `2` for invalid test CLI |
| `ErrorCode` names | `OK`, `INVALID_ARGUMENT`, `UNSUPPORTED_OS_BUILD`, `UNSUPPORTED_EXPLORER_BUILD`, `TARGET_VALIDATION_FAILED`, `TARGET_MITIGATION_BLOCKED`, `HOOK_INSTALL_FAILED`, `HOOK_TRIGGER_FAILED`, `HOOK_RELEASE_FAILED`, `DLL_INITIALIZATION_FAILED`, `WINDOW_ATTACH_FAILED`, `DLL_UNLOAD_TIMEOUT`, `EXPLORER_UI_CONTRACT_MISMATCH`, `ACTIVE_VIEW_CONTRACT_MISMATCH`, `IPC_PROTOCOL_ERROR`, `PIPE_DISCONNECTED`; no other name in Gate A/B |
| `Status` | fields `ErrorCode code`, `HRESULT hresult`, `DWORD win32`; `ok()` is true only when the exact serialized error name is `OK` |
| Handle wrapper | move-only; `get`, `release`, `reset`, boolean conversion; copy disabled |
| UTF conversion | strict conversion; invalid input returns `InvalidArgument`; no replacement characters |

- [ ] **Step 1: Add the build/test files and a failing contracts test**

  Add tests named `contracts.target_versions`, `contracts.error_names`, `contracts.handle_move`, `contracts.utf8_roundtrip`, and `contracts.utf8_rejects_invalid`. The target-version test must assert all exact values from Global Constraints. Leave the production contract definitions absent so the first build fails at compile time.

- [ ] **Step 2: Run the build and verify the red state**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  ```

  Expected: nonzero exit with a missing `contracts.h`, `status.h`, handle, or UTF symbol; no test binary produced.

- [ ] **Step 3: Implement the exact shared contracts and build policy**

  Configure C++20, Unicode, `/W4`, `/WX`, `/permissive-`, `/utf-8`, static CRT, `/guard:cf`, `WIN32_LEAN_AND_MEAN`, and `NOMINMAX`. Implement only the interfaces in this task. Constants referenced from three or more modules belong in `contracts.h`; one-use values remain local to their owning file.

- [ ] **Step 4: Run the contracts tests and verify green**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  ```

  Expected: CMake configure and Ninja build succeed; CTest reports `100% tests passed`; `contracts.*` reports five PASS lines and zero FAIL lines.

- [ ] **Step 5: Commit the foundation**

  ```powershell
  git add .gitignore CMakeLists.txt CMakePresets.json scripts src/common tests
  git commit -m "build: add native test foundation"
  ```

---

### Task 2: Exact Host Command and Report Contracts

**Files:**
- Create: `src/host/command_line.h`
- Create: `src/host/command_line.cpp`
- Create: `src/probe/probe_types.h`
- Create: `src/probe/report_writer.h`
- Create: `src/probe/report_writer.cpp`
- Create: `tests/command_line_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_main.cpp`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| `HostCommand` | `ProbeSnapshot`, `ProbeObserve` only |
| `ParsedCommand` | command plus `uint32 duration_ms`; snapshot duration is 0 |
| Parser accepted input | `--probe snapshot`; `--probe observe --duration-ms <1000..60000>` |
| Parser rejection | missing value, extra token, duplicate flag, non-decimal, sign, whitespace, overflow, out-of-range duration |
| Host exit codes | `0=pass`, `1=contract/test failure`, `2=invalid CLI`, `3=Win32/COM failure` |
| Report encoding | UTF-8 without BOM, LF line endings, one `key=value` per line |
| Required report header | `probe_version=1`, `mode=snapshot|observe`, `result=pass|fail`, final `error_code=<exact name>` |

- [ ] **Step 1: Write failing command-line and report tests**

  Add `command_line.snapshot_exact`, `command_line.observe_exact`, `command_line.rejects_unknown`, `command_line.rejects_duplicate`, `command_line.rejects_duration_bounds`, `report.escapes_control_characters`, and `report.orders_keys`. Require lexicographic ordering inside each report section and strict percent encoding for CR, LF, `=`, and `%` in values.

- [ ] **Step 2: Run the targeted tests and verify failure**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter command_line
  ```

  Expected: compile/link failure for missing parser/report interfaces, or targeted test exit 1 with named FAIL cases.

- [ ] **Step 3: Implement only the exact parser, probe value types, and report writer**

  Keep CLI token matching ordinal and case-sensitive. Do not accept `/probe`, `--snapshot`, `--duration`, environment variables, defaults for missing duration, or legacy aliases. The report writer must never print a pointer value through locale-dependent formatting.

- [ ] **Step 4: Run command-line and report tests**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  ```

  Expected: all `command_line.*`, `report.*`, and earlier tests pass.

- [ ] **Step 5: Commit the CLI/report contract**

  ```powershell
  git add CMakeLists.txt src/host src/probe tests
  git commit -m "feat: add exact probe command contract"
  ```

---

### Task 3: Win32 and UI Automation Static Contract Probe

**Files:**
- Create: `src/probe/win32_probe.h`
- Create: `src/probe/win32_probe.cpp`
- Create: `src/probe/target_validator.h`
- Create: `src/probe/target_validator.cpp`
- Create: `src/probe/uia_probe.h`
- Create: `src/probe/uia_probe.cpp`
- Create: `src/probe/probe_runner.h`
- Create: `src/probe/probe_runner.cpp`
- Create: `src/host/main.cpp`
- Create: `src/host/WinExInfoHost.manifest`
- Create: `tests/host_manifest_tests.cpp`
- Create: `tests/target_validation_tests.cpp`
- Create: `tests/uia_contract_tests.cpp`
- Create on live Gate A failure: `docs/superpowers/evidence/2026-07-10-winexinfo-gate-a.md`
- Modify: `CMakeLists.txt`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| OS version | `RtlGetVersion` major/minor/build plus exact `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` `UBR`; only `10.0.26200.8655` passes |
| Target file identity | target image and `GetWindowsDirectoryW` `explorer.exe` must have equal volume serial and `FILE_ID_INFO`; path strings never establish identity |
| Signature | `CryptCATAdminCalcHashFromFileHandle2`→`CryptCATAdminEnumCatalogFromHash`→`WinVerifyTrust` with `WINTRUST_CATALOG_INFO`, `WTD_CACHE_ONLY_URL_RETRIEVAL`, and `WTD_REVOKE_NONE`; signer subject equals `CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US`; no embedded-signature path |
| Token/session | target user SID and session equal Host; target integrity level equals Host |
| Architecture | native AMD64 only; WOW64 or another native machine fails |
| File versions | `VS_FIXEDFILEINFO.dwFileVersionMS/LS` only: `explorer.exe=10.0.26100.8655`, `ExplorerFrame.dll=10.0.26100.8655`, `shell32.dll=10.0.26100.8655`; Host embeds Windows 10/11 `supportedOS={8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}` so version.dll does not return an older compatibility context |
| Mitigations | `DisableExtensionPoints=OFF`, `MicrosoftSignedOnly=OFF`, `CFG=ON`, `StrictCFG=OFF` |
| Preflight errors | OS mismatch=`UNSUPPORTED_OS_BUILD`; file-version mismatch=`UNSUPPORTED_EXPLORER_BUILD`; any mitigation mismatch=`TARGET_MITIGATION_BLOCKED`; every other target mismatch=`TARGET_VALIDATION_FAILED` |
| `EnumerateExplorerWindows` | visible top-level `CabinetWClass` windows only; records HWND, PID, UI thread ID |
| HWND tree | exact parent/child class snapshot including `ShellTabWindowClass`, `DUIViewWndClassName`, `DirectUIHWND`, `msctls_statusbar32`; capture the top-level direct-child z-order before and after the tree and require the two sequences to be identical |
| Active tab HWND | scan the documented direct-child z-order from `GetTopWindow` through `GetWindow(..., GW_HWNDNEXT)` and select the first exact `ShellTabWindowClass`; multiple ShellTab siblings are valid, but zero candidates or a hidden first exact candidate returns `EXPLORER_UI_CONTRACT_MISMATCH` without selecting a later candidate |
| Active view parent HWND | under the selected ShellTab, all `DUIViewWndClassName` descendants are counted regardless of visibility; exactly one must exist and be visible, otherwise `EXPLORER_UI_CONTRACT_MISMATCH` |
| UIA search scope | `ElementFromHandle(active DUIViewWndClassName)` `TreeScope_Subtree` for StatusBar; top-level Explorer element `TreeScope_Subtree` for TabView; selected StatusBar `TreeScope_Children` for Groups; selected TabView `TreeScope_Children` for TabListView; no inactive-tab or desktop-global search |
| StatusBar selector | FrameworkId `DirectUI`, ControlType StatusBar, AutomationId/ClassName `StatusBarModuleInner`, NativeWindowHandle 0 |
| Left group selector | direct child Group, AutomationId `System.StatusBarViewItemCount`, NativeWindowHandle 0 |
| Right group selector | direct child Group, AutomationId `ViewButtonsGroup`, NativeWindowHandle 0 |
| Tab selector | FrameworkId `XAML`, ControlType Tab, AutomationId `TabView`, ClassName `Microsoft.UI.Xaml.Controls.TabView` |
| Tab list selector | direct child, FrameworkId `XAML`, ControlType List, AutomationId `TabListView`, ClassName `ListView` |
| Cardinality | every required selector must resolve exactly once |
| Snapshot mode | no DLL load, no window message, no registry/file write outside normal process output |

- [ ] **Step 1: Write failing target-validation and exact-selector tests**

  Add validation cases for the exact inspected target; missing exact UBR value; alternate version source; path-equal/file-ID-different; signer mismatch; signature cache miss; different SID/session/integrity; WOW64; each file-version mismatch; each mitigation mismatch; and a cache-only verification policy that cannot request network data. Verify the built Host manifest resource contains exactly one Windows 10/11 `supportedOS` GUID. Add UI cases for an empty direct-child z-order; multiple visible `ShellTabWindowClass` siblings with the first z-order candidate selected; a hidden first exact candidate followed by a visible candidate; changed or cyclic z-order capture; zero/multiple total `DUIViewWndClassName` descendants; one visible plus one hidden DUIView duplicate; one exact but hidden DUIView; missing UIA element; duplicate UIA element; alternate AutomationId; alternate FrameworkId; wrong parent scope; nonzero StatusBar or Group native HWND; and localized Name-only matches. Add a Win32 class-tree test that proves the zero-width `msctls_statusbar32` is recorded but never selected.

- [ ] **Step 2: Run the UIA tests and verify red**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter target_validation
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter uia_contract
  ```

  Expected: missing probe symbols or named `uia_contract.*` FAIL cases.

- [ ] **Step 3: Implement target preflight, then static Win32/UIA capture and validation**

  Complete every preflight row before opening UIA state for that PID. Use only `RtlGetVersion`, the exact UBR registry value, `VS_FIXEDFILEINFO.dwFileVersionMS/LS`, opened-file identity, cache-only catalog verification, process tokens/session, `IsWow64Process2`, and exact process mitigation policies; a missing source fails and never triggers an alternate lookup. Embed the Windows 10/11 `supportedOS` GUID in the Host manifest before reading version resources. Capture the top-level direct-child z-order before and after descendant enumeration, reject a changed sequence or a repeated HWND, and retain exact GetTopWindow/GetWindow errors. Run UI Automation in an MTA worker. Use cached FrameworkId, ControlType, ClassName, AutomationId, ProcessId, NativeWindowHandle, BoundingRectangle, and IsOffscreen properties. Convert screen rectangles without changing any target window. Exact cardinality and Win32/UIA transport failures serialize `EXPLORER_UI_CONTRACT_MISMATCH`; transport failures also retain the exact HRESULT or Win32 code and make Host exit 3.

- [ ] **Step 4: Add and build `WinExInfoHost.exe --probe snapshot`**

  `main.cpp` must call `CoInitializeEx` with MTA, parse only Task 2 commands, run snapshot mode, print the deterministic report, remove UIA handlers/COM state, and call `CoUninitialize`. Snapshot output must include window index, HWND, PID, thread ID, class chain, exact selector properties, rectangles, cardinalities, and error code.

- [ ] **Step 5: Run unit tests and a live read-only snapshot**

  Before the first live command, capture a read-only baseline at `docs/superpowers/evidence/raw/2026-07-10-pre-gates.txt`: Explorer module entries named `WinExInfoHook.dll`, the exact `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WinExInfo` value state, service name `WinExInfo`, scheduled task exact name `WinExInfo`, and outbound connections owned by an image named `WinExInfoHost.exe` or `WinExInfoTests.exe`. A pre-existing Explorer module entry is a Gate A FAIL and immediate stop; record it without unloading it. Preserve every other exact absent/present state and do not normalize or remove a pre-existing entry.

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  .\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
  ```

  Expected: unit tests pass; live command exits 0; each probed PID reports every preflight row PASS; at least one `CabinetWClass` record reports exact StatusBar, left group, right group, TabView, and TabListView selectors; no process contains `WinExInfoHook.dll`.

- [ ] **Step 6: Commit the static probe or record an early Gate A failure**

  If Step 5 passes, run:

  ```powershell
  git add CMakeLists.txt src/host src/probe tests
  git commit -m "feat: probe Explorer Win32 and UIA contracts"
  ```

  If the safety baseline, preflight, or live UIA contract fails, write the bounded evidence available so far with `Gate A: FAIL`, stage the same implementation paths plus the evidence file, commit with `test: record Explorer static gate failure`, and stop before Task 4. Do not unload a pre-existing module or add another selector.

---

### Task 4: Active Shell View Mapping

**Files:**
- Create: `src/probe/shell_probe.h`
- Create: `src/probe/shell_probe.cpp`
- Create: `tests/active_view_tests.cpp`
- Modify: `src/probe/probe_types.h`
- Modify: `src/probe/probe_runner.cpp`
- Modify: `src/probe/report_writer.cpp`
- Modify: `CMakeLists.txt`
- Create on live Gate A failure: `docs/superpowers/evidence/2026-07-10-winexinfo-gate-a.md`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| Shell enumeration | `IShellWindows` entries whose `IWebBrowser2::HWND` equals one probed top-level HWND |
| Browser mapping | `IWebBrowser2`→`IServiceProvider`→`QueryService(SID_STopLevelBrowser, IID_IShellBrowser)`; among all entries sharing the top-level HWND, `IShellBrowser::GetWindow` must equal the selected first-z-order `ShellTabWindowClass` for exactly one entry |
| View mapping | `QueryActiveShellView`→`IShellView::GetWindow`; view must be visible descendant of active `ShellTabWindowClass` |
| Active selection | exactly one qualifying view; zero/multiple returns `ACTIVE_VIEW_CONTRACT_MISMATCH` |
| Folder path | `IFolderView::GetFolder`→`IShellFolder`→`SHGetIDListFromObject` absolute PIDL→`SHCreateItemFromIDList` `IShellItem`→`GetAttributes(SFGAO_FILESYSTEM)`; only `S_OK` with the bit set proceeds to a required non-empty `SIGDN_FILESYSPATH`, while `S_FALSE` with the bit unset is unavailable |
| Non-filesystem view | expected hidden state; not a contract error |
| Report privacy | snapshot command may print explicit path because it is user-invoked; runtime log files are not created in Gate A |
| Terminal stage | every window emits exactly one `shell_terminal_stage`; its fixed values are the 19 design-spec values from `not_started` through `complete`, invalid enum values fail with `ACTIVE_VIEW_CONTRACT_MISMATCH` and `E_INVALIDARG` |

- [ ] **Step 1: Write failing active-view selection tests**

  Add exact tests for one browser HWND equal to the selected ShellTab and one visible descendant view; zero or two matching browser HWNDs; same HWND but hidden view; visible view under the wrong `ShellTabWindowClass`; non-filesystem Shell item; PIDL conversion failure; and duplicate Shell entries pointing to the same visible view.

- [ ] **Step 2: Run the targeted tests and verify red**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter active_view
  ```

  Expected: missing mapping symbols or named FAIL cases; earlier suites remain green.

- [ ] **Step 3: Implement Shell COM enumeration and exact selection**

  Do not use `LocationURL`, address bar text, tab Name, window title, or a second service ID. Release every COM interface and PIDL on all paths. Compare HWND relationships with Win32 APIs, not title/path inference. Shell COM transport failures serialize `ACTIVE_VIEW_CONTRACT_MISMATCH`, retain the exact HRESULT, and make Host exit 3.

- [ ] **Step 4: Extend snapshot output and verify current Explorer**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  .\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
  ```

  Expected: each filesystem Explorer window reports `shell_tab_match_count=1`, `active_view_count=1`, a distinct `shell_view`, and its exact active path; non-filesystem views report `filesystem_path_available=false` with one empty `filesystem_path`; no entry uses a fallback field.

- [ ] **Step 5: Commit Shell mapping or record an early Gate A failure**

  If Step 4 passes, run:

  ```powershell
  git add CMakeLists.txt src/probe tests
  git commit -m "feat: resolve active Explorer shell views"
  ```

  If live Shell mapping produces zero/multiple active views for an applicable existing window or a Shell COM transport failure, write the bounded target/UIA/Shell evidence with `Gate A: FAIL`, stage the same implementation paths plus the evidence file, commit with `test: record active-view gate failure`, and stop before Task 5. Do not add a path, title, URL, or alternate service fallback.

---

### Task 5: Event Observation and Gate A Evidence

**Files:**
- Create: `src/probe/event_observer.h`
- Create: `src/probe/event_observer.cpp`
- Create: `tests/event_filter_tests.cpp`
- Create after execution: `docs/superpowers/evidence/2026-07-10-winexinfo-gate-a.md`
- Modify: `src/probe/probe_types.h`
- Modify: `src/probe/probe_runner.cpp`
- Modify: `src/probe/report_writer.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

| Event | Exact source/filter |
|---|---|
| Shell entry lifecycle | `DShellWindowsEvents::WindowRegistered` and `WindowRevoked` only |
| Folder navigation | each mapped `IWebBrowser2` `DIID_DWebBrowserEvents2` connection point; `DISPID_NAVIGATECOMPLETE2` only |
| Tab selection | exact TabListView; `UIA_SelectionItem_ElementSelectedEventId`; `TreeScope_Children` |
| Tab collection | exact TabListView; structure-changed handler; `TreeScope_Subtree` |
| UIA cache | non-null, `AutomationElementMode=Full`, cache `TreeScope_Element`, FrameworkId/ControlType/ClassName/AutomationId/ProcessId/IsOffscreen |
| Selection sender | direct TabListView child with FrameworkId=`XAML`, ControlType=`TabItem`, ClassName=`ListViewItem` |
| Structure sender | TabListView itself or any descendant only; accepted event re-enumerates Shell entries and TabListView direct children |
| Observe command | `--probe observe --duration-ms N`; no default duration and no infinite mode |
| Existing-window action | navigation/selection/structure event immediately hides the logical snapshot and remaps; zero/multiple records `ACTIVE_VIEW_CONTRACT_MISMATCH` and stays hidden until the next allowed event |
| WindowRegistered action | create a pending entry and re-enumerate once; count 0 is transitional until its first allowed navigation/structure event, without polling |
| WindowRevoked action | require a previous mapping for the cookie/top-level HWND, then remove its current HWND/view/path mapping; current count 0 is the expected terminal state |
| Correlation record | monotonic sequence, event kind, source top-level HWND, Shell cookie when present, previous/current active view HWND, active-view count, previous/current filesystem-path availability and value |

- [ ] **Step 1: Write failing event-filter and lifetime tests**

  Cover accepted `WindowRegistered`/`WindowRevoked`, rejected `DShellWindowsEvents` DISPIDs, accepted `DISPID_NAVIGATECOMPLETE2`, rejected `DWebBrowserEvents2` DISPIDs, exact UIA sender ancestry, wrong TreeScope simulation, duplicate registration, handler removal identity, stale Shell entry revocation, and event-after-stop rejection. Assert UIA Name and tab title never influence acceptance.

- [ ] **Step 2: Run the event tests and verify red**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter event_filter
  ```

  Expected: missing observer/filter symbols or named FAIL cases.

- [ ] **Step 3: Implement exact COM/UIA event subscriptions and cleanup**

  Use one MTA observer worker. Advise/unadvise each Shell connection point exactly once. Register/remove UIA handlers with the same element and handler identities. The callback only queues an immutable event record; Shell remapping occurs on the observer worker, never inside the callback. COM/UIA subscription or callback transport failures serialize `ACTIVE_VIEW_CONTRACT_MISMATCH`, retain the exact HRESULT, and make Host exit 3.

- [ ] **Step 4: Run full tests and start the 45-second observer**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  .\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe observe --duration-ms 45000
  ```

  Start the observer asynchronously. While it runs, use the `computer-use` skill and refresh Explorer state before every action: open one controlled Explorer window and record its single-tab HWND/PID/TID/selectors; add one tab to that window and record its multi-tab HWND/PID/TID/selectors; switch between its two tabs; open one existing child folder and navigate Back; close the added tab; close the controlled window. This restores the original window count, active tab, and folder before the observer ends.

  Expected: exact single-tab and multi-tab identity/selector snapshots plus at least one accepted `window_registered`, `window_revoked`, `tab_selected`, `tab_structure_changed`, `navigate_complete2`, and active-view remap with `active_view_count=1`; every event record correlates the triggering top-level HWND to the resulting active view and filesystem-path transition; no `ACTIVE_VIEW_CONTRACT_MISMATCH`; Host exits after 45 seconds without manual termination.

- [ ] **Step 5: Write Gate A evidence and evaluate the stop condition**

  The evidence document must contain target identity/preflight results, controlled single-tab and multi-tab HWND/PID/TID and selector scopes, exact commands, selector values, event kinds/counts, a bounded per-event table correlating source top-level HWND to resulting active view HWND and filesystem-path transition, active-view cardinality, exit codes, test summary, the Task 3 safety baseline summary, and `Gate A: PASS` or `Gate A: FAIL`. Do not paste unbounded raw UIA trees, certificate blobs, access tokens, or full environment variables.

  If FAIL, commit the evidence with the exact failure and stop this plan. If PASS, continue to Task 6.

- [ ] **Step 6: Commit Gate A implementation and evidence**

  ```powershell
  git add CMakeLists.txt src/probe tests docs/superpowers/evidence/2026-07-10-winexinfo-gate-a.md
  git commit -m "test: verify Explorer event mapping gate"
  ```

---

### Task 6: Versioned Attach/Detach IPC

**Files:**
- Create: `src/ipc/protocol.h`
- Create: `src/ipc/protocol.cpp`
- Create: `src/ipc/named_pipe.h`
- Create: `src/ipc/named_pipe.cpp`
- Create: `tests/ipc_protocol_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| Frame header | ASCII `WXI1`, `uint16 protocol_version=1`, `uint16 message_type`, `uint32 payload_length`, `uint64 request_id`, in that order |
| Integer/string encoding | all integers unsigned little-endian; each string is `uint32 byte_length` followed by NUL-free strict UTF-8 bytes |
| Maximum | total frame at most 256KiB; each string strictly below 256KiB |
| Pipe roles | Gate B controller is the Host-equivalent byte-mode server; one injected test-target DLL is the client; the role never reverses |
| Message types in Gate B | `3=detach_request`, `4=attach_result`, `5=detach_result` |
| Attach result | `request_id=attach_id`; `uint32 explorer_pid`, `uint32 ui_thread_id`, `uint64 top_level_hwnd`, `uint32 result`, `string error_code` |
| Detach request | `request_id=detach_id`; nonzero controller-process-unique strictly increasing detach ID, empty payload; never reset across DLL reloads |
| Detach result | matching `request_id=detach_id`; `uint32 explorer_pid`, `uint32 result`, `string error_code`; success means ready-to-unload only |
| Result rule | success=result 0+empty error; failure=nonzero+non-empty error |
| Pipe | `\\.\pipe\WinExInfo.v1.<SID>`, current-user ACL, `PIPE_REJECT_REMOTE_CLIENTS`, byte mode |
| Decoder/transport | exact version/type/length/order only; first mismatch closes that connection, serializes `IPC_PROTOCOL_ERROR`, and performs no resynchronization or legacy/alternate-field read |

- [ ] **Step 1: Write failing codec and pipe-security tests**

  Cover byte-exact valid frames; wrong magic/version/type; truncated/oversized payload; zero, reused, or decreasing detach ID; mismatched detach-result request ID; invalid UTF-8; embedded NUL; inconsistent success/error fields; extra trailing bytes; remote-reject creation flag; ACL without current user; and ACL with an unexpected broad principal. Use one real local byte-mode pipe pair to send a malformed frame followed by a valid frame on the same connection and assert the first mismatch closes the connection with `IPC_PROTOCOL_ERROR` before the valid frame can be decoded or used for resynchronization.

- [ ] **Step 2: Run IPC tests and verify red**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter ipc
  ```

  Expected: missing codec/pipe symbols or named FAIL cases.

- [ ] **Step 3: Implement the exact Gate B codec and local pipe transport**

  Apply byte-count checks before allocation and arithmetic overflow checks before addition. Do not implement status request/result payloads in this phase. Do not add JSON, text protocol, alternate pipe names, remote access, or retry under another SID.

- [ ] **Step 4: Run IPC and full unit tests**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  ```

  Expected: all `ipc.*` and prior tests pass; no network endpoint appears.

- [ ] **Step 5: Commit IPC**

  ```powershell
  git add CMakeLists.txt src/ipc tests
  git commit -m "feat: add versioned attach IPC"
  ```

---

### Task 7: Thread Hook Injection State Machine

**Files:**
- Create: `src/injection/hook_platform.h`
- Create: `src/injection/hook_platform.cpp`
- Create: `src/injection/thread_hook_injector.h`
- Create: `src/injection/thread_hook_injector.cpp`
- Create: `tests/hook_injector_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| Attach ID | target PID별 attach 직렬화; monotonic `1..INT64_MAX`, Host process lifetime에서 재사용 금지 |
| Event | hook 설치 전에 `CreateEventW`로 만드는 current-user-only, initially-nonsignaled manual-reset event; `ERROR_ALREADY_EXISTS` returns `HOOK_INSTALL_FAILED` before hook installation |
| Event name | `Local\WinExInfo.HookReleased.<PID>.<TID>.<attach_id decimal>` |
| Hook | `SetWindowsHookExW` with `WH_CALLWNDPROC`, exact `WinExInfoCallWndProc` x64 DLL export, and the validated target UI thread ID only |
| Trigger | `RegisterWindowMessageW` for exact name `WinExInfo.Attach.v1`, wParam `0x57495831`, lParam attach ID |
| Send | `SendMessageTimeoutW`, `SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT`, 1000ms |
| Attach wait | matching attach result or 5000ms |
| Release | call `UnhookWindowsHookEx`; signal event only on TRUE |
| Release failure | `HOOK_RELEASE_FAILED`, event remains unsignaled, no unload/no retry |
| Timeout event lifetime | keep event until target exit or DLL disappearance; no retry |
| Test seam | replace only the named Win32 operation table inside tests; no production CLI flag |

- [ ] **Step 1: Write failing injector state-machine tests**

  Cover SetHook failure; trigger failure+successful unhook; attach success+successful unhook; attach failure; attach timeout; unhook FALSE; late attach result; wrong attach ID/PID/TID/HWND; duplicate target; INT64_MAX exhaustion; event-name exactness; `ERROR_ALREADY_EXISTS`; initially-signaled rejection; auto-reset rejection; and an ACL containing any principal other than the current user. On unhook FALSE assert SetEvent and unload authorization are never called.

- [ ] **Step 2: Run injector tests and verify red**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter hook_injector
  ```

  Expected: missing injector/platform symbols or named FAIL cases.

- [ ] **Step 3: Implement the exact injector and production Win32 operation table**

  Every return path after successful SetHook must attempt one unhook. Event signaling must be adjacent to and conditional on `UnhookWindowsHookEx == TRUE`. Preserve the first operational error unless unhook fails; then expose `HOOK_RELEASE_FAILED` as final error with the original error retained only as diagnostic context.

- [ ] **Step 4: Run targeted and full tests**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  ```

  Expected: all injector fault paths pass; fake unhook failure leaves event unsignaled; prior tests pass.

- [ ] **Step 5: Commit injector state machine**

  ```powershell
  git add CMakeLists.txt src/injection tests
  git commit -m "feat: add safe thread hook state machine"
  ```

---

### Task 8: Hook DLL Runtime and Gate B Stress Harness

**Files:**
- Create: `src/hook/dll_main.cpp`
- Create: `src/hook/WinExInfoHook.def`
- Create: `src/hook/hook_entry.cpp`
- Create: `src/hook/runtime.h`
- Create: `src/hook/runtime.cpp`
- Create: `src/hook/status_pane.h`
- Create: `src/hook/status_pane.cpp`
- Create: `tests/hook_runtime_tests.cpp`
- Create: `tests/hook_target_mode.cpp`
- Create: `tests/hook_controller_mode.cpp`
- Create: `scripts/run-gate-b.ps1`
- Create after execution: `docs/superpowers/evidence/2026-07-10-winexinfo-gate-b.md`
- Modify: `tests/test_main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

| Produces | Exact contract |
|---|---|
| DLL export | exact undecorated name `WinExInfoCallWndProc`, C linkage, `LRESULT CALLBACK`/`WINAPI` ABI, outer parameters `int code`, `WPARAM hook_wparam`, `LPARAM hook_lparam`; one `WH_CALLWNDPROC` procedure only |
| DllMain | process attach에서 module handle을 저장하고 TRUE 반환; static CRT이므로 `DisableThreadLibraryCalls`를 호출하지 않으며 COM/thread/window/pipe 작업도 수행하지 않음 |
| Runtime states | `Starting`, `Running`, `Stopping`, `Stopped`; forward-only transitions |
| Stop trigger | first valid detach request or pipe disconnect transitions once to `Stopping`; duplicate triggers never restart cleanup |
| Module reference | first valid attach only; explicit `GetModuleHandleExW` reference |
| Hook callback | `code<0` passes immediately without dereferencing; otherwise outer `hook_lparam` is interpreted as `CWPSTRUCT*` and its `hwnd`, `message`, `wParam=0x57495831`, and nonzero `lParam=attach_id` are validated; outer `hook_wparam` is never treated as the magic; every path calls `CallNextHookEx` with the original outer code/WPARAM/LPARAM |
| Status pane | class `WinExInfo.StatusPane`; child of test target; fixed text `WinExInfo Gate B`; one `SetWindowSubclass` registration removed with the same HWND/proc/ID; no Explorer detection |
| Attach result | sent before Running; HookReleased must be signaled before Running |
| Detach result | success means ready-to-unload; close pipe then `FreeLibraryAndExitThread` on unload thread |
| Stop order | reject new work → cancel pending I/O → destroy pane/remove subclass and receive UI-thread ACK → stop workers and reach in-flight callback count 0 except the detach writer → release handles/COM → send detach result → close pipe → recheck HookReleased → unload |
| Stop timeout | each stop stage is bounded by 5000ms; timeout returns `DLL_UNLOAD_TIMEOUT` and retains the module without forced unload |
| Target CLI | `--hook-target` only |
| Ready event | target creates current-user-only, initially-nonsignaled manual-reset `Local\WinExInfo.GateBTarget.Ready.v1.<PID decimal>`, rejects `ERROR_ALREADY_EXISTS`, creates its window, then signals once |
| Test target | exact top-level window class `WinExInfo.GateBTarget.v1`; one `READY protocol=1 pid=<decimal> tid=<decimal> hwnd=<0x plus 16 uppercase hex>` LF-terminated UTF-8 record; stays alive through all controller cycles |
| Controller normal CLI | `--hook-controller <target PID decimal> --iterations <1..100>` only |
| Controller fault CLI | `--hook-controller <target PID decimal> --fault unhook-failure` only |
| Target validation | open and retain PID handle first; exact current test executable volume serial+file ID, same user/session/integrity, native AMD64, signaled exact ready event, and exactly one target-class top-level HWND whose PID/TID match |
| Controller accepted record | `TARGET_ACCEPTED protocol=1 pid=<decimal> tid=<decimal> hwnd=<0x plus 16 uppercase hex>` exactly once before hook installation |
| Harness validation | `scripts/run-gate-b.ps1` requires READY within 5000ms and requires its canonical PID/TID/HWND field strings to equal the controller `TARGET_ACCEPTED` fields before accepting results |
| Cycle result | target remains alive, child pane disappears, pipe closes, and DLL module disappears after every detach |
| Cycle IDs | one controller process issues strictly increasing, never-reused attach IDs and detach IDs across all iterations; every attach/detach result must echo the matching request ID |
| Normal termination | after final module disappearance, controller sends one `WM_CLOSE` to the validated HWND and requires target process exit code 0 within 5000ms |
| Unhook-fault result | test operation table leaves the real hook installed while returning FALSE; module remains and HookReleased stays unsignaled until controller closes the target; no unhook retry or forced unload |
| Test-mode exits | `0=pass/clean target exit`, `1=gate failure`, `2=invalid exact CLI`, `3=Win32/IPC failure` |

- [ ] **Step 1: Write failing runtime and cleanup tests**

  Cover exact `GetProcAddress("WinExInfoCallWndProc")` success and alternate-name failure; `code<0` with an invalid outer lParam and no dereference; original outer argument forwarding to `CallNextHookEx`; rejection when only outer WPARAM carries the magic; each wrong inner `CWPSTRUCT` field; valid/invalid attach messages; exact target/controller CLI forms and rejection of extras; ready-event ACL/manual-reset/initial-state creation and `ERROR_ALREADY_EXISTS`; event-open failure before module reference; HookReleased already signaled before attach result; first attach failure with zero panes; additional window failure with existing pane; exact subclass install/removal identity; duplicate detach; pending worker cancellation; in-flight callback counter; every stop-stage timeout; detach result ordering; unload attempt while event unsignaled; and a DllMain audit proving module-handle capture only and no `DisableThreadLibraryCalls`, COM, thread, window, or pipe call.

- [ ] **Step 2: Run runtime tests and verify red**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter hook_runtime
  ```

  Expected: missing hook runtime symbols or named FAIL cases.

- [ ] **Step 3: Implement the hook DLL and test-only process modes**

  Build `WinExInfoHook.dll` x64 with static CRT and CFG. The PowerShell harness starts only `WinExInfoTests.exe --hook-target`, reads exactly one READY line, then invokes the exact controller PID form. The controller rejects every mismatch listed in Target validation with `TARGET_VALIDATION_FAILED`; it never accepts a path match in place of file identity. Production Host remains probe-only in this plan.

- [ ] **Step 4: Run unit tests and one hook cycle**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Iterations 1
  ```

  Expected: `GATE_B_PASS iterations=1`; target remains alive through DLL disappearance; pane, subclass, worker threads, and pipe handles are absent after detach; controller exits 0.

- [ ] **Step 5: Run the 100-cycle stress test and fault verification**

  Run:

  ```powershell
  pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Iterations 100
  pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Fault UnhookFailure
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter hook_injector.unhook_failure
  ```

  After pipe close, poll the retained target process handle and Toolhelp module list for at most 5 seconds; success requires the exact DLL file identity to disappear without a forced unload. Before the first attach and after every confirmed module disappearance, read target and controller handle counts with `GetProcessHandleCount` and thread counts from one Toolhelp snapshot; each cycle must return all four deltas to zero before the next begins. Expected normal output: `GATE_B_PASS iterations=100 target_handles_delta=0 target_threads_delta=0 controller_handles_delta=0 controller_threads_delta=0 target_exit=0`. Expected fault output: `GATE_B_FAULT_PASS fault=unhook_failure module_retained=true event_signaled=false target_exit=0`; the target must remain alive until the module-retained and event-state checks complete. No `explorer.exe` process loads the DLL.

- [ ] **Step 6: Write Gate B evidence and evaluate the stop condition**

  Record exact build, commands, per-cycle result summary, starting/ending handle and thread counts, pane/subclass/worker/pipe cleanup checks, module disappearance checks, target liveness, fault-test result, and `Gate B: PASS` or `Gate B: FAIL`.

  If FAIL, commit the exact evidence and stop. If PASS, continue only to Task 9 verification; do not begin Gate C.

- [ ] **Step 7: Commit Gate B implementation and evidence**

  ```powershell
  git add CMakeLists.txt scripts/run-gate-b.ps1 src/hook tests docs/superpowers/evidence/2026-07-10-winexinfo-gate-b.md
  git commit -m "test: verify hook load and unload gate"
  ```

---

### Task 9: Release Verification and Mandatory Handoff

**Files:**
- Modify only if verification finds an evidence omission: `docs/superpowers/evidence/2026-07-10-winexinfo-gate-a.md`
- Modify only if verification finds an evidence omission: `docs/superpowers/evidence/2026-07-10-winexinfo-gate-b.md`

**Acceptance Criteria:**

| Area | Required evidence |
|---|---|
| Debug | configure, build, CTest all exit 0 |
| Release | configure, build, CTest all exit 0 |
| Runtime outputs | `WinExInfoHost.exe`, `WinExInfoTests.exe`, `WinExInfoHook.dll` exist in Release bin |
| Gate A | exact target preflight, controlled single/multi-tab selectors, and active-view/event/path correlation PASS in Debug and Release |
| Gate B | 100 cycles PASS with per-cycle zero handle/thread delta, pane/subclass/worker/pipe cleanup, module disappearance while target remains alive, then target exit 0 |
| Safety | no Explorer module load, registry change, service/task creation, network connection |
| Repository | `git diff --check` clean; evidence committed; no generated binaries tracked |

- [ ] **Step 1: Run fresh Debug and Release verification**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Test
  pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Release -Iterations 100
  pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Release -Fault UnhookFailure
  ```

  Expected: every command exits 0; the Release normal harness prints Gate B PASS with per-cycle zero deltas and target exit 0; the Release fault harness prints Gate B FAULT PASS only after proving module retention, unsignaled HookReleased, and target exit 0.

- [ ] **Step 2: Re-run the read-only Gate A snapshot and observer in Release**

  ```powershell
  .\out\build\windows-x64-release\bin\WinExInfoHost.exe --probe snapshot
  .\out\build\windows-x64-release\bin\WinExInfoHost.exe --probe observe --duration-ms 45000
  ```

  Start the observer asynchronously and repeat the exact controlled single-tab→multi-tab→tab switch→child navigation→Back→tab close→window close sequence from Task 5 with fresh `computer-use` state before each action. Expected: both commands exit 0; target preflight and exact selectors pass; one active filesystem view exists per applicable window; all five event kinds correlate to the expected view/path transitions; no fallback field.

- [ ] **Step 3: Verify safety and repository state**

  Verify the DLL is absent from every `explorer.exe` module list. Compare the exact `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WinExInfo` value, services named `WinExInfo`, scheduled tasks whose exact task name is `WinExInfo`, and outbound connections owned by WinExInfo processes with the baseline captured before Task 3; all four states must be unchanged. Then run:

  ```powershell
  git diff --check
  git status --short --branch
  ```

  Expected: diff check exits 0; only an intentional evidence correction may be pending.

- [ ] **Step 4: Commit an evidence-only correction if Step 3 required one**

  If no correction exists, skip this commit. If evidence text alone changed:

  ```powershell
  git add docs/superpowers/evidence
  git commit -m "docs: finalize WinExInfo feasibility evidence"
  ```

- [ ] **Step 5: Stop before Gate C and report to 형님**

  Report Gate A and B outcomes, commit SHAs, verification commands, and any residual risk. Ask for explicit approval to write and execute the separate Gate C live Explorer placement plan. Do not inject `WinExInfoHook.dll` into `explorer.exe`, register autostart, or start Git-engine work in this task.

---

## Spec Coverage Self-Check

| Design spec area | Covered here | Task |
|---|---|---|
| Exact target identity/preflight and build policy | Yes | 1, 3, 9 |
| HWND/UIA selectors and cardinality | Yes | 3 |
| Shell active-view mapping | Yes | 4 |
| Navigation/tab event sources and scopes | Yes | 5 |
| Versioned attach/detach IPC subset | Yes | 6 |
| WH_CALLWNDPROC trigger/unhook ordering | Yes | 7 |
| Module reference and unload state | Yes | 8 |
| Gate A/B evidence and stop rules | Yes | 5, 8, 9 |
| Live Explorer child HWND placement | Deferred by approved Gate C boundary | Separate plan after explicit approval |
| Git status parser/runner/watchers | Outside Gate A/B scope | Separate post-gate plan |
| Product status-pane rendering | Outside Gate A/B scope | Separate post-gate plan |
| Install, Run key, status, uninstall | Outside Gate A/B scope | Separate post-gate plan |

The deferred rows are intentional scope boundaries, not missing implementation steps. Gate A/B must pass before those plans are authored or executed.
