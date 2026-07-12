# WinExInfo Install and Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a user-installable WinExInfo v1 with background startup, status reporting, logging, clean shutdown, uninstall, and final real-Explorer acceptance evidence.

**Architecture:** The same Host executable exposes exact operational commands. User-scoped install copies only Release Host/Hook and writes one exact HKCU Run value. A single-instance background process owns logs and Explorer sessions; status is read-only through a current-user control pipe; uninstall asks the Host to stop, verifies DLL cleanup, removes persistence/files/logs, and leaves no residue.

**Tech Stack:** C++20, Win32 Registry, named pipes, `%LOCALAPPDATA%`, CMake/Ninja, PowerShell packaging/E2E.

## Global Constraints

- Git-status E2E must pass before installation work starts.
- Install root is exactly `%LOCALAPPDATA%\WinExInfo`; log root is exactly `%LOCALAPPDATA%\WinExInfo\logs`.
- Run value is exactly `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WinExInfo` with data `"<install>\WinExInfoHost.exe" --background`.
- No service, scheduled task, elevation, machine-wide registry, network, updater, or alternate install path.
- `--uninstall` must remove everything WinExInfo created after cleanly detaching; failure reports residue and never claims success.

---

### Task 1: Operational CLI and Single-Instance Control

**Files:**
- Modify: `src/host/command_line.*`
- Create: `src/host/control_server.h`
- Create: `src/host/control_server.cpp`
- Create: `tests/operations_cli_tests.cpp`
- Modify: `src/host/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces exact `--install`, `--uninstall`, `--status`, `--background`; control requests `status` and `stop` on `\\.\pipe\WinExInfo.Control.v1.<SID>`.

- [ ] **Step 1: Write failing exact CLI/control tests**

Reject combined/extra/mixed-case commands. Cover second background instance, current-user ACL, wrong client PID/user, status while stopped/running/degraded, stop ACK, pipe disconnect, and 5000 ms timeout.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter operations
```

- [ ] **Step 3: Implement command dispatch and control server**

```cpp
enum class HostCommand {
    ProbeSnapshot, ProbeObserve, GateCPlace,
    Background, Install, Uninstall, Status
};
```

`--status` never starts Host or changes state. A second `--background` detects exact mutex `Local\WinExInfo.Host.v1`, reports the running instance, and exits 0.

- [ ] **Step 4: Run targeted and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter operations
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/host tests/operations_cli_tests.cpp
git commit -m "feat: add WinExInfo operational commands"
```

### Task 2: User-Scoped Install and Uninstall

**Files:**
- Create: `src/host/installer.h`
- Create: `src/host/installer.cpp`
- Create: `tests/installer_tests.cpp`
- Modify: `src/host/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `InstallForCurrentUser`, `UninstallForCurrentUser`, exact file/registry operations table.

- [ ] **Step 1: Write failing transactional installer tests**

Cover exact paths/value/data/type, source file identity, same-volume replacement, already-installed exact match, mismatched install rejection, partial-copy rollback, Run write failure rollback, running Host stop, DLL retained timeout, file-in-use failure, logs removal, and residue report.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter installer
```

- [ ] **Step 3: Implement current-user transaction**

```cpp
[[nodiscard]] Status InstallForCurrentUser(
    const InstallOperations&, InstallReport*);
[[nodiscard]] Status UninstallForCurrentUser(
    const InstallOperations&, UninstallReport*);
```

Copy exactly `WinExInfoHost.exe` and `WinExInfoHook.dll`; write `REG_SZ` only after both identities verify. Uninstall stops Host, proves Explorer module absence, removes Run first, then files/logs/directory, and enumerates exact residue.

- [ ] **Step 4: Run installer and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter installer
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/host tests/installer_tests.cpp
git commit -m "feat: install WinExInfo for current user"
```

### Task 3: Bounded Diagnostic Logging

**Files:**
- Create: `src/host/logger.h`
- Create: `src/host/logger.cpp`
- Create: `tests/logger_tests.cpp`
- Modify: `src/host/background_coordinator.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces UTF-8 logs `WinExInfo-YYYYMMDD.log`, maximum 5 files and 2 MiB each, no secrets or full environment.

- [ ] **Step 1: Write failing log tests**

Cover strict UTF-8, timestamp/sequence, exact error/HRESULT/Win32/PID/TID/HWND/generation fields, newline escaping, rotation, concurrent writes, disk failure, and secret/environment redaction.

- [ ] **Step 2: Run and verify red**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter logger
```

- [ ] **Step 3: Implement one serialized logger worker**

The logger owns file handles; callers enqueue bounded immutable records. Logging failure changes status to degraded but never blocks Explorer UI or triggers fallback behavior.

- [ ] **Step 4: Run targeted and full tests**

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter logger
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/host tests/logger_tests.cpp
git commit -m "feat: add bounded diagnostic logging"
```

### Task 4: Packaging, User Guide, and Final Acceptance

**Files:**
- Create: `scripts/package.ps1`
- Create: `scripts/run-final-e2e.ps1`
- Create: `README.md`
- Create: `docs/superpowers/evidence/2026-07-12-winexinfo-final-e2e.md`

- [ ] **Step 1: Implement deterministic Release package**

```powershell
pwsh -NoProfile -File .\scripts\package.ps1 -Configuration Release
```

Produce `out/package/WinExInfo/WinExInfoHost.exe`, `WinExInfoHook.dll`, and `README.md` only; verify x64, static CRT policy, CFG, exact names, and SHA-256 manifest printed to stdout but not added to runtime files.

- [ ] **Step 2: Write the user guide**

Document build, direct `--background`, `--install`, `--status`, `--uninstall`, expected pane text, supported exact OS build, logs, troubleshooting, and the fact that no network access occurs.

- [ ] **Step 3: Run fresh Debug/Release verification**

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Test
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Release -Iterations 100
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Release -Fault UnhookFailure
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Release
pwsh -NoProfile -File .\scripts\run-production-lifecycle.ps1 -Configuration Release
pwsh -NoProfile -File .\scripts\run-git-status-e2e.ps1 -Configuration Release
```

Expected: every command exits 0.

- [ ] **Step 4: Run install/restart/status/uninstall E2E**

```powershell
pwsh -NoProfile -File .\scripts\run-final-e2e.ps1 -Configuration Release
```

The harness installs, starts background Host, validates live Git transitions in multiple tabs/windows, restarts a controlled Explorer process, checks `--status`, stops/uninstalls, and requires exact zero residue for Run/service/task/process/DLL/install/log/network state.

Expected: `WINEXINFO_V1_PASS install=true startup=true git_refresh_max_ms<=1500 explorer_restart=true uninstall_residue=0 network_connections=0`.

- [ ] **Step 5: Record final evidence and commit**

```powershell
git add README.md scripts/package.ps1 scripts/run-final-e2e.ps1 docs/superpowers/evidence/2026-07-12-winexinfo-final-e2e.md
git commit -m "test: verify complete WinExInfo v1"
```

- [ ] **Step 6: Final repository verification**

```powershell
git diff --check
git status --short --branch
git ls-files out
```

Expected: clean worktree, no generated artifact tracked, and final evidence says `WINEXINFO_V1_PASS`.

