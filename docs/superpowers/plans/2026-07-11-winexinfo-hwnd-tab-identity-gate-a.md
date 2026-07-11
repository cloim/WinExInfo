# WinExInfo HWND Tab Identity Gate A Implementation Plan

> **For agentic workers:** Execute this plan task by task with `superpowers:executing-plans`. Use `superpowers:subagent-driven-development` only when 형님 explicitly requests delegated work.

**Goal:** Replace Shell lifecycle-cookie correlation with exact Explorer tab HWND reconciliation and prove the revised event-observation contract in Gate A.

**Architecture:** Shell lifecycle, UIA, and navigation callbacks are wake signals only. After each accepted signal, the Shell STA captures the complete current `CabinetWClass`/direct-child `ShellTabWindowClass` set twice, joins every targeted tab HWND to exactly one `IShellBrowser::GetWindow` result and canonical `IUnknown` identity, then transactionally updates browser subscriptions. The lifecycle cookie remains diagnostic data and is never an identity, key, or fallback.

**Tech Stack:** C++20, Win32, Shell COM, UI Automation, WRL, CMake/MSVC, the existing WinExInfo test framework, PowerShell build scripts.

## Global Constraints

- Work only in `D:\PROJECTS\WinExInfo\.worktrees\winexinfo-gates-a-b` on `feature/winexinfo-gates-a-b`.
- Preserve the exact target OS and Explorer build policy already defined in `src/common/contracts.h`.
- Use only the exact `CabinetWClass` top-level HWND and direct-child `ShellTabWindowClass` HWND contract. Do not add class-name alternatives, legacy keys, cookie lookup, or best-effort matching.
- Treat every cookie carried by `WindowRegistered` and `WindowRevoked` as diagnostic-only data.
- Never pass raw COM or UIA pointers between apartments. Cross-thread messages contain immutable scalar values, handles, generations, event kinds, and copied strings only.
- Keep callbacks short: validate and enqueue only. Reconciliation, COM capture, subscription changes, reporting, and gate evaluation run after the callback returns.
- A mismatch fails with the existing exact error code `ACTIVE_VIEW_CONTRACT_MISMATCH`; transport failures retain the exact HRESULT or Win32 code and exit through the existing Host error contract.
- Stop after revised Gate A evidence is written when Gate A fails. Gate B production DLL and real Explorer injection remain outside this plan.
- Follow test-driven development because this changes runtime identity and event behavior. Run the named failing test before each implementation step.

---

### Task 1: Extract the Exact Tab-Set Reconciliation Contract

**Files:**

- Create: `src/probe/tab_identity.h`
- Create: `src/probe/tab_identity.cpp`
- Create: `tests/tab_identity_tests.cpp`
- Modify: `src/probe/observer_runtime.h`
- Modify: `src/probe/observer_runtime.cpp`
- Modify: `tests/observer_runtime_tests.cpp`
- Modify: `CMakeLists.txt`

**Exact contract:**

| Input or output | Required meaning |
|---|---|
| Target tab identity | Exact tuple of top-level HWND, direct-child tab HWND, and nonzero canonical `IUnknown` identity |
| Ordered HWND input | Stable direct-child `ShellTabWindowClass` z-order for each targeted top-level HWND |
| Previous/current set | Complete targeted set, not the individual object suggested by a callback cookie |
| Retained entry | All three identity fields unchanged; generations remain unchanged |
| Added entry | Present only in current set; receives the next nonzero generation for its exact HWND lifetime |
| Removed entry | Present only in previous set; its generation becomes inactive and remains recorded as the latest generation |
| Handle reuse | Same HWND with a different canonical identity is one removal plus one addition, never retention |
| Active tab | First visible exact tab in the confirmed z-order, and it must occur exactly once in the current target set |
| Failure | Duplicate HWND, duplicate canonical identity, zero identity, missing/extra join, unstable order, or invalid active tab returns `ACTIVE_VIEW_CONTRACT_MISMATCH` |

- [ ] **Step 1: Write failing pure reconciliation tests**

  Cover unchanged/reordered input, one and many additions, one and many removals, simultaneous additions/removals, same-HWND identity replacement, duplicate tab HWND, duplicate canonical identity, zero identity, missing and extra target entries, non-target Shell entries, invalid active HWND, generation retention, and generation increment after handle reuse.

- [ ] **Step 2: Run the focused tests and confirm red**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter tab_identity
  ```

  Expected: missing reconciliation types/functions or named failing cases.

- [ ] **Step 3: Implement the pure reconciler and remove cookie correlation helpers**

  Move the shared Shell-entry metadata into the focused tab-identity module. Replace `ClassifyObserverShellSetTransition` and `CorrelateObserverShellLifecycle` with one complete-set reconciliation operation. Keep generation assignment explicit and deterministic. Do not retain an alternate single-add/single-remove path.

- [ ] **Step 4: Run focused and full unit tests**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  ```

  Expected: all reconciliation tests and all unaffected existing tests pass.

