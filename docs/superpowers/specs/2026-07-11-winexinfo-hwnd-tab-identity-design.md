# WinExInfo HWND-Centric Tab Identity Design

Status: approved direction, pending written review

## Context

Gate A proved that each Windows 11 Explorer tab has a distinct
`ShellTabWindowClass` HWND, canonical Shell COM identity, active view HWND,
and folder state. It also proved the exact join from a selected tab HWND to
one `IShellWindows` entry through `IShellBrowser::GetWindow`.

Gate A failed only at Shell lifecycle correlation. A
`DShellWindowsEvents::WindowRegistered(cookie)` callback supplied a cookie,
but `IShellWindows::FindWindowSW` returned `S_FALSE` for that cookie on the
approved target. Treating the sole newly enumerated Shell entry as that cookie
was ambiguous when registrations and revocations were batched.

The product already requires an Explorer UI-thread hook DLL for the status
pane. The lifecycle design therefore does not need the Shell cookie to be the
tab identity.

This document supersedes the Shell lifecycle identity, event remapping, and
Gate A lifecycle acceptance sections of
`2026-07-10-winexinfo-git-status-design.md`. The existing target preflight,
exact UIA selectors, active-view/path mapping, injection safety, IPC, Git, and
pane layout contracts remain unchanged. The implementation plan will be
revised only after this design is approved.

## Decision

Use the exact `ShellTabWindowClass` HWND and canonical Shell COM identity as
the tab identity. Shell lifecycle, UIA structure, UIA selection, subclass,
and navigation callbacks are wake signals that request a complete state
reconciliation. They do not identify a tab by themselves.

The identity tuple is:

- Explorer top-level `CabinetWClass` HWND;
- direct-child `ShellTabWindowClass` HWND;
- canonical `IUnknown` identity of the matching Shell entry;
- top-level generation and tab generation.

The Shell cookie is retained as raw diagnostic evidence. It is never a
map key, lookup fallback, or source of a tab-to-object association.

## Goals

- Identify every current Explorer tab with an exact HWND-to-Shell-object
  join.
- Determine the active tab and active folder view without title, URL,
  address-bar, UIA Name, or positional guessing.
- Add and remove browser event subscriptions from complete set differences,
  even when several lifecycle callbacks are batched.
- Let the injected DLL attach same-thread subclass procedures to exact tab
  HWNDs without making COM calls on Explorer UI threads.
- Preserve deterministic cleanup, apartment ownership, and Gate A/B safety
  contracts.

## Non-goals

- Recovering or interpreting `WindowRegistered` cookies after `S_FALSE`.
- Polling for a cookie to become resolvable.
- Treating a sole added or removed Shell entry as proof of cookie ownership.
- Parsing localized labels, tab titles, window titles, URLs, or address-bar
  text.
- Detouring Explorer functions, patching vtables, or using remote threads.

## Exact Tab Set Capture

For one retained Explorer top-level HWND, one reconciliation performs these
steps in order:

1. Capture the top-level direct-child z-order from `GetTopWindow` through
   `GetWindow(..., GW_HWNDNEXT)`.
2. Retain every exact `ShellTabWindowClass` direct child. Capture the same
   z-order again and require the two sequences to match.
3. Enumerate the full `IShellWindows` collection on the dedicated Shell STA.
4. Retain entries whose `IWebBrowser2::get_HWND` equals the target top-level
   HWND.
5. Obtain each entry's canonical `IUnknown`, `IServiceProvider`,
   `IShellBrowser`, and `IShellBrowser::GetWindow` HWND.
6. Join an entry to a tab only when `IShellBrowser::GetWindow` exactly equals
   one captured `ShellTabWindowClass` HWND.
7. Require a one-to-one relation: every retained Shell entry and every
   retained tab HWND appears exactly once, with no duplicate canonical
   identity or HWND.
8. Select the first visible exact tab in the stable direct-child z-order as
   the active tab, using the already approved current-target contract.
9. Require exactly one visible active `IShellView` descendant and capture its
   filesystem state through the existing PIDL and `IShellItem` path.

