# WinExInfo Git Status Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compute offline Git status for each active filesystem tab, refresh it within 1.5 seconds of relevant changes, and render it in the Explorer-owned pane.

**Architecture:** The Host owns repository discovery, hardened Git child processes, porcelain-v2 parsing, caching, and directory watchers. Tab generations request status asynchronously; only the newest matching generation is sent to the DLL. The DLL renders immutable text and never invokes Git or waits for Host work on Explorer UI threads.

**Tech Stack:** C++20, Git for Windows exact executable, `CreateProcessW`, Job Objects, overlapped pipes, porcelain v2 `-z`, `ReadDirectoryChangesW`, named-pipe protocol v1.

## Global Constraints

- Require lifecycle evidence `LIFECYCLE_PASS` before this plan starts.
- Execute only `C:\Program Files\Git\cmd\git.exe`; verify the opened executable file identity before every spawned process family.
- Never run fetch or any network command; ahead/behind uses only the locally stored upstream ref.
- No shell command strings, `cmd.exe`, PowerShell, PATH lookup, legacy porcelain, localized parsing, or fallback key names.
- Git and watcher work never runs on Explorer UI threads.

---

### Task 1: Hardened Git Process Runner

**Files:**
- Create: `src/git/git_runner.h`
- Create: `src/git/git_runner.cpp`
- Create: `tests/git_runner_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `GitCommand`, `GitRunResult`, `RunGitCommand` with 5000 ms timeout, 4 MiB stdout/stderr limits, Job Object tree termination.

- [ ] **Step 1: Write failing quoting/security/timeout tests**

```cpp
struct GitCommand final {
    std::filesystem::path working_directory;
    std::vector<std::wstring> arguments;
};
struct GitRunResult final {
    DWORD exit_code;
    std::vector<std::uint8_t> stdout_bytes;
    std::vector<std::uint8_t> stderr_bytes;
};
```

Cover spaces/quotes/backslashes, empty arguments, exact executable identity mismatch, inherited handle rejection, timeout, output cap, child-tree kill, cancellation, and no network-capable argument family.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter git_runner
```

- [ ] **Step 3: Implement `CreateProcessW` runner**

Use an explicit application path, Windows quoting per `CommandLineToArgvW` inverse rules, restricted inherited pipe handles, `CREATE_NO_WINDOW`, one kill-on-close Job Object, overlapped reads, and exact timeout/cap errors.

- [ ] **Step 4: Run targeted and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter git_runner
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/git tests/git_runner_tests.cpp
git commit -m "feat: add hardened Git process runner"
```

### Task 2: Repository Discovery and Porcelain v2 Parser

**Files:**
- Create: `src/git/repository_status.h`
- Create: `src/git/repository_status.cpp`
- Create: `tests/repository_status_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `RepositoryStatus`, `DiscoverRepository`, `ParsePorcelainV2Z`, `FormatRepositoryStatus`.

- [ ] **Step 1: Write failing parser fixtures/tests**

```cpp
struct RepositoryStatus final {
    std::filesystem::path root;
    std::string branch;
    std::uint32_t staged, unstaged, untracked, conflicts;
    std::int32_t ahead, behind;
    bool detached, unborn, upstream_available;
};
```

Create temporary repositories covering clean/dirty/staged/untracked/conflict, spaces/non-ASCII, rename/copy, detached, unborn, no upstream, local bare upstream, linked worktree, `.git` file, nested repository, malformed/truncated/duplicate headers, and NUL records.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter repository_status
```

- [ ] **Step 3: Implement exact commands and parser**

Run `-C <folder> rev-parse --show-toplevel --absolute-git-dir --git-common-dir` and then `-C <root> status --porcelain=v2 --branch -z --untracked-files=all`. Require exact known record prefixes; malformed data returns `GIT_STATUS_PROTOCOL_ERROR` without legacy parsing.

- [ ] **Step 4: Run repository and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter repository_status
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/git tests/repository_status_tests.cpp
git commit -m "feat: parse offline Git repository status"
```

### Task 3: Cache, Watchers, and Generation Cancellation

**Files:**
- Create: `src/git/status_service.h`
- Create: `src/git/status_service.cpp`
- Create: `src/git/repository_watcher.h`
- Create: `src/git/repository_watcher.cpp`
- Create: `tests/status_service_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `StatusRequest{tab_hwnd,tab_generation,path}`, `StatusSnapshot`, `StatusService::Request/Cancel/Stop`.

- [ ] **Step 1: Write failing scheduling/watcher tests**

Cover 250 ms request debounce, newest-generation-only delivery, 30-second unchanged cache, repository/common Git directory watches, worktree file watches, overflow full refresh, deletion/recreation, shared repo watchers, 1.5-second delivery bound, cancellation, and stop cleanup.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter status_service
```

- [ ] **Step 3: Implement bounded asynchronous service**

```cpp
struct StatusRequest final {
    std::uint64_t tab_hwnd;
    std::uint64_t tab_generation;
    std::filesystem::path folder;
};
```

One service worker queue owns Git jobs; watchers only enqueue invalidations. Results are delivered only when HWND and generation still match the newest request.

- [ ] **Step 4: Run stress and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter status_service
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/git tests/status_service_tests.cpp
git commit -m "feat: refresh Git status from filesystem changes"
```

### Task 4: Pane Text Protocol and Rendering

**Files:**
- Modify: `src/ipc/protocol.*`
- Modify: `src/hook/status_pane.*`
- Modify: `src/hook/runtime.cpp`
- Modify: `src/host/background_coordinator.cpp`
- Create: `tests/status_render_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `StatusSnapshot`, pane text update protocol.
- Produces exact display `branch  ↑A ↓B  +S ~U ?N !C`, clean `branch  ✓`, hidden outside Git/non-filesystem, tooltip with full text and root.

- [ ] **Step 1: Write failing formatting/render tests**

Cover clean, each count, combined counts, detached/unborn/no-upstream, non-ASCII branch, width ellipsis, tooltip, theme/high contrast/DPI, stale text generation, hidden state, and no focus activation.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter status_render
```

- [ ] **Step 3: Implement immutable text update path**

Use `WM_SETTEXT`/custom paint only on the owning Explorer UI thread, system colors/font, single-line ellipsis, and exact generation acknowledgment. The Host hides on any contract or status error and logs the exact code.

- [ ] **Step 4: Run all Git, IPC, rendering, and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter git
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter status
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/git src/host src/hook src/ipc tests/status_render_tests.cpp
git commit -m "feat: render live Git status in Explorer"
```

### Task 5: Live Offline Git E2E

**Files:**
- Create: `scripts/run-git-status-e2e.ps1`
- Create: `docs/superpowers/evidence/2026-07-12-winexinfo-git-status-e2e.md`

- [ ] **Step 1: Build a disposable local repository harness**

Create a local repository and local bare upstream under `%TEMP%`, start Host, open one controlled Explorer window, and drive clean/staged/unstaged/untracked/conflict/ahead/behind/non-repo transitions without network access.

- [ ] **Step 2: Run Debug and Release E2E**

```powershell
pwsh -NoProfile -File .\scripts\run-git-status-e2e.ps1 -Configuration Debug
pwsh -NoProfile -File .\scripts\run-git-status-e2e.ps1 -Configuration Release
```

Expected: `GIT_STATUS_E2E_PASS refresh_max_ms<=1500 network_connections=0 cleanup=true`.

- [ ] **Step 3: Record evidence and commit**

```powershell
git add scripts/run-git-status-e2e.ps1 docs/superpowers/evidence/2026-07-12-winexinfo-git-status-e2e.md
git commit -m "test: verify live offline Git status"
```