- [ ] **Step 5: Commit the pure contract**

  ```powershell
  git add CMakeLists.txt src/probe/tab_identity.* src/probe/observer_runtime.* tests/tab_identity_tests.cpp tests/observer_runtime_tests.cpp
  git commit -m "refactor: reconcile Explorer tabs by HWND"
  ```

---

### Task 2: Capture a Stable Ordered Tab HWND Snapshot

**Files:**

- Modify: `src/probe/win32_probe.h`
- Modify: `src/probe/win32_probe.cpp`
- Modify: `src/probe/shell_probe.h`
- Modify: `src/probe/shell_probe.cpp`
- Modify: `tests/uia_contract_tests.cpp`
- Modify: `tests/active_view_tests.cpp`

**Exact contract:**

- Each targeted `CabinetWClass` snapshot exposes the full ordered list of direct-child `ShellTabWindowClass` HWNDs, not only `active_shell_tab`.
- Capture the direct-child z-order twice. Both ordered HWND vectors must be identical before the snapshot is accepted.
- The first visible tab in that confirmed order is the active tab. Exactly one active filesystem view must map to it.
- Shell enumeration joins each targeted tab HWND through `IShellBrowser::GetWindow`; every targeted HWND has exactly one canonical Shell identity and every targeted Shell identity has exactly one HWND.
- Hidden tabs remain in the complete ordered set. Visibility determines the active member only.

- [ ] **Step 1: Add failing stable-order and bijection tests**

  Cover single tab, multiple tabs, hidden inactive tabs, active-first visibility, order changing between captures, missing Shell browser, extra Shell browser, duplicate join, and an HWND that is no longer a direct child.

- [ ] **Step 2: Run the focused tests and confirm red**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter uia_contract
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter active_view
  ```

- [ ] **Step 3: Extend the Win32 and Shell captures**

  Return the confirmed ordered tab HWNDs with the existing active-tab and active-view result. Keep the existing PIDL-based filesystem-path route unchanged. Reject the full snapshot before returning any partial mapping when the order or join is invalid.

- [ ] **Step 4: Run full unit tests and commit**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  git add src/probe/win32_probe.* src/probe/shell_probe.* tests/uia_contract_tests.cpp tests/active_view_tests.cpp
  git commit -m "feat: capture ordered Explorer tab handles"
  ```

---

### Task 3: Make Browser Subscription Updates Transactional

**Files:**

- Modify: `src/probe/observer_runtime.h`
- Modify: `src/probe/observer_runtime.cpp`
- Modify: `tests/observer_runtime_tests.cpp`

**Exact transaction:**

1. Validate the complete desired tab set without mutating the live resource graph.
2. Create sinks and advise all additions while retaining their Shell browser objects.
3. Unadvise removals only after every addition succeeds.
4. If any addition fails, unadvise every addition completed in this transaction and keep the old graph.
5. If any removal fails, re-advise every removal already completed, unadvise every new addition, and keep the old graph.
6. Commit entries, registration IDs, and generations together only after all operations succeed.
7. If rollback itself fails, preserve the first operation failure as the primary status, record the rollback transport failure, stop observation, and let cleanup retain ownership of every still-live resource.

- [ ] **Step 1: Write failing transaction tests**

  Cover no change, batch add, batch remove, mixed add/remove, advise failure at each addition, unadvise failure at each removal, re-advise rollback failure, cleanup after a failed transaction, stable registration IDs, and exact generation behavior.

- [ ] **Step 2: Run the focused tests and confirm red**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter observer_shell_reconcile
  ```

- [ ] **Step 3: Implement the transaction around the existing resource graph**

  Reuse the current advise/unadvise operation table and ownership model. Remove the `resolve_registered` operation from required startup/runtime operations. Remove cookie-keyed registration maps. Keep lifecycle subscription setup and cleanup unchanged except where exact ownership fields move.

- [ ] **Step 4: Run full unit tests and commit**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  git add src/probe/observer_runtime.* tests/observer_runtime_tests.cpp
  git commit -m "feat: update tab subscriptions transactionally"
  ```

---

### Task 4: Reconcile the Full Current Set After Every Wake Signal

**Files:**