An unstable z-order, missing side of the join, duplicate, zero active view, or
multiple active views is an exact contract failure. No alternate key is tried.

## Reconciliation and Generations

The Shell STA owns the previous and current complete tab maps. It compares
maps by the exact HWND and canonical identity pair.

- A pair present only in the current map is an addition.
- A pair present only in the previous map is a removal.
- An unchanged pair retains its browser subscription and generation.
- Reuse of an HWND with a different canonical identity creates a new tab
  generation only after the prior pair has been removed.
- Two live entries using one HWND or one canonical identity are a contract
  failure.

All additions are subscribed to `DIID_DWebBrowserEvents2` by their canonical
identity. All removals are unadvised before their COM resources are released.
The active logical snapshot is replaced only after the complete new map,
subscriptions, view cardinality, and path state pass validation.

This full-set comparison remains exact when several additions or removals are
observed together. It reports the current truth instead of attributing an
individual Shell cookie to one delta.

## Event and Hook Roles

### Read-only Host and Gate A

The existing exact event sources remain wake signals:

- `DShellWindowsEvents::WindowRegistered` and `WindowRevoked`;
- exact TabListView UIA selection and structure events;
- subscribed browser `NavigateComplete2` events.

The lifecycle cookie is recorded only as callback evidence. After any allowed
signal, the Shell STA performs complete HWND-centric reconciliation. A UIA
selection event hides the prior pane state until the refreshed active HWND,
view, and path pass.

### Injected product DLL

`WH_CALLWNDPROC` remains the bounded loader and initialization mechanism. It
does not become the long-lived tab identity.

After initialization on the exact Explorer UI thread, the DLL installs a
same-thread subclass on each reconciled `ShellTabWindowClass` HWND. The
subclass has three responsibilities only:

- report its exact HWND when it receives terminal destruction;
- report geometry or visibility changes relevant to pane placement;
- post an internal refresh request and immediately continue normal message
  processing.

New tab discovery comes from TabListView structure or Shell lifecycle wake
signals followed by complete reconciliation. A newly reconciled HWND is then
subclassed on its owning Explorer UI thread. Selection remains an exact UIA
event because XAML tab selection is not assumed to produce a particular
Win32 message.

The subclass never performs Shell COM, UIA, Git, file, pipe-wait, or blocking
work. `SetWindowSubclass` and `RemoveWindowSubclass` occur on the owning UI
thread only.

### Production update flow

After the Shell STA has built a complete valid map, the Host sends the desired
ordered tab HWND set, top-level generation, and tab generations through the
validated current-user pipe. The DLL pipe worker validates lengths and enum
values, then posts the immutable update to the exact Explorer UI thread. It
does not call subclass APIs itself.

The Explorer UI thread revalidates each HWND's process, thread, exact class,
and direct parent before changing the subclass set. It adds missing exact
subclasses, removes subclasses absent from the desired set, and returns one
bounded result. The Explorer UI thread creates, reparents, or shows the pane
only after the subclass update succeeds. The Host accepts the attachment only
after the returned top-level and generations match the request. A stale result
is ignored; an explicit failure hides the pane and enters the existing
attach/detach cleanup path.

Gate A runs the same map and generation logic without sending a DLL update.
This keeps the read-only identity proof separate from Gate B's injected
subclass lifecycle proof.

## Thread and Ownership Contract

- The Host Shell STA exclusively owns `IShellWindows`, browser objects,
  canonical identities, Shell services, views, and connection points.
- The UIA MTA exclusively owns UIA elements, cache requests, and handlers.
- Each Explorer UI thread exclusively owns its tab subclasses and pane HWNDs.
- Cross-thread and cross-process messages contain only validated immutable
  HWNDs, generations, event kinds, sequence numbers, and path values.
- Raw COM pointers never cross apartments or the named pipe.

The Shell STA revalidates the PID, UI thread, parent chain, exact class, and
current generation of every HWND received from another component.

## Error Handling

The following conditions hide the pane state and record
`ACTIVE_VIEW_CONTRACT_MISMATCH` without fallback:

