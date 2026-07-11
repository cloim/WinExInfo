# WinExInfo Gate A Evidence

Gate A: FAIL

Captured on 2026-07-11 KST against the approved
`10.0.26100.8655` target.

## Decision

The current Windows 11 Explorer emits `WindowRegistered(cookie)`, but the
same cookie cannot be resolved to an exact Shell `IDispatch` with
`IShellWindows::FindWindowSW` on this target. The final exact lookup returned
`S_FALSE` (`window not found`). Without an exact cookie-to-object result, a
new live Shell-set entry cannot safely be attributed to that cookie.

The implementation now fails at `lifecycle.resolve_registered` instead of
binding the cookie to an unrelated single live delta. This is the required
exact-contract behavior. Per the plan stop condition, Tasks 6 through 9 must
not start after this Gate A failure.

## Invalidated earlier result

An earlier controlled run reported Gate A PASS and all five event kinds. That
run treated `FindWindowSW == S_FALSE` as transitional and assigned the sole
new Shell-set entry to the callback cookie.

Review found two indistinguishable histories:

1. cookie A is registered and the sole new live entry is A;
2. A is registered and revoked, then B is registered before A is processed,
   so the sole new live entry is B.

The earlier algorithm silently bound B to cookie A in the second history.
Its PASS result is therefore invalid and is superseded by this evidence.

## Current target preflight and selector evidence

The final read-only snapshot was rerun after the exact-correlation change:

```text
.\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
```

Result: exit 0, empty standard error, `result=pass`, and `error_code=OK`.

| Window | PID / TID | Active views | Shell-tab matches | Shell entries |
|---|---|---:|---:|---:|
| `0x0000000000020BB6` | `8312 / 36516` | 1 | 1 | 7 |
| `0x0000000003870DA6` | `15356 / 43988` | 1 | 1 | 1 |

Both windows passed the exact OS version, file identity, cache-only catalog
signature, principal, native AMD64 architecture, `explorer.exe`,
`ExplorerFrame.dll`, `shell32.dll`, and mitigation checks. Every numeric fixed
version was `10.0.26100.8655`.

Each window returned cardinality 1 for every required selector:

| Element | Scope | FrameworkId | Control type | AutomationId | ClassName |
|---|---|---|---:|---|---|
| Status bar | active DUIView subtree | `DirectUI` | 50017 | `StatusBarModuleInner` | `StatusBarModuleInner` |
| Left group | StatusBar direct children | `DirectUI` | 50026 | `System.StatusBarViewItemCount` | `Element` |
| Right group | StatusBar direct children | `DirectUI` | 50026 | `ViewButtonsGroup` | `SelectorNoDefault` |
| TabView | Explorer top-level subtree | `XAML` | 50018 | `TabView` | `Microsoft.UI.Xaml.Controls.TabView` |
| TabListView | TabView direct children | `XAML` | 50008 | `TabListView` | `ListView` |

The bounded snapshot summary is stored in
[`2026-07-11-gate-a-final-snapshot-summary.txt`](2026-07-11-gate-a-final-snapshot-summary.txt).

## Controlled single-tab and multi-tab selector state

An additional bounded read-only run captured the same controlled top-level in
both states. Each snapshot returned exit 0, `result=pass`, `error_code=OK`,
all target preflight rows `pass`, and cardinality 1 for all five selectors:

| State | Top-level HWND | PID / TID | Shell entries | Shell-tab matches | Active views |
|---|---|---|---:|---:|---:|
| Single tab | `0x000000000A38074E` | `54128 / 27020` | 1 | 1 | 1 |
| Two tabs | `0x000000000A38074E` | `54128 / 27020` | 2 | 1 | 1 |

Both controlled snapshots used the exact scopes shown in the selector table:
active DUIView subtree for StatusBar, StatusBar direct children for its two
groups, Explorer top-level subtree for TabView, and TabView direct children
for TabListView. The action runner then restricted tab candidates to direct
TabListView children with FrameworkId `XAML`, ControlType `TabItem`, and
ClassName `ListViewItem`. Selection used only `SelectionItemPattern`; tab
names were evidence only and never lookup keys. `AddButton` was an exact
Explorer top-level subtree match, and `CloseButton` was an exact selected-tab
subtree match; both required FrameworkId `XAML`, ControlType `Button`,
ClassName `Button`, and cardinality 1.

The controlled state snapshots and bounded action output are stored in
[`2026-07-11-gate-a-controlled-snapshots.txt`](2026-07-11-gate-a-controlled-snapshots.txt).

The separate final Gate-failure action output is stored in
[`2026-07-11-gate-a-final-action.txt`](2026-07-11-gate-a-final-action.txt).

## Exact resolver contract

The retained production contract is:

