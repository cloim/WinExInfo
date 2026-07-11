# WinExInfo HWND Gate A Evidence — 2026-07-11

## Verdict

Gate A passed with a real Windows 11 Explorer window. The observer tracked tab
addition, selection remap, tab removal, filesystem navigation, and top-level
window removal by the reconciled HWND tab set. Runtime and cleanup both ended
with `OK`.

## Build and unit verification

Build command:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-x64-debug --config Debug --target WinExInfoTests'
```

Unit command:

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit
```

Result:

```text
[SUMMARY] passed=243 failed=0
exit_code=0
```

The FIFO worker timing test failed once during an earlier full run, then passed
five consecutive isolated runs and the final full run.

## Controlled Explorer identity

The following snapshot was captured before the observe run. The same Explorer
process and top-level HWND were then used for all Gate A actions.

```text
pid=58480
thread_id=48028
top_level_hwnd=0x0000000007221A0E
initial_shell_tab_hwnd=0x0000000001780FB6
initial_active_view_hwnd=0x0000000008EF117E
initial_filesystem_path=D:\PROJECTS
snapshot_result=pass
snapshot_error_code=OK
```

Snapshot command:

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
```

## Controlled action sequence

The controlled window was prepared at `D:\PROJECTS\WinExInfo`, then the
observer ran while these UI actions were performed:

1. Add a new Explorer tab.
2. Confirm both `WinExInfo` and `내 PC` tabs.
3. Close the added `내 PC` tab.
4. Navigate Back from `D:\PROJECTS\WinExInfo` to `D:\PROJECTS`.
5. Close the controlled Explorer window.

Each action used the current accessibility snapshot before the next action.
The final window state was `PID=58480`, `HWND=0`, with an empty title.

Observe command:

```powershell
.\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe observe --duration-ms 60000
```

## Observe result

```text
result=pass
observe.event_count=14
observe.ignored_event_count=34
observe.late_event_count=0
observe.kind.window_registered_count=1
observe.kind.window_revoked_count=1
observe.kind.navigate_complete2_count=1
observe.kind.tab_selected_count=4
observe.kind.tab_structure_changed_count=7
observe.runtime.error_code=OK
observe.runtime.hresult=0
observe.runtime.win32=0
observe.runtime.stage=lifecycle.reconciled
observe.cleanup.error_code=OK
observe.cleanup.hresult=0
observe.cleanup.win32=0
error_code=OK
exit_code=0
```

Key reconciliations:

```text
event.01 kind=window_registered transition=reconciled
event.01 top_level=0x0000000007221A0E tabs=9->10 added=1 removed=0

event.08 kind=tab_selected transition=remapped active_view_count=1
event.08 top_level=0x0000000007221A0E tabs=10->9 added=0 removed=1
event.08 current_filesystem_path=D:\PROJECTS\WinExInfo

event.11 kind=navigate_complete2 transition=remapped active_view_count=1
event.11 previous_filesystem_path=D:\PROJECTS\WinExInfo
event.11 current_filesystem_path=D:\PROJECTS

event.14 kind=window_revoked transition=reconciled
event.14 top_level=0x0000000007221A0E tabs=9->8 added=0 removed=1
```

## Live defects found and closed

The real run exposed three short Explorer transition states. Each received a
failing regression test before the implementation change:

- UIA callbacks from already replaced elements returned
  `UIA_E_ELEMENTNOTAVAILABLE` or exact `S_FALSE`; these stale callbacks are now
  ignored while actual transport failures remain fatal.
- After tab removal, ShellWindows briefly retained metadata for the removed tab;
  the current ordered HWND set remains authoritative and stale extra metadata is
  excluded.
- During top-level teardown, the window briefly remained enumerable after its
  child tab HWND disappeared; exact `S_FALSE + ERROR_INVALID_DATA` refreshes are
  treated as an early wake and the next signal performs reconciliation.

## Safety comparison

The safety probe matched before and after the controlled run:

```json
{"run_value":"<absent>","service_count":0,"task_count":0,"winex_process_count":0,"explorer_module_count":0,"outbound_connection_count":0}
```

No Explorer injection, service, scheduled task, Run-key persistence, resident
WinExInfo process, or outbound WinExInfo connection remained.

Gate A: PASS
