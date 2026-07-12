# WinExInfo Gate C Evidence

Gate C: FAIL

Captured on 2026-07-12 KST against the approved Windows 11 Explorer target.

Product under test: product head `3612965`. The harness evolved in separate
evidence commits; the latest live result below used the strict harness state
committed at `fa30256`. The additional fail-closed safety hardening documented
next was made afterward and has not been used for a live rerun.

## Decision

The strict harness review invalidated the prior PASS as insufficient evidence.
A fresh strict Debug run at head `3612965` passed all lifecycle, observed
selection, continuity, cleanup, and controlled-window checks. After rebuilding
Release, the strict Release run failed before Host launch because the new
controlled window appeared between its two stable Shell samples. No Release
product result was obtained. Per the stop condition, Gate C is FAIL for this
run. Earlier commands and outcomes are retained below as explicitly
superseded diagnostic history.

No fallback selector, overlay, persistence, service, scheduled task, Run-key
mutation, existing Explorer-window targeting, or network command was used.

The harness contains no network command. TCP count zero was observed only at
the recorded before/after safety samples; it is not treated as continuous
network monitoring.

## Final strict run

### Strict Debug: PASS

```text
CONTROLLED hwnd=0x0000000002990372 pid=39008 tid=25744 class=CabinetWClass url=file:///D:/PROJECTS/WinExInfo
SAFETY stage=before explorer_windows=1 explorer_pids=1 run=0 service=0 task=0 processes=0 tcp=0
```

Canonical continuity and exact rectangles:

```text
initial      active_tab=0x00000000000810A8 pane=0x0000000001000D78 parent=0x0000000000C90BB8 rect=262,960,1604,984
resized      active_tab=0x00000000000810A8 pane=0x0000000001000D78 parent=0x0000000000C90BB8 rect=262,870,1444,894
tab_switched active_tab=0x00000000001D0E08 pane=0x0000000001000D78 parent=0x00000000000B1004 rect=255,870,1444,894
restored     active_tab=0x00000000000810A8 pane=0x0000000001000D78 parent=0x0000000000C90BB8 rect=262,960,1604,984
```

Every rectangle equaled its expected rectangle and overlap was false. Resize
retained pane/tab/parent and changed the rectangle. Switching retained pane
HWND while changing active tab and parent. Restore retained pane HWND,
returned to the exact original tab/parent, and returned pane and expected
rectangles to the initial values. Exact UIA runtime identities and observed
selection states were:

```text
original_identity=42.2953076.4.11
added_identity=42.2953076.4.101
selected_original=true selected_added=true
```

The restored direct tab count was exactly one and its runtime identity equaled
the original identity. The controlled Shell identity was revalidated before
close.

```text
HOST_STDOUT TARGET_ACCEPTED protocol=1 pid=39008 tid=25744 hwnd=0x0000000002990372
CLEANUP pane_absent=true exact_module_count=0 host_exit=0
SAFETY stage=after explorer_windows=1 explorer_pids=2 run=0 service=0 task=0 processes=0 tcp=0
GATE_C_PASS configuration=Debug pane_owner=explorer overlap=false cleanup=true
```

### Strict Release: FAIL before Host launch

