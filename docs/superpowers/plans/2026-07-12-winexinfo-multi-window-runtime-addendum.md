# WinExInfo Multi-Window Runtime Addendum Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development and strict test-driven-development task by task.

**Goal:** Close the implementation gap between the approved lifecycle architecture (one Host session and one pipe per Explorer PID, one pane per top-level window) and the current Gate C runtime (one top-level window and one pane).

**Architecture:** One process-wide DLL runtime owns the pipe and a bounded registry of UI-thread-owned window runtimes. The Host retains one exact `WH_CALLWNDPROC` hook lease per participating Explorer UI thread. The pipe worker serializes immutable updates and dispatches them with `SendMessageTimeoutW` to the exact top-level HWND; that thread's retained hook consumes the registered control message and mutates only its own window runtime. No raw COM/UIA pointer crosses a thread or pipe.

**Global constraints:**

- Preserve strict Gate C Debug/Release PASS, exact target validation, current-user IPC, callback rundown, no fallback, and cleanup-retention rules.
- One `ExplorerSession` and one connected pipe client per Explorer PID. Several UI-thread hook leases may belong to that one session.
- Maximum validated top-level windows per process is 64. Maximum wait is 5000 ms. Every lookup/traversal is bounded.
- Explorer UI-thread hook callbacks perform only exact fixed-buffer validation, lock-free ingress/control admission, bounded local HWND/subclass work, and immediate `CallNextHookEx`; no COM, UIA, pipe wait, filesystem, Git, or heap payload ownership transfer.
- Any uncertain hook/subclass/pane cleanup retains the process runtime and DLL. No partial cleanup may acknowledge detach.

### Task A: Instance-Owned Tab Subclass State

**Files:**
- Modify: `src/hook/tab_subclass_set.h`
- Modify: `src/hook/tab_subclass_set.cpp`
- Modify: `src/hook/runtime.cpp`
- Modify: `tests/tab_subclass_tests.cpp`

**Interfaces:**

```cpp
class TabSubclassSet final {
public:
    Status Apply(HWND topLevel, const ipc::TabSetUpdate&,
                 const TabSubclassOperations&, ipc::TabSetResult*);
    Status RemoveAll(const TabSubclassOperations&);
    bool cleanup_safe() const noexcept;
    void Notify(HWND window, UINT message) noexcept;
};
```

- Remove process-global tab state. Each window runtime owns exactly one set.
- Preserve every reviewed generation, conservative ledger, rollback, cycle bound, exhaustive cleanup, and callback restriction.
- Tests first: two independent sets may use identical HWND numeric values without sharing generations/cleanup; destroying or failing one set cannot mutate the other.
- Run `tab_subclass`, `hook_runtime`, full Debug, and Gate B fault/normal evidence. Commit `refactor: isolate tab subclass state per window`.

### Task B: One-Pipe Multi-Window DLL Runtime

**Files:**
- Create: `src/hook/process_runtime.h`
- Create: `src/hook/process_runtime.cpp`
- Create: `tests/process_runtime_tests.cpp`
- Modify: `src/hook/runtime.h`
- Modify: `src/hook/runtime.cpp`
- Modify: `src/hook/hook_entry.*`
- Modify: `src/hook/hook_export.cpp`
- Modify: `CMakeLists.txt`

**Interfaces and exact behavior:**

```cpp
inline constexpr std::size_t kMaximumRuntimeWindows = 64;
inline constexpr UINT kProcessRuntimeControlMagic = 0x57495833;

struct WindowRuntimeKey final {
    HWND top_level;
    DWORD process_id;
    DWORD ui_thread_id;
    std::uint64_t generation;
};

Status DispatchProcessTabSetUpdate(
    ProcessRuntime&, const ipc::TabSetUpdate&, ipc::TabSetResult*);
Status RemoveAllProcessWindows(ProcessRuntime&);
bool HandleProcessRuntimeControlMessage(
    HWND, UINT, WPARAM, LPARAM) noexcept;
```

