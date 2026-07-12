# WinExInfo Gate C Evidence

Gate C: FAIL

Captured on 2026-07-12 KST against the approved Windows 11 Explorer target.

## Decision

The fresh Debug live run at approved product head `4864b5a` proved exact
initial placement, resize reflow, and migration to the newly selected tab's
active `DUIViewWndClassName`. After closing the added tab and restoring the
original active tab, however, exact pane cardinality remained zero for the
entire bounded 3000 ms wait. Failure cleanup also left one exact Hook DLL
module in the now-windowless controlled Explorer process. Gate C is therefore
FAIL. Per the stop condition, Release was not executed and product
implementation must not continue from this gate.

No fallback selector, overlay, network access, persistence, service, scheduled
task, Run-key mutation, or existing Explorer-window targeting was used.

## Fresh run at approved head 4864b5a

Command:

```text
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Debug
```

Result: exit 1. Release was not run.

Controlled identity and before state:

```text
CONTROLLED hwnd=0x0000000000A50F76 pid=46216 tid=21276 class=CabinetWClass url=file:///D:/PROJECTS/WinExInfo
SAFETY stage=before explorer_windows=1 explorer_pids=1 run=0 service=0 task=0 processes=0 tcp=0
```

Initial exact active scope and geometry:

```text
active_tab_hwnd=0x0000000008B10FB8
pane_hwnd=0x0000000000031032
parent_hwnd=0x00000000000410B8
pid=46216 tid=21276 dpi=96
parent=179,282,1664,984 status=179,960,1664,984
left=186,960,254,984 right=1612,960,1660,984
pane=262,960,1604,984 expected=262,960,1604,984 overlap=false
```

After resize:

```text
active_tab_hwnd=0x0000000008B10FB8 parent_hwnd=0x00000000000410B8
parent=179,282,1504,894 status=179,870,1504,894
left=186,870,254,894 right=1452,870,1500,894
pane=262,870,1444,894 expected=262,870,1444,894 overlap=false
```

After adding and selecting the new tab, the product fix correctly migrated
the same pane HWND to the new active parent and recomputed the exact rectangle:

```text
active_tab_hwnd=0x000000000007107A
pane_hwnd=0x0000000000031032
parent_hwnd=0x0000000000210F1E
left=186,870,247,894 right=1452,870,1500,894
pane=255,870,1444,894 expected=255,870,1444,894 overlap=false
TAB_TRANSITION before=1 after_add=2 selected_original=true selected_added=true
```

After closing the added tab, the original native active scope returned but no
exact pane appeared within 3000 ms:

```text
active_tab_hwnd=0x0000000008B10FB8
active_view_hwnd=0x00000000000410B8
pane_cardinality=0
GATE_C_FAIL configuration=Debug reason=Pane_did_not_reach_exact_restored_state_within_3000ms
```

The harness closed only controlled HWND `0x0000000000A50F76`. Immediate after
counts were:

```text
SAFETY stage=after explorer_windows=1 explorer_pids=2 run=0 service=0 task=0 processes=0 tcp=0
```

A separate bounded five-second read-only cleanup check found:

```text
pid=15836 hook_count=0
pid=46216 hook_count=1
controlled_pid_exists=1 controlled_hook_count=1
```

The controlled top-level window was gone and no WinExInfo Host/Test process,
Run value, service, task, or owned TCP connection remained, but exact module
cleanup failed. The harness did not terminate Explorer.

A later final read-only check found that controlled PID 46216 had exited on
its own. Final state was one Shell window, only Explorer PID 15836 with Hook
count zero, and zero WinExInfo processes. This eventual process exit does not
replace the failed bounded module-cleanup result.

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

## Superseded diagnostic history

The earlier committed FAIL searched StatusBar from the Explorer top-level.
That scope contains both tab subtrees after a tab is added and could not
distinguish a product reflow failure from `status_cardinality=2`. It is
retained as diagnostic history but is superseded by the corrected run below.

The corrected harness mirrors production: it captures direct-child native
z-order before and after, selects the first visible exact direct-child
`ShellTabWindowClass`, requires exactly one visible descendant
`DUIViewWndClassName`, roots UIA at `AutomationElement.FromHandle(activeView)`,
and requires the sole pane's direct parent to equal that active view with the
same PID/TID and visible exact classes. Two corrected-run attempts stopped
inside `Add-Type` before opening Explorer because the helper temporarily used
a generic List without a compiler reference. The final helper uses no such
dependency; those attempts caused no live mutation.

