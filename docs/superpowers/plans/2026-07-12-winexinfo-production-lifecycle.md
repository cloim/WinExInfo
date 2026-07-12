# WinExInfo Production Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Gate C one-window proof into a session-long Host/DLL lifecycle supporting all validated Explorer processes, windows, and tabs.

**Architecture:** A single Host background coordinator owns the Shell STA and UIA MTA state, reconciles exact HWND/canonical identity maps, and sends immutable generation-tagged tab/layout updates over the current-user pipe. Each Explorer DLL owns UI-thread subclasses and one pane per top-level window, rejects stale generations, and performs bounded reverse cleanup.

**Tech Stack:** C++20, Win32, COM STA/MTA, UI Automation, named-pipe binary protocol v1, Toolhelp, Job Objects, CMake/Ninja.

## Global Constraints

- Gate C evidence must say `Gate C: PASS` before this plan starts.
- Keep the exact target, HWND identity, security, IPC, callback rundown, and no-fallback contracts from the approved design.
- Raw COM/UIA pointers never cross apartments or the pipe; only immutable integers, HWND values, generations, enums, and UTF-8 strings cross.
- Production background mode performs no Git or installation work in this plan.

---

### Task 1: Protocol v1 Production Messages

**Files:**
- Modify: `src/ipc/protocol.h`
- Modify: `src/ipc/protocol.cpp`
- Create: `tests/ipc_production_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces message types `6=tab_set_update`, `7=tab_set_result`, `8=pane_text_update`, `9=pane_text_result`.
- Produces structs `TabDescriptor`, `TabSetUpdate`, `TabSetResult`, `PaneTextUpdate` with exact request/generation correlation.

- [ ] **Step 1: Write byte-exact failing codec tests**

```cpp
struct TabDescriptor final {
    std::uint64_t tab_hwnd;
    std::uint64_t tab_generation;
    std::uint32_t ui_thread_id;
};
```

Cover ordered descriptors, duplicate HWND/generation, zero values, stale top-level generation, maximum sizes, strict UTF-8/NUL rejection, unknown flags/types, result mismatch, and trailing bytes.

- [ ] **Step 2: Run IPC tests and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter ipc.production
```

- [ ] **Step 3: Implement exact codecs without alternate fields**

```cpp
[[nodiscard]] Status EncodeTabSetUpdate(
    std::uint64_t requestId, const TabSetUpdate&, std::vector<std::uint8_t>*);
[[nodiscard]] Status DecodeTabSetResult(
    const DecodedFrame&, std::uint64_t requestId, TabSetResult*);
```

- [ ] **Step 4: Run all IPC and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter ipc
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/ipc tests/ipc_production_tests.cpp
git commit -m "feat: add production tab update protocol"
```

### Task 2: DLL Tab Subclass Set and Generation State

**Files:**
- Create: `src/hook/tab_subclass_set.h`
- Create: `src/hook/tab_subclass_set.cpp`
- Create: `tests/tab_subclass_tests.cpp`
- Modify: `src/hook/runtime.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ApplyTabSetUpdate`, `RemoveAllTabSubclasses`, exact subclass ID `0x57495832`.
- Consumes: `TabSetUpdate` and Explorer UI-thread dispatch.

- [ ] **Step 1: Write failing generation/lifetime tests**

Cover same-thread validation, exact class/direct parent, ordered add/remove, stale top-level/tab generation, HWND reuse, duplicate update, destroy signal, rollback, wrong ACK, and reverse cleanup.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter tab_subclass
```

- [ ] **Step 3: Implement minimal same-thread set reconciliation**

```cpp
[[nodiscard]] Status ApplyTabSetUpdate(
    HWND topLevel, const ipc::TabSetUpdate& update,
    const TabSubclassOperations& operations,
    ipc::TabSetResult* result);
```

Subclass procedures report destruction/geometry only, post refresh, and immediately call `DefSubclassProc`; no COM, UIA, pipe waits, filesystem, or Git work occurs there.

- [ ] **Step 4: Run targeted, Gate B, and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter tab_subclass
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Iterations 100
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/hook tests/tab_subclass_tests.cpp
git commit -m "feat: manage exact Explorer tab subclasses"
```

### Task 3: Background Coordinator and Process Sessions

> **Prerequisite addendum:** Before this task, complete and review all tasks in
> `2026-07-12-winexinfo-multi-window-runtime-addendum.md`. The Gate C runtime
> owns one top-level/pane today, while this task requires one pipe/session per
> PID with several top-level windows. A mock-only coordinator is not acceptable.

**Files:**
- Create: `src/host/background_coordinator.h`
- Create: `src/host/background_coordinator.cpp`
- Create: `src/host/explorer_session.h`
- Create: `src/host/explorer_session.cpp`
- Create: `tests/background_coordinator_tests.cpp`
- Modify: `src/host/command_line.*`
- Modify: `src/host/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: exact `--background`, `RunBackgroundCoordinator`, one `ExplorerSession` per validated PID.
- Consumes: observer reconciliation snapshots, injection controller, production protocol.

- [ ] **Step 1: Write failing coordinator tests**

Cover single-instance mutex `Local\WinExInfo.Host.v1`, existing windows, process start/exit, one attach per PID, several top-level windows, batched tabs, stale generations, transport failure, unsupported process skip, and deterministic stop.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter background_coordinator
```

- [ ] **Step 3: Implement the coordinator**

```cpp
[[nodiscard]] HostExitCode RunBackgroundCoordinator(
    const BackgroundOperations& operations);
```

The coordinator owns immutable snapshot queues; Shell COM stays on the Shell STA, UIA stays on one MTA, and each process session serializes attach/update/detach requests.

- [ ] **Step 4: Run non-injected integration and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter background_coordinator
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/host tests/background_coordinator_tests.cpp
git commit -m "feat: coordinate Explorer background sessions"
```

### Task 4: Multi-Window/Tab Live Lifecycle Evidence

**Files:**
- Create: `scripts/run-production-lifecycle.ps1`
- Create: `docs/superpowers/evidence/2026-07-12-winexinfo-production-lifecycle.md`

**Interfaces:**
- Produces: `LIFECYCLE_PASS` and evidence required by the Git-status plan.

- [ ] **Step 1: Implement a reversible controlled harness**

Start Host `--background`, open two controlled windows, add/switch/close tabs, resize, close one window, restart a controlled Explorer process, stop Host, and compare exact pane/subclass/module/handle/thread/safety baselines.

- [ ] **Step 2: Run Debug and Release**

```powershell
pwsh -NoProfile -File .\scripts\run-production-lifecycle.ps1 -Configuration Debug
pwsh -NoProfile -File .\scripts\run-production-lifecycle.ps1 -Configuration Release
```

Expected: `LIFECYCLE_PASS windows=2 tabs_changed=true restart=true cleanup=true`.

- [ ] **Step 3: Record evidence**

Include exact identities, generations, transitions, resource counts, pane ownership, module disappearance, exits, and unchanged safety state. A failure stops before Git functionality.

- [ ] **Step 4: Commit**

```powershell
git add scripts/run-production-lifecycle.ps1 docs/superpowers/evidence/2026-07-12-winexinfo-production-lifecycle.md
git commit -m "test: verify production Explorer lifecycle"
```