- unstable or cyclic direct-child z-order;
- zero or duplicate exact tab HWNDs;
- incomplete or non-bijective HWND-to-Shell-entry join;
- duplicate canonical identity;
- active tab or active view cardinality other than one;
- stale top-level or tab generation;
- wrong process, thread, class, or parent for a subclass signal;
- failed or incoherent Advise/Unadvise result.

Transport failures preserve their original HRESULT and origin. Cleanup keeps
the first failure while continuing reverse-order handler removal,
unsubclassing, Unadvise, worker shutdown, and COM release.

## Revised Gate A Contract

Gate A no longer requires a `WindowRegistered` cookie to resolve to a Shell
object or requires one lifecycle record per cookie. PASS requires:

- exact target preflight and all five UIA selectors;
- a complete one-to-one tab HWND and canonical Shell entry map in controlled
  single-tab and multi-tab states;
- exactly one active view for the stable active tab;
- addition and removal set differences that create and remove the exact
  browser subscriptions;
- tab selection, structure change, navigation, Back, tab close, and window
  close signals that each lead to the correct complete current state;
- zero guessed associations, unresolved current entries, late events, and
  cleanup residue.

The existing `S_FALSE` cookie evidence remains historical proof for why the
identity contract changed. It is not a failure in the revised Gate A because
`FindWindowSW(cookie)` is no longer part of the identity path.

## Gate B Impact

Gate B retains the test-target-only 100-cycle hook load/unload proof. Its
cleanup assertions expand to cover every installed tab subclass:

- every subclass is removed on its owning UI thread;
- every child pane is destroyed;
- the temporary `WH_CALLWNDPROC` hook is released;
- the DLL module disappears after the explicit reference is released;
- target/controller handle and thread deltas return to zero;
- an unhook failure never signals HookReleased or forces unload.

No real Explorer injection begins until the revised Gate A and test-target
Gate B both pass.

## Verification Strategy

Automated tests cover:

- exact one-to-one joins for one and several tabs;
- zero, duplicate, foreign, stale, and reused HWNDs;
- duplicate and changed canonical identities;
- batched additions and removals without cookies;
- subscription creation, retention, removal, and rollback ordering;
- exact UIA sender scope and full-set refresh triggering;
- subclass same-thread validation, destruction, stale generation rejection,
  and reverse cleanup;
- pipe tab-set update validation, UI-thread dispatch, acknowledgment
  correlation, and stale acknowledgment rejection;
- no state commit before a complete validated map;
- Gate A report and exit behavior under success, contract failure, transport
  failure, and cleanup failure.

The live controlled run captures single-tab and multi-tab HWND/PID/TID,
stable z-order, all selector scopes, the full HWND-to-canonical-identity map,
active view/path transitions, event sequence, subscription counts, cleanup,
and the unchanged safety baseline.

## Acceptance Criteria

The design is accepted for implementation when all of these are true:

- `ShellTabWindowClass` HWND is the only tab window key.
- Canonical `IUnknown` is the only Shell object key.
- The current tab set is a complete bijection between those keys.
- Shell cookies never select or recover a tab entry.
- Event callbacks only request a refresh or report a validated exact HWND.
- The active view and folder path are committed only after full validation.
- Every COM resource, handler, subclass, child HWND, hook, thread, and handle
  has one deterministic owner and reverse cleanup step.

## Rejected Alternatives

- **Synchronous cookie resolution inside `WindowRegistered`:** exact when it
  returns `S_OK`, but unnecessary once the product identity is the tab HWND;
  it also keeps Gate A dependent on the observed `S_FALSE` API behavior.
- **Direct COM pointers in callback envelopes:** conflicts with the immutable
  cross-apartment boundary and complicates deterministic release.
- **Sole-delta inference, titles, URLs, address text, or UIA Name:** ambiguous
  or localized and therefore prohibited.
- **Persistent thread hook as the tab identity:** a thread hook observes
  messages, not tab ownership. Exact HWND reconciliation remains the identity
  source.

## References

- <https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw>
- <https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass>
- <https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nn-exdisp-ishellwindows>
- <https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-findwindowsw>
- <https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-addautomationeventhandler>
- <https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-addstructurechangedeventhandler>