## Superseded corrected-target run before head 4864b5a

The final run captured the Shell-window set before launch, started Explorer
with `/n, D:\PROJECTS\WinExInfo`, and accepted only one new HWND absent from
the before set whose exact Shell URL was
`file:///D:/PROJECTS/WinExInfo`. Existing Explorer windows were never eligible.

```text
CONTROLLED hwnd=0x00000000000E0FAE pid=43524 tid=46428 class=CabinetWClass url=file:///D:/PROJECTS/WinExInfo
```

The native pane contract required exact cardinality one, class
`WinExInfo.StatusPane`, text `WinExInfo Gate B`, owner PID/TID equal to the
controlled Explorer PID/TID, direct parent class `DUIViewWndClassName`, and an
exact rectangle computed from these UIA selectors:

| Element | Scope | FrameworkId | Control type | AutomationId | ClassName | Native HWND |
|---|---|---|---:|---|---|---:|
| Status bar | active DUIView descendants | `DirectUI` | StatusBar | `StatusBarModuleInner` | `StatusBarModuleInner` | 0 |
| Left group | StatusBar direct children | any | Group | `System.StatusBarViewItemCount` | any | 0 |
| Right group | StatusBar direct children | any | Group | `ViewButtonsGroup` | any | 0 |
| TabView | Explorer descendants | `XAML` | 50018 / Tab | `TabView` | `Microsoft.UI.Xaml.Controls.TabView` | evidence only |
| TabListView | TabView direct children | `XAML` | List | `TabListView` | `ListView` | evidence only |

Tab items were restricted to direct TabListView children with FrameworkId
`XAML`, ControlType `TabItem`, and ClassName `ListViewItem`. The harness used
only the exact `AddButton`, `CloseButton`, `InvokePattern`, and
`SelectionItemPattern`; names and titles were not lookup keys.

## Superseded exact geometry evidence

All rectangles are screen coordinates `left,top,right,bottom`. DPI was 96.

Initial state:

```text
active_tab_hwnd=0x000000000002103E
pane_hwnd=0x00000000002E0F44
parent_hwnd=0x0000000000460F6C
pane_pid=43524 pane_tid=46428
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
new direct tab items, the corrected active-scope diagnostic remained stable
through the 3000 ms deadline:

```text
active_tab_hwnd=0x0000000000D80D9A
active_view_hwnd=0x0000000000AF0DAC
pane_parent_hwnd=0x0000000000460F6C
active_parent_or_identity_mismatch
```

The pane parent is the exact old view observed in initial/resized state, not
the new active view. Active ancestry/visibility therefore failed and no stale
hidden-tab pane can satisfy the harness. No PASS record is valid.

## Superseded cleanup and safety state

The final Debug run's before counts were:

```text
explorer_windows=1 explorer_pids=1 run=0 service=0 task=0 processes=0 tcp=0
```

Failure cleanup restored the original window placement, removed the added
tab when present, stopped the still-running bounded Host if necessary, and
posted `WM_CLOSE` only to controlled HWND `0x00000000000E0FAE`. A subsequent
read-only safety capture returned:

```text
explorer_windows=1 explorer_pids=2 winexinfo_processes=0 tcp=0 run=0 service=0 task=0
pid=15836 hook_count=0
pid=43524 hook_count=0
```

The controlled top-level HWND was absent from the Shell window set. Explorer
PID 43524 remained as an Explorer process, but had zero exact
`WinExInfoHook.dll` modules and no controlled top-level window. The before and
after Explorer window counts matched; the separate controlled Explorer
process remained after its only window closed, so Explorer process count was
1 before and 2 after. The harness did not terminate Explorer. All remaining
Explorer PIDs had zero Hook DLL count, and exact
Run/service/task/WinExInfo-process/TCP counts were zero.

## Required result

```text
GATE_C_FAIL configuration=Debug reason=Pane_did_not_reach_exact_restored_state_within_3000ms._diagnostic=pane_cardinality=0 cleanup=false
```

Gate C: FAIL. Release is untested and downstream product work is blocked.