- `FindWindowSW` receives the exact callback cookie as `VT_I4`;
- `pvarLocRoot` is a non-null `VT_EMPTY` variant;
- `swClass` is exactly `SWC_EXPLORER`;
- flags are exactly
  `SWFO_COOKIEPASSED | SWFO_NEEDDISPATCH | SWFO_INCLUDEPENDING`;
- only `S_OK`, a non-null `IDispatch`, a canonical `IUnknown`, and an exact
  identity match in the fresh Shell capture may commit a registration;
- `S_FALSE`, `E_PENDING`, and `E_NOINTERFACE` remain explicit Contract
  failures; no live-entry, title, path, URL, Name, legacy-key, or alternate
  class fallback is used.

Microsoft documents `S_FALSE` as “matching window not found” and `E_PENDING`
as “matching window found but pending open” when `SWFO_INCLUDEPENDING` is set:

- <https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-findwindowsw>
- <https://learn.microsoft.com/en-us/windows/win32/api/exdisp/ne-exdisp-shellwindowfindwindowoptions>
- <https://learn.microsoft.com/en-us/windows/win32/shell/dshellwindowsevents-windowregistered>

## Automated verification

Command:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Result: exit 0; 237 tests passed, 0 failed; CTest 1/1 passed.

The focused regression tests prove that:

- the resolver uses the exact cookie/root/class/flag contract;
- deferred or missing cookie results are not retried into a guessed live
  entry;
- registration correlation rejects identity `0` even when the physical set
  contains exactly one new entry;
- the production resolver retains the canonical COM identity and preserves
  Contract versus Transport failure origin.

## Final controlled 45-second command

The observer was started with the plan's exact duration option while a
separate action runner opened one controlled Explorer window, added and
selected a tab, navigated to `docs`, went Back, selected and closed the added
tab, and closed the controlled window.

```text
.\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe observe --duration-ms 45000
```

Action result: exit 0 and `ACTIONS_COMPLETE=OK`.

| Millisecond | Action |
|---:|---|
| 3,406 | Opened controlled Explorer window `HWND=14225546` |
| 4,801 | Confirmed one exact TabListView child |
| 10,696 | Added a tab and confirmed two exact children |
| 14,474 | Selected the original tab |
| 14,769 | Navigated to `docs` |
| 14,988 | Navigated Back |
| 17,246 | Selected the added tab |
| 19,417 | Closed the added tab and confirmed one child remained |
| 19,420 | Sent the controlled window close request |

Host result: exit 1, empty standard error, and the exact Contract failure:

```text
result=fail
observe.duration_ms=45000
observe.event_count=0
observe.runtime.error_code=ACTIVE_VIEW_CONTRACT_MISMATCH
observe.runtime.hresult=1
observe.runtime.stage=lifecycle.resolve_registered
observe.runtime.win32=0
observe.cleanup.error_code=OK
observe.cleanup.hresult=0
observe.cleanup.win32=0
error_code=ACTIVE_VIEW_CONTRACT_MISMATCH
```

`HRESULT 1` is `S_FALSE`. No Shell identity, browser connection point,
reducer entry, or cookie map entry was committed before this failure.
The exact Host output is stored in
[`2026-07-11-gate-a-final-observe.txt`](2026-07-11-gate-a-final-observe.txt).

## Bounded diagnostics

Temporary read-only diagnostics were run and then reverted:

- exact Explorer class with flags including pending: `S_FALSE`;
- one immediate same-cookie check: `S_FALSE`;
- one same-cookie check after 250 ms: `S_FALSE`;
- one same-cookie check after 3 seconds: `S_FALSE`;
- null root form: rejected with HRESULT/Win32 error 1780 on this target;
- `SWC_BROWSER`: `S_FALSE`.

These checks did not produce an exact cookie-to-dispatch mapping. Repeated
polling or choosing among alternate classes is not part of the retained
implementation.

## Safety state after the final run

The corrected Task 3 baseline recorded Hook DLL count 0 in Explorer PIDs
`8312` and `15356`; the exact Run value, service, and scheduled task were
absent; and WinExInfo process and owned TCP connection counts were 0.

The state after all final observation and controlled-snapshot runs was:

- remaining Explorer PIDs `8312` and `15356`:
  `WinExInfoHook.dll` count 0 for each;
- controlled Explorer PIDs `12096` and `54128`: no longer running after their
  windows were closed;
- exact Run value
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WinExInfo`: absent;
- exact service name `WinExInfo`: absent;
- exact scheduled task name `WinExInfo`: absent;
- `WinExInfoHost.exe` and `WinExInfoTests.exe` process count: 0;
- WinExInfo-owned TCP connection count: 0.

The controlled window was closed, cleanup returned `OK`, and no Explorer
injection, persistence, remaining WinExInfo process, or owned network
connection was introduced.