- Modify: `src/probe/observer_production_runtime.cpp`
- Modify: `src/probe/observer_runtime.h`
- Modify: `src/probe/observer_runtime.cpp`
- Modify: `src/probe/uia_observer_worker.h`
- Modify: `src/probe/uia_observer_worker.cpp`
- Modify: `tests/observer_runtime_tests.cpp`
- Modify: `tests/uia_observer_worker_tests.cpp`

**Accepted wake signals:**

| Signal | Callback payload retained | Required action after dequeue |
|---|---|---|
| `WindowRegistered` | Raw lifecycle cookie for diagnostics | Full Win32/Shell capture and reconciliation |
| `WindowRevoked` | Raw lifecycle cookie for diagnostics | Full Win32/Shell capture and reconciliation |
| UIA structure change | Exact sender top-level/tab scope and sequence | Full capture and reconciliation |
| UIA selection change | Exact sender top-level/tab scope and sequence | Full capture and active-view validation |
| `NavigateComplete2` | Canonical source identity, HWNDs, generation, sequence | Full capture, stale-generation rejection, and path refresh |

**Runtime rules:**

- The callback cookie is never passed to `FindWindowSW` and never used to select an added or removed entry.
- Each refresh enumerates the current exact Explorer top levels, captures their stable ordered tab sets, captures the Shell browser set, performs the exact bijection, applies the subscription transaction, then publishes the post-reconcile state.
- Repeated or coalesced signals may produce a stable delta; stable state is valid when the current mapping is exact.
- An event carrying a stale top-level or tab generation is recorded as stale and cannot change the active state.
- One reconciliation is in flight on the Shell STA. Additional signals remain queued and are processed in sequence; no concurrent graph mutation is allowed.

- [ ] **Step 1: Replace cookie-correlation tests with wake-and-reconcile tests**

  Cover lifecycle cookie values that have no Shell-object relationship, multiple changes before one signal, repeated stable signals, queued add/select/navigate/remove order, stale generation, window close, capture mismatch, transaction failure, and callback non-reentrancy.

- [ ] **Step 2: Run observer and UIA worker tests and confirm red**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter observer_runtime
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter uia_observer
  ```

- [ ] **Step 3: Implement the production wake-and-reconcile path**

  Delete `registered_shell_entries_`, the resolved-cookie cache, and their tombstone behavior. Keep raw lifecycle events in the event stream. Route all accepted wake signals through the same exact current-set capture and reconciliation path.

- [ ] **Step 4: Run full unit tests and commit**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  git add src/probe/observer_production_runtime.cpp src/probe/observer_runtime.* src/probe/uia_observer_worker.* tests/observer_runtime_tests.cpp tests/uia_observer_worker_tests.cpp
  git commit -m "fix: refresh Explorer tabs from current HWND state"
  ```

---

### Task 5: Revise Reporting and the Gate A Verdict

**Files:**

- Modify: `src/probe/probe_types.h`
- Modify: `src/probe/event_observer.h`
- Modify: `src/probe/event_observer.cpp`
- Modify: `src/probe/report_writer.h`
- Modify: `src/probe/report_writer.cpp`
- Modify: `src/probe/probe_runner.cpp`
- Modify: `tests/event_filter_tests.cpp`
- Modify: `tests/observer_runtime_contract_tests.cpp`

**Exact report contract:**

Each accepted event record retains its existing sequence, timestamp, kind, source HWNDs, generations, active view, and path fields, and adds the post-reconcile facts below:

| Key | Exact value |
|---|---|
| `reconcile.previous_tab_count` | Complete targeted tab count before the refresh |
| `reconcile.current_tab_count` | Complete targeted tab count after the refresh |
| `reconcile.added_tab_count` | Exact added identity count |
| `reconcile.removed_tab_count` | Exact removed identity count |
| `reconcile.retained_tab_count` | Exact retained identity count |
| `reconcile.active_shell_tab` | Exact active tab HWND after reconciliation |
| `lifecycle.cookie` | Raw cookie only for lifecycle records; absent for other event kinds |

- Do not emit a field that claims the cookie identifies a tab or top-level window.
- Gate A requires at least one proven tab addition, tab selection, navigation/path transition, tab removal, and controlled window removal, with `active_view_count=1` whenever a filesystem view is applicable.
- A lifecycle event may be stable because signals can be delayed or coalesced. The gate evaluates exact resulting state and the complete controlled action sequence, not one-cookie-to-one-object attribution.
- Any count inconsistency, stale state accepted as current, missing required transition, or callback/cleanup transport failure produces `Gate A: FAIL`.

- [ ] **Step 1: Write failing report and gate-evaluation tests**

  Cover exact key names, lifecycle-only cookie presence, absence of cookie-derived identity fields, stable lifecycle signals, coalesced batch delta, all required transitions, active-view cardinality, stale-event exclusion, and cleanup failure.

