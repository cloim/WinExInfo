# WinExInfo Gate C Explorer Placement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove that `WinExInfoHook.dll` can create, position, update, and remove an internal child HWND in one validated real Explorer window without covering either native status-bar group.

**Architecture:** Extend the tested hook runtime with an Explorer-specific layout worker and a production controller command. The DLL owns the pane HWND and UI-thread subclass; a DLL MTA worker reads the exact UIA selectors and posts immutable placement metrics back to the Explorer UI thread. The Host injects only the explicitly selected validated window, observes cleanup, and exits.

**Tech Stack:** C++20, MSVC x64 static CRT, Win32, UI Automation COM, `WH_CALLWNDPROC`, named pipes, CMake/Ninja, PowerShell evidence harness.

## Global Constraints

- Support only Windows 11 Pro `10.0.26200.8655`, native AMD64, Explorer/ExplorerFrame/shell32 fixed version `10.0.26100.8655`.
- Use exact HWND, class, AutomationId, ControlType, FrameworkId, process, thread, file identity, signer, session, user, integrity, and mitigation contracts; no alternate key or UI fallback.
- Do not parse window titles, tab names, address-bar text, URLs, or localized UI names.
- Do not inject with remote threads, patch DirectUI, create an external overlay, access the network, or persist anything in Gate C.
- Every wait is bounded by 5000 ms; cleanup failure retains the DLL instead of forcing unload.

---

### Task 1: Gate C Exact CLI and Target Selection

**Files:**
- Modify: `src/host/command_line.h`
- Modify: `src/host/command_line.cpp`
- Modify: `src/host/main.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/command_line_tests.cpp`

**Interfaces:**
- Produces: `HostCommand::GateCPlace`, `ParsedCommand::target_hwnd`, exact CLI `--gate-c-place --hwnd 0x<16 uppercase hex> --duration-ms <5000..30000>`.
- Consumes: existing `RunSnapshotProbe()` target and UIA contract.

- [ ] **Step 1: Write failing CLI tests**

```cpp
WXI_TEST(command_line_gate_c_exact, "command_line.gate_c") {
    ParsedCommand command{};
    WXI_REQUIRE(ParseCommandLine(
        {"--gate-c-place", "--hwnd", "0x000000001234ABCD",
         "--duration-ms", "15000"}, &command).ok());
    WXI_REQUIRE_EQ(command.command, HostCommand::GateCPlace);
    WXI_REQUIRE_EQ(command.target_hwnd, std::uint64_t{0x1234ABCD});
    WXI_REQUIRE_EQ(command.duration_ms, std::uint32_t{15000});
}
```

Reject lowercase hex, missing `0x`, fewer/more than 16 digits, zero HWND, reordered keys, extras, duration 4999/30001, and probe arguments mixed with Gate C.

- [ ] **Step 2: Run the CLI tests and verify red**

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter command_line.gate_c
```

Expected: compile failure for missing `GateCPlace`/`target_hwnd` or named test failures.

- [ ] **Step 3: Implement the exact parser**

```cpp
enum class HostCommand { ProbeSnapshot, ProbeObserve, GateCPlace };
struct ParsedCommand final {
    HostCommand command{};
    std::uint32_t duration_ms = 0;
    std::uint64_t target_hwnd = 0;
};
```

Parse exactly 16 uppercase hex digits after `0x` with `std::from_chars(..., 16)` and fail with `INVALID_ARGUMENT` on any mismatch.

- [ ] **Step 4: Run command-line and full unit tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter command_line
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/host tests/command_line_tests.cpp
git commit -m "feat: add exact Gate C command"
```

### Task 2: Exact Explorer Gap Layout Contract

**Files:**
- Create: `src/hook/explorer_layout.h`
- Create: `src/hook/explorer_layout.cpp`
- Create: `tests/explorer_layout_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ExplorerLayoutMetrics`, `ComputeStatusPaneRect`, `CaptureExplorerLayout`.
- Consumes: exact `CabinetWClass -> first visible direct-child ShellTabWindowClass -> one DUIViewWndClassName` and UIA selectors from the design.

- [ ] **Step 1: Write failing pure-layout tests**

```cpp
struct ExplorerLayoutMetrics final {
    RECT parent_screen{};
    RECT status_screen{};
    RECT left_group_screen{};
    RECT right_group_screen{};
    UINT dpi = 96;
};

WXI_TEST(explorer_layout_gap, "explorer_layout.gap") {
    RECT output{};
    WXI_REQUIRE(ComputeStatusPaneRect(
        {{100,100,1100,800}, {100,760,1100,800},
         {110,760,250,800}, {900,760,1090,800}, 96}, &output).ok());
    WXI_REQUIRE_EQ(output, (RECT{158,660,792,700}));
}
```

Cover 8-DIP margins, `ScreenToClient` equivalent conversion, 96-DIP minimum width, clipping, negative/overflow rectangles, overlap, reversed groups, DPI 96/144/192, and hidden result.

- [ ] **Step 2: Run tests and verify red**

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter explorer_layout
```

Expected: missing symbols or named failures.

- [ ] **Step 3: Implement pure geometry and production UIA capture**

```cpp
[[nodiscard]] Status ComputeStatusPaneRect(
    const ExplorerLayoutMetrics& metrics,
    RECT* output) noexcept;
[[nodiscard]] Status CaptureExplorerLayout(
    HWND topLevel,
    ExplorerLayoutMetrics* metrics,
    HWND* paneParent);
