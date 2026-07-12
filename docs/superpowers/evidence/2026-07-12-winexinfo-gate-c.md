# WinExInfo Gate C Evidence

Gate C: FAIL

Captured on 2026-07-12 KST against the approved Windows 11 Explorer target.

## Decision

The Debug live run proved exact initial placement and exact resize reflow, but
the controlled pane did not reach a valid exact layout within the bounded
3000 ms wait after adding and switching to a second Explorer tab. Gate C is
therefore FAIL. Per the stop condition, Release was not executed and product
implementation must not continue from this gate.

No fallback selector, overlay, network access, persistence, service, scheduled
task, Run-key mutation, or existing Explorer-window targeting was used.

## Commands and exits

Release was built before the live runs:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release
```

Result: exit 0.

The live Debug command was:

```text
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Debug
```

Result: exit 1 at the exact post-tab-switch layout deadline.

The Release live command was deliberately not run after Debug failed.

Three earlier Debug invocations stopped inside `Add-Type` before opening an
Explorer window or starting the Host. They identified missing compile-time
assembly references in the harness and caused no live mutation. A later live
harness attempt reached exact initial and resized placement but found the
harness's TabView selector used `ControlType.Pane`; the plan requires exact
control type 50018 (`ControlType.Tab`). Its `finally` cleanup stopped the Host,
restored the controlled window, and closed only that controlled HWND. The
selector was corrected before the final Debug result recorded below.

## Controlled target and selector

The final run captured the Shell-window set before launch, started Explorer
with `/n, D:\PROJECTS\WinExInfo`, and accepted only one new HWND absent from
the before set whose exact Shell URL was
`file:///D:/PROJECTS/WinExInfo`. Existing Explorer windows were never eligible.

```text
CONTROLLED hwnd=0x0000000004AD0C88 pid=36932 tid=31356 class=CabinetWClass url=file:///D:/PROJECTS/WinExInfo
```

The native pane contract required exact cardinality one, class
`WinExInfo.StatusPane`, text `WinExInfo Gate B`, owner PID/TID equal to the
controlled Explorer PID/TID, direct parent class `DUIViewWndClassName`, and an
exact rectangle computed from these UIA selectors:

| Element | Scope | FrameworkId | Control type | AutomationId | ClassName | Native HWND |
|---|---|---|---:|---|---|---:|
| Status bar | Explorer descendants | `DirectUI` | StatusBar | `StatusBarModuleInner` | `StatusBarModuleInner` | 0 |
| Left group | StatusBar direct children | any | Group | `System.StatusBarViewItemCount` | any | 0 |
| Right group | StatusBar direct children | any | Group | `ViewButtonsGroup` | any | 0 |
| TabView | Explorer descendants | `XAML` | 50018 / Tab | `TabView` | `Microsoft.UI.Xaml.Controls.TabView` | evidence only |
| TabListView | TabView direct children | `XAML` | List | `TabListView` | `ListView` | evidence only |

Tab items were restricted to direct TabListView children with FrameworkId
`XAML`, ControlType `TabItem`, and ClassName `ListViewItem`. The harness used
only the exact `AddButton`, `CloseButton`, `InvokePattern`, and
`SelectionItemPattern`; names and titles were not lookup keys.

## Exact geometry evidence

All rectangles are screen coordinates `left,top,right,bottom`. DPI was 96.

Initial state:

```text
pane_hwnd=0x000000000001105A
parent_hwnd=0x0000000000270FBA
pane_pid=36932 pane_tid=31356
pane_class=WinExInfo.StatusPane pane_text=WinExInfo Gate B
parent_class=DUIViewWndClassName
parent=179,282,1664,984
status=179,960,1664,984
left=186,960,254,984
right=1612,960,1660,984
pane=262,960,1604,984
expected=262,960,1604,984
overlap=false
```

After resizing the controlled window by -160 by -90 pixels:

```text
parent=179,282,1504,894
status=179,870,1504,894
left=186,870,254,894
right=1452,870,1500,894
pane=262,870,1444,894
expected=262,870,1444,894
overlap=false
```

The pane HWND, parent HWND, PID, TID, class, text, DPI, exact rectangle, and
no-overlap checks all remained exact through this resize transition.

After the harness invoked the exact AddButton and selected the original and
new direct tab items, it could not capture a valid exact pane/layout within
3000 ms:

```text
Pane did not reach exact state after tab switch within 3000ms.
```

Because the post-switch rectangle and ownership contract was not proven, no
PASS record is valid.

## Cleanup and safety state

The final Debug run's before counts were:

```text
explorer_windows=1 explorer_pids=2 run=0 service=0 task=0 processes=0 tcp=0
```

Failure cleanup restored the original window placement, removed the added
tab when present, stopped the still-running bounded Host if necessary, and
posted `WM_CLOSE` only to controlled HWND `0x0000000004AD0C88`. A subsequent
read-only safety capture returned:

```text
explorer_windows=1 explorer_pids=2 winexinfo_processes=0 tcp=0 run=0 service=0 task=0
pid=15836 hook_count=0
pid=36932 hook_count=0
```

The controlled top-level HWND was absent from the Shell window set. Explorer
PID 36932 remained as an Explorer process, but had zero exact
`WinExInfoHook.dll` modules and no controlled top-level window. The before and
after Explorer window/process counts matched, all remaining Explorer PIDs had
zero Hook DLL count, and exact Run/service/task/WinExInfo-process/TCP counts
were zero.

## Required result

```text
GATE_C_FAIL configuration=Debug reason=pane_not_exact_after_tab_switch
```

Gate C: FAIL. Release is untested and downstream product work is blocked.