- The initial Gate C attach creates the process runtime and first `WindowRuntime`; additional top-level windows are created only by a valid type-6 update.
- A window update's first descriptor UI thread must equal the exact top-level TID and every descriptor TID. Validate `CabinetWClass`, PID, top-level ancestry, exact ordered direct tabs, process/session/user contracts already captured by Host.
- The pipe worker stores one serialized pending control request with a nonzero monotonic token, then calls `SendMessageTimeoutW` on the exact top-level with the registered `WinExInfo.ProcessControl.v1` message, magic, and token, using `SMTO_ABORTIFHUNG | SMTO_BLOCK`, 5000 ms. The retained hook on that UI thread validates message/magic/token/HWND/PID/TID before consuming it. Timeout or mismatch is transport failure and leaves state retained.
- Each `WindowRuntime` owns its pane, refresh worker/ingress/coordinator, parent-destroy shelter, signal-source state, and instance-owned `TabSubclassSet`. A bounded process registry contains at most 64 stable slots; hook ingress scans only those slots and never takes the process mutex.
- A removed top-level is cleaned on its own UI thread in reverse creation order. Window destruction, HWND reuse, generation staleness, partial creation, post failure, cleanup timeout, and process stop all fail closed.
- Process detach first stops admission, then cleans every window in reverse creation order, drains callbacks, verifies all thread hooks released, replies once, and unloads the DLL.
- Tests first cover two windows on one/different UI threads, one pipe, serialized token correlation, max 64/65, window add/remove/reuse, independent panes/tab sets/refresh, timeout, rollback, reverse cleanup, and Gate C single-window compatibility.
- Run `process_runtime`, `hook_runtime`, `tab_subclass`, full Debug, Gate B, and strict Gate C Debug (Release remains for lifecycle E2E). Commit `feat: manage multiple Explorer windows in one DLL runtime`.

### Task C: Per-Process Multi-Thread Hook Leases and Session Transport

**Files:**
- Modify: `src/injection/thread_hook_injector.h`
- Modify: `src/injection/thread_hook_injector.cpp`
- Create: `src/host/explorer_session.h`
- Create: `src/host/explorer_session.cpp`
- Create: `tests/explorer_session_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct SessionWindowSnapshot final {
    HWND top_level;
    std::uint64_t top_level_generation;
    std::vector<ipc::TabDescriptor> tabs;
};

class ExplorerSession final {
public:
    Status Reconcile(std::span<const SessionWindowSnapshot>);
    Status Stop();
};
```

- One session owns one PID, one server pipe/client connection, one initial attach, and a map `ui_thread_id -> retained HHOOK lease`.
- Before sending a window update, ensure the exact validated UI thread has a retained hook. The initial hook performs attach; later hooks are observation/control-only and never create a second pipe/runtime.
- Reconcile immutable snapshots in top-level order, send one type-6 update at a time, require exact type-7 request/generation ACK, then remove absent windows in reverse prior order through an authoritative empty/removal control operation (extend the internal control dispatch, not protocol fallback fields).
- Stop unhooks every lease in reverse install order, signals `HookReleased` only after all exact unhooks succeed, sends one detach request, requires exact ACK/module disappearance, and retains all state on uncertainty.
- Tests first: one attach per PID, two windows same/different TID, no duplicate HHOOK for shared TID, add/remove/restart, stale generation, transport failure, unhook failure retention, reverse stop, exact one-pipe correlation.
- Run `explorer_session`, `hook_injector`, `explorer_controller`, full Debug, Gate B. Commit `feat: manage Explorer process hook leases`.

### Resume Original Lifecycle Task 3

After Tasks A-C review clean, implement `BackgroundCoordinator` using immutable snapshots:

```cpp
struct ExplorerProcessSnapshot final {
    DWORD process_id;
    bool validated;
    std::vector<SessionWindowSnapshot> windows;
};

struct BackgroundSnapshot final {
    std::uint64_t sequence;
    std::vector<ExplorerProcessSnapshot> processes;
};
```

One coordinator owns `PID -> ExplorerSession`, accepts strictly increasing snapshot sequences, reconciles validated processes, skips unsupported processes, stops absent sessions deterministically, and keeps Shell STA/UIA MTA ownership in their established workers.