Release build command returned exit 0:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release
```

The live command returned exit 1 before Host launch:

```text
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Release
```

Exact failure:

```text
GATE_C_FAIL configuration=Release reason=Shell_window_snapshot_was_not_stable
first=593076|15836|24460|CabinetWClass|file:///D:/MySetting/Downloads
second=593076|15836|24460|CabinetWClass|file:///D:/MySetting/Downloads;9244760|20152|13516|CabinetWClass|file:///D:/PROJECTS/WinExInfo
HOST_STDOUT <not-created>
HOST_STDERR <not-created>
```

The exact new controlled window was separately revalidated and closed after
the fail-closed rejection:

```text
CONTROLLED_CLEANUP hwnd=0x00000000008D1058 identity=CabinetWClass|20152|13516 url=file:///D:/PROJECTS/WinExInfo closed=true
```

The harness was subsequently hardened to require two identical candidate
samples while retaining an exact discovered candidate for cleanup; it still
propagates all COM/native identity errors and ambiguous deltas. This change
was not followed by another live run under the mandatory Release-failure stop
condition.

After that stopped run, remaining review-only harness hardening changed safety
queries to full `-ErrorAction Stop` process/service/task enumerations with
exact filtering, registry property inspection that distinguishes expected
absence from operational failure, and TCP enumeration with Stop semantics
when WinExInfo PIDs exist. Native close now atomically revalidates top-level
HWND/PID/TID/class immediately before `WM_CLOSE`, after the exact Shell URL
assertion. Module counting returns zero only when a full process enumeration
shows the controlled PID has naturally exited; module access errors propagate.
No live command has been run with these post-`fa30256` harness changes pending
review approval.

Final read-only safety state after the separately revalidated controlled close:

```text
explorer_windows=1 explorer_pids=1 processes=0
pid=15836 hook_count=0
```

## Superseded non-strict passing runs at head 3612965

### Debug

Command and result:

```text
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Debug
exit 0
```

Controlled identity and safety baseline:

```text
CONTROLLED hwnd=0x00000000000D0FC0 pid=42620 tid=12980 class=CabinetWClass url=file:///D:/PROJECTS/WinExInfo
SAFETY stage=before explorer_windows=1 explorer_pids=1 run=0 service=0 task=0 processes=0 tcp=0
```

Pane continuity and exact transitions:

```text
initial      active_tab=0x00000000020F0DDA pane=0x0000000000431056 parent=0x00000000004C0F9C rect=262,960,1604,984 expected=262,960,1604,984 overlap=false
resized      active_tab=0x00000000020F0DDA pane=0x0000000000431056 parent=0x00000000004C0F9C rect=262,870,1444,894 expected=262,870,1444,894 overlap=false
tab_switched active_tab=0x0000000000091046 pane=0x0000000000431056 parent=0x0000000000B10C9A rect=255,870,1444,894 expected=255,870,1444,894 overlap=false
restored     active_tab=0x00000000020F0DDA pane=0x0000000000431056 parent=0x00000000004C0F9C rect=262,960,1604,984 expected=262,960,1604,984 overlap=false
```

The pane owner was Explorer PID/TID `42620/12980` in every stage; class was
`WinExInfo.StatusPane`, text was `WinExInfo Gate B`, parent class was
`DUIViewWndClassName`, and DPI was 96. The tab transition was exactly one tab
to two tabs, original selected, new tab selected, then restored to one tab.

```text
TARGET_ACCEPTED protocol=1 pid=42620 tid=12980 hwnd=0x00000000000D0FC0
CLEANUP pane_absent=true exact_module_count=0 host_exit=0
SAFETY stage=after explorer_windows=1 explorer_pids=2 run=0 service=0 task=0 processes=0 tcp=0
GATE_C_PASS configuration=Debug pane_owner=explorer overlap=false cleanup=true
```

### Release

Current head was rebuilt first:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release
exit 0
```

Live command and result:

```text
pwsh -NoProfile -File .\scripts\run-gate-c.ps1 -Configuration Release
exit 0
```

Controlled identity and safety baseline:

```text
CONTROLLED hwnd=0x00000000001B0FF4 pid=16824 tid=18908 class=CabinetWClass url=file:///D:/PROJECTS/WinExInfo
SAFETY stage=before explorer_windows=1 explorer_pids=2 run=0 service=0 task=0 processes=0 tcp=0
```

Pane continuity and exact transitions:

```text
initial      active_tab=0x0000000000A30FBC pane=0x00000000001E10A2 parent=0x0000000000300FF0 rect=262,960,1604,984 expected=262,960,1604,984 overlap=false
resized      active_tab=0x0000000000A30FBC pane=0x00000000001E10A2 parent=0x0000000000300FF0 rect=262,870,1444,894 expected=262,870,1444,894 overlap=false
tab_switched active_tab=0x00000000011B0D6C pane=0x00000000001E10A2 parent=0x000000000006103A rect=255,870,1444,894 expected=255,870,1444,894 overlap=false
restored     active_tab=0x0000000000A30FBC pane=0x00000000001E10A2 parent=0x0000000000300FF0 rect=262,960,1604,984 expected=262,960,1604,984 overlap=false
```

The pane owner was Explorer PID/TID `16824/18908` in every stage, with the
same exact class/text/parent/DPI contract as Debug.

```text
TARGET_ACCEPTED protocol=1 pid=16824 tid=18908 hwnd=0x00000000001B0FF4
CLEANUP pane_absent=true exact_module_count=0 host_exit=0
SAFETY stage=after explorer_windows=1 explorer_pids=2 run=0 service=0 task=0 processes=0 tcp=0
GATE_C_PASS configuration=Release pane_owner=explorer overlap=false cleanup=true
```

Final read-only safety capture:

```text
explorer_windows=1 explorer_pids=2 processes=0 tcp=0 run=0 service=0 task=0
pid=15836 hook_count=0
pid=16824 hook_count=0
```

Both controlled top-level HWNDs were closed by the harness. No unrelated
Explorer top-level was acted on. PID 16824 remained temporarily windowless
after Release, with exact Hook DLL count zero.

## Superseded failure at approved head 4864b5a

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

## Superseded earlier commands and exits

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
GATE_C_PASS configuration=Debug pane_owner=explorer overlap=false cleanup=true
GATE_C_FAIL configuration=Release reason=Shell_window_snapshot_was_not_stable
```

Gate C: FAIL. No downstream product work is authorized.