```

`CaptureExplorerLayout` initializes UIA on a dedicated MTA, requires exact cardinality one for StatusBar/left/right groups, requires NativeWindowHandle 0 for those UIA elements, captures BoundingRectangle, and returns the exact `DUIViewWndClassName` HWND only.

- [ ] **Step 4: Run targeted and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter explorer_layout
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/hook/explorer_layout.* tests/explorer_layout_tests.cpp
git commit -m "feat: compute exact Explorer status gap"
```

### Task 3: UI-Thread Pane Placement and Reflow

**Files:**
- Modify: `src/hook/status_pane.h`
- Modify: `src/hook/status_pane.cpp`
- Modify: `src/hook/runtime.h`
- Modify: `src/hook/runtime.cpp`
- Create: `tests/status_pane_placement_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ApplyStatusPanePlacement(HWND pane, HWND parent, RECT rect, bool visible)`, internal reflow message `WM_APP + 0x572`.
- Consumes: `ExplorerLayoutMetrics` and existing same-proc/same-ID subclass cleanup.

- [ ] **Step 1: Write failing placement/lifetime tests**

Use a Win32 operation table to assert exact `SetParent`, `SetWindowPos(HWND_TOP, ..., SWP_NOACTIVATE | SWP_SHOWWINDOW)`, `WS_CHILD`, `WS_EX_NOACTIVATE`, hide-under-96-DIP, no focus activation, same UI thread, and reflow coalescing. Assert cleanup removes subclass before destroying the pane.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter status_pane.placement
```

Expected: missing placement symbols or failures.

- [ ] **Step 3: Implement UI-thread-only placement**

```cpp
[[nodiscard]] Status ApplyStatusPanePlacement(
    HWND pane, HWND expectedParent, const RECT& rect, bool visible) noexcept;
```

The MTA worker posts immutable layout results; the Explorer UI thread revalidates PID/TID/class/parent, then applies placement. Resize, DPI, theme, and tab-visibility signals only enqueue one refresh.

- [ ] **Step 4: Run placement, hook runtime, and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter status_pane
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter hook_runtime
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/hook tests/status_pane_placement_tests.cpp
git commit -m "feat: place pane in Explorer status gap"
```

### Task 4: Production Gate C Controller

**Files:**
- Create: `src/host/explorer_controller.h`
- Create: `src/host/explorer_controller.cpp`
- Create: `tests/explorer_controller_tests.cpp`
- Modify: `src/host/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `RunGateCPlacement(HWND target, std::uint32_t durationMs)`.
- Consumes: production preflight, named pipe, `ThreadHookInjector`, exact DLL file identity, and pane placement runtime.

- [ ] **Step 1: Write failing controller-state tests**

Cover one exact top-level target, wrong/non-Explorer HWND, target disappearing before attach, process/thread mismatch, attach result mismatch, pane-owner PID mismatch, layout failure, duration completion, detach ordering, module disappearance, and cleanup timeout retention.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter explorer_controller
```

- [ ] **Step 3: Implement the controller**

```cpp
[[nodiscard]] HostExitCode RunGateCPlacement(
    HWND target,
    std::uint32_t durationMs,
    std::wstring_view hookDllPath);
```

The controller validates one existing Explorer window, prints one canonical `TARGET_ACCEPTED`, attaches, requires the pane owner PID to equal Explorer PID, waits the requested duration without Git work, detaches, verifies the pane and exact DLL file identity disappear, and returns 0/1/3 by contract.

- [ ] **Step 4: Run non-Explorer integration and full tests**

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Iterations 100
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/host tests/explorer_controller_tests.cpp
git commit -m "feat: add bounded Explorer placement controller"
```

### Task 5: Live Gate C Harness and Evidence

**Files:**
- Create: `scripts/run-gate-c.ps1`
- Create after execution: `docs/superpowers/evidence/2026-07-12-winexinfo-gate-c.md`

**Interfaces:**
- Produces: exact `GATE_C_PASS` record and Gate C evidence.
- Consumes: Release Host/Hook and one controlled Explorer window.

- [ ] **Step 1: Implement exact harness validation**

```powershell
param([ValidateSet('Debug','Release')]$Configuration='Release')
```

The script opens one controlled Explorer window at `D:\PROJECTS\WinExInfo`, captures canonical HWND/PID/TID and native group rectangles, runs `--gate-c-place` for 15000 ms, verifies pane PID/parent/class/text/rectangle, resizes and switches tabs, verifies no overlap, waits for cleanup, closes only the controlled window, and restores the original state.

- [ ] **Step 2: Run Debug Gate C**

```powershell
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Debug
```

Expected: `GATE_C_PASS configuration=Debug pane_owner=explorer overlap=false cleanup=true`.

- [ ] **Step 3: Run Release Gate C**

```powershell
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Release
```

Expected: the same PASS record with `configuration=Release`.

- [ ] **Step 4: Record evidence and safety state**

Record exact selectors, HWND/PID/TID, before/during/after rectangles, resize/tab transition, pane owner, module cleanup, Run/service/task/process/network counts, commands, exits, and `Gate C: PASS`. If any exact contract fails, record `Gate C: FAIL`, commit evidence, and stop product implementation without fallback.

- [ ] **Step 5: Commit**

```powershell
git add scripts/run-gate-c.ps1 docs/superpowers/evidence/2026-07-12-winexinfo-gate-c.md
git commit -m "test: verify real Explorer pane placement"
```