- [ ] **Step 2: Run the focused tests and confirm red**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Target WinExInfoTests
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter event_filter
  .\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter observer_contract
  ```

- [ ] **Step 3: Implement the exact report and verdict changes**

  Update only the named report contract. Remove pending-cookie attribution and any gate requirement built on it. Keep strict UTF-8, bounded output, event order, and existing exit-code behavior.

- [ ] **Step 4: Run full unit tests and commit**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  git add src/probe/probe_types.h src/probe/event_observer.* src/probe/report_writer.* src/probe/probe_runner.cpp tests/event_filter_tests.cpp tests/observer_runtime_contract_tests.cpp
  git commit -m "test: evaluate Gate A from reconciled tab state"
  ```

---

### Task 6: Run Revised Gate A and Record Evidence

**Files:**

- Create after execution: `docs/superpowers/evidence/2026-07-11-winexinfo-hwnd-gate-a.md`
- Modify only when verification exposes a real defect: files from Tasks 1 through 5

- [ ] **Step 1: Establish a clean safety baseline**

  Record the current WinExInfo/Explorer process and module state, exact Run-key value, exact service/task names, and WinExInfo-owned outbound connections using the same bounded checks as the approved feasibility plan. This task remains read-only outside the build tree and controlled Explorer UI actions.

- [ ] **Step 2: Run Debug verification**

  ```powershell
  pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
  .\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
  .\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe observe --duration-ms 45000
  ```

  During the observer window, use one controlled Explorer window and record state before every action: verify one tab, add a tab, select the other tab, open an existing child folder, navigate Back, close the added tab, and close the controlled window.

- [ ] **Step 3: Evaluate the exact Gate A acceptance criteria**

  Required PASS evidence:

  - Stable ordered tab HWND snapshots before and after each controlled action.
  - Exact targeted HWND-to-canonical-Shell-identity bijection at every accepted state.
  - Addition and removal subscription transactions complete without leaks or rollback failures.
  - Selection and navigation remap to exactly one applicable active filesystem view and the expected path transition.
  - Lifecycle cookies appear only as raw diagnostics and no cookie-attribution field exists.
  - Observer exits normally after 45 seconds; all tests pass; the safety baseline remains unchanged.

- [ ] **Step 4: Write evidence and apply the stop rule**

  Record exact commands, exit codes, test counts, controlled HWND/PID/TID values, ordered tab sets, bounded per-event reconciliation counts, active-view/path transitions, cleanup result, safety comparison, and exactly one final line: `Gate A: PASS` or `Gate A: FAIL`.

  If Gate A fails, commit the evidence and stop. Do not start Gate B. If Gate A passes, commit the evidence and request 형님's approval before authoring/executing the revised Gate B plan for same-thread tab HWND subclassing.

- [ ] **Step 5: Commit Gate A evidence**

  ```powershell
  git add docs/superpowers/evidence/2026-07-11-winexinfo-hwnd-gate-a.md
  git commit -m "test: record HWND identity Gate A evidence"
  git diff --check
  git status --short --branch
  ```

---

## Gate B Boundary After Gate A PASS

The approved design keeps Gate B as a separate implementation plan. That later plan must extend the existing versioned IPC and hook-lifecycle design with these exact additions:

- Host sends the desired ordered tab HWND set plus top-level and tab generations.
- The DLL pipe worker validates the message and posts an immutable update to the exact Explorer UI thread.
- The Explorer UI thread revalidates every HWND and installs/removes `SetWindowSubclass` using the same HWND, procedure, and subclass ID.
- Subclass callbacks report only destroy, geometry, and visibility facts; they perform no COM, UIA, pipe I/O, Git work, or blocking waits.
- The pane becomes visible only after the UI-thread subclass update is acknowledged.
- The 100-cycle Gate B cleanup proof includes tab subclasses, update messages, pane, pipe, workers, module disappearance, and zero handle/thread deltas.
- Real `explorer.exe` injection remains prohibited until revised Gate A and test-target Gate B both pass and 형님 explicitly approves the next gate.

## Coverage Check

| Approved design area | Covered here |
|---|---|
| HWND/canonical identity as primary tab identity | Tasks 1 and 2 |
| Cookies diagnostic only | Tasks 1, 4, and 5 |
| Complete-set reconciliation | Tasks 1 through 4 |
| Exact active tab/view/path | Tasks 2, 4, and 6 |
| Transactional browser subscriptions | Task 3 |
| Events as wake signals | Task 4 |
| Revised Gate A verdict and evidence | Tasks 5 and 6 |
| Same-thread DLL subclassing and IPC | Explicit Gate B boundary after Gate A PASS |

