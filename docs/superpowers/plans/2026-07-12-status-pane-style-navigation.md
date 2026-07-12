# Status Pane Style and Navigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Git text visually match Explorer's status bar and reliably show, replace, or hide it when the active tab path changes.

**Architecture:** `WinExInfo.StatusPane` remains the uniquely tracked host, while a child `STATIC` control performs native text rendering. The Host emits an explicit pane state on every active-tab path state change; the DLL accepts it only for the exact current top-level and tab generations and preserves that visibility through later layout refreshes.

**Tech Stack:** C++20, Win32 controls/messages, Shell COM snapshots, named-pipe protocol v1.

## Global Constraints

- No custom text painting in Explorer.
- Git work remains outside Explorer UI threads.
- Empty pane text is an explicit hidden state.
- Stale HWND or generation updates never mutate the pane.
- Keep the running application path focused; use only targeted rendering/navigation checks.

---

### Task 1: Native Status-Bar Styling

**Files:**
- Modify: `src/hook/status_pane.cpp`
- Modify: `src/hook/status_pane.h`
- Test: `tests/status_render_tests.cpp`

**Interfaces:**
- Consumes: `WinExInfo.StatusPane` host HWND and its Explorer parent.
- Produces: `ApplyNativeStatusPaneStyle(HWND pane, HWND explorerStatusPeer)`.

- [ ] **Step 1: Add a focused style contract check**

Cover font inheritance, child-static sizing, non-activation, system colors, and theme/font refresh. The production shape is:

```cpp
struct NativeStatusPaneStyle final {
    HFONT font = nullptr;          // borrowed from Explorer
    COLORREF text_color = 0;
    HBRUSH background = nullptr;   // system-owned brush
};
```

- [ ] **Step 2: Implement the native child control**

Create one `STATIC` child with:

```cpp
WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS
```

Apply Explorer's nearest status-bar `WM_GETFONT` result using `WM_SETFONT`; fall back to `DEFAULT_GUI_FONT`. Handle `WM_CTLCOLORSTATIC` with `COLOR_WINDOWTEXT`, transparent text background, and `GetSysColorBrush(COLOR_WINDOW)`. Resize the child with an eight-pixel horizontal inset on `WM_SIZE`. Reapply style on `WM_THEMECHANGED`, `WM_SETTINGCHANGE`, `WM_DPICHANGED`, and `WM_FONTCHANGE`.

- [ ] **Step 3: Build the Release hook only**

Run:

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Target WinExInfoHook
```

Expected: `WinExInfoHook.dll` links successfully.

- [ ] **Step 4: Commit**

```powershell
git add src/hook/status_pane.cpp src/hook/status_pane.h tests/status_render_tests.cpp
git commit -m "feat: match Explorer status pane styling"
```

### Task 2: Explicit Navigation State

**Files:**
- Modify: `src/host/background_coordinator.cpp`
- Modify: `src/host/explorer_session.h`
- Modify: `src/host/explorer_session.cpp`
- Modify: `src/hook/runtime.cpp`
- Test: `tests/status_render_tests.cpp`

**Interfaces:**
- Consumes: exact active tab HWND/generation and `ActiveShellViewSnapshot::filesystem_path`.
- Produces: `PaneTextUpdate` with non-empty Git text or empty hidden state.

- [ ] **Step 1: Add focused state-transition checks**

Cover these exact sequences:

```text
Git A -> Git B       replace A with B
Git A -> non-Git     clear text and hide
non-Git -> Git A     show A
Git A(gen 4) -> stale Git B(gen 3)  ignore B
tab A -> tab B -> late A result     ignore late A
```

- [ ] **Step 2: Always populate active-tab identity**

For every valid window snapshot, set:

```cpp
sessionWindow.active_tab = activeShellTab;
sessionWindow.active_tab_generation = matchingDescriptor.tab_generation;
```

If the active view has no filesystem path or Git discovery fails, leave `display_text` and `tooltip_text` empty instead of omitting the pane update.

- [ ] **Step 3: Send every changed pane state**

`ExplorerSession::SendPaneText` must accept the empty/empty pair and encode the exact active tab identity. Deduplicate only an identical tuple:

```cpp
(top_level, top_generation, active_tab, tab_generation,
 display_text, tooltip_text)
```

- [ ] **Step 4: Preserve visibility in the DLL**

Add `bool pane_text_available` to each runtime window resource. For an accepted update:

```cpp
resources->pane_text_available = !update.display_text.empty();
SetWindowTextW(label, convertedText.c_str());
ShowWindow(resources->pane.hwnd,
           resources->pane_text_available ? SW_SHOWNOACTIVATE : SW_HIDE);
```

Every later placement operation must combine layout visibility with `pane_text_available`; a resize must not reshow a non-Git pane.

- [ ] **Step 5: Build Release Host and Hook**

Run:

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Target WinExInfoHook
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Target WinExInfoHost
```

Expected: both targets link successfully.

- [ ] **Step 6: Commit**

```powershell
git add src/host/background_coordinator.cpp src/host/explorer_session.h src/host/explorer_session.cpp src/hook/runtime.cpp tests/status_render_tests.cpp
git commit -m "feat: refresh pane for Explorer navigation"
```

### Task 3: Visible Navigation Check

**Files:**
- Modify only if a visible defect is found in Task 1 or Task 2 files.

**Interfaces:**
- Consumes: Release Host and Hook from Tasks 1-2.
- Produces: a running Explorer window whose visible status text follows navigation.

- [ ] **Step 1: Stop the current Host gracefully**

Signal `Local\WinExInfo.Host.Stop.v1.<exact-host-pid>`, wait for Host exit, and confirm `WinExInfoHook.dll` is absent before rebuilding or restarting.

- [ ] **Step 2: Start the Release app on one project Explorer window**

Open the controlled project worktree, start `WinExInfoHost.exe --background`, and confirm the process remains running.

- [ ] **Step 3: Inspect actual pixels and accessibility text**

Confirm the text is visible beside Explorer's item count, uses the same baseline and visual weight, and has no gray rectangle or focus border.

- [ ] **Step 4: Navigate through three paths**

Use one tab and navigate:

```text
WinExInfo Git worktree -> non-Git temp folder -> another Git repository
```

Expected: first text visible, then hidden, then replaced with the second repository's branch/count text without restarting Host.

- [ ] **Step 5: Leave the actual app running**

Bring the verified Explorer window to the foreground and report the exact visible text and Host PID.
