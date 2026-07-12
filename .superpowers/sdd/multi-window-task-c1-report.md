# Multi-Window Task C1 Report

## Scope

Extended `ThreadHookInjector` from one retained hook per PID to a bounded,
deduplicated set of Explorer UI-thread hook leases. The initial attach path
still exclusively creates and owns the release event, attach message/receipt,
and process runtime/pipe handshake. No DLL runtime, background coordinator,
Git integration, installation behavior, or existing controller call site was
changed.

## TDD evidence

The lease-set tests were added before the public API or implementation. The
first correctly configured Debug build failed with:

```text
tests/hook_injector_tests.cpp(167): error C2039:
'EnsureThreadHookLease': is not a member of 'ThreadHookInjector'
```

This was the expected RED: the requested lease operation did not exist.

The new focused coverage exercises:

- initial plus different-TID lease installation;
- same-TID/two-top-level deduplication after exact validation;
- invalid identity, unretained PID, 64-lease acceptance, and 65th rejection;
- final-validation adjacency and rejection before `SetWindowsHookEx`;
- exhaustive reverse release, partial failure retention, failed-lease-only
  retry, and one-shot event signaling;
- signal-only retry without double-unhook;
- premature target-gone rejection and destructor fail-safe retention;
- preservation of the existing attach cleanup and Gate B fault contracts.

## Implementation and self-review

- Each retained PID owns a fixed-capacity 64-entry installation-order lease
  set. The initial attach hook is entry zero; additional hooks use the same
  exact `WinExInfoCallWndProc` / `WH_CALLWNDPROC` contract.
- `EnsureThreadHookLease` requires a live retained PID, nonzero TID, non-null
  top-level HWND, and a supplied exact final validator. Existing TIDs validate
  before returning idempotent success. New TIDs validate immediately before
  hook installation and do not create events, send attach messages, or wait on
  runtime/pipe work. Non-exact validator tuples are never reported as success.
- A PID retained only because attach cleanup could not prove its hook removal
  or event signal is marked release-started and cannot acquire more leases; it
  remains retained solely for safe release retry/target-gone handling.
- Release starts a one-way detach phase, attempts every still-retained hook in
  reverse order, retains failures, removes successful leases, and preserves the
  first release diagnostic. A non-exact platform tuple whose enum is `OK` is
  canonicalized to `HOOK_RELEASE_FAILED`, so it cannot accidentally authorize
  detach or be overwritten by a later failure. The event is signaled only after
  no hook remains.
- Repeated release cannot repeat successful unhooks or a successful signal.
  `ConfirmTargetGone` now requires both an empty live-hook set and a signaled
  release event before closing the event and erasing the PID.
- Attach failure cleanup still retains an uncertain initial hook, and the
  destructor never attempts an unhook or event signal.
- Reviewed one-window host/controller call sites remain source-identical and
  the Gate C injector/controller identities and byte/order behavior are
  unchanged.

## Verification

Focused injector:

```text
WinExInfoTests.exe --unit --filter hook_injector
[SUMMARY] passed=25 failed=0
```

Focused controller:

```text
WinExInfoTests.exe --unit --filter explorer_controller
[SUMMARY] passed=20 failed=0
```

Full Debug:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
[SUMMARY] passed=373 failed=0
WinExInfo.Unit: Passed
WinExInfo.CommandLineGateC: Passed
100% tests passed, 0 tests failed out of 2
```

Gate B fault retention:

```text
GATE_B_FAULT_PASS fault=unhook_failure module_retained=true
event_signaled=false target_exit=0
```

Gate B normal one-cycle exact result:

```text
RESOURCE_COUNTS target_handles_start=161 target_handles_end=161
target_threads_start=5 target_threads_end=5
controller_handles_start=152 controller_handles_end=152
controller_threads_start=5 controller_threads_end=5
GATE_B_PASS iterations=1 target_handles_delta=0 target_threads_delta=0
controller_handles_delta=0 controller_threads_delta=0 target_exit=0
```

## Concern

Fresh 100-cycle normal runs reached the already documented safe-source Windows
lazy appcore/COM sampling signature at cycle 1:

```text
target_handles=31 target_threads=3
controller_handles=0 controller_threads=0
```

The exact signature is recorded in `multi-window-task-a-report.md` as
reproducing on the unmodified base after the hook DLL has disappeared. It is
therefore reported as a known verification artifact, not represented as a
successful 100-cycle zero-delta run, and no forbidden runtime/harness timing
workaround was introduced.

## Independent review

Review of the initial commit found no Critical issues and three Important
edge cases: failure-retained attach states were still attachable, non-exact
validator tuples could appear successful, and first-release-error tracking
used the status enum as an unset sentinel. Each issue received a focused RED
test and implementation correction. The final focused/full/Gate B evidence
above was regenerated after those corrections. Re-review of the corrected
implementation found no remaining Critical, Important, or Minor findings.

## Review-fix follow-up

### RED

Three additional regressions were added before production changes:

- malformed attach-wait `{OK, S_FALSE, ERROR_BUSY}` must not authorize unload
  or additional thread leases and must enter safe cleanup;
- PID retention storage must be reserved before release-event/hook side effects,
  with an injected `std::bad_alloc` producing `E_OUTOFMEMORY` and zero external
  calls;
- an exact non-OK `set_event` tuple must be returned intact and retried without
  double-unhook.

The first build failed at the deliberately absent allocation seam:

```text
tests/hook_injector_tests.cpp(287): error C2440
ThreadHookInjector: function does not take 3 arguments
```

This was the expected RED: reservation could not be injected and the requested
pre-side-effect failure contract did not yet exist.

### Correction

- `Attach` now inserts one empty, fixed-capacity `RetainedTarget` before calling
  release-event creation or either hook installation path. `std::bad_alloc` is
  caught before external side effects. Every proven-clean early exit closes any
  event and erases this reservation; uncertain hook/event states retain this
  same record. No `map::emplace` or other PID insertion remains after a hook
  installation.
- Attach authorization now requires the complete exact success tuple. A
  malformed wait success is diagnosed through `original_status`, canonicalized
  to `WINDOW_ATTACH_FAILED`, safely unhooked/signaled, retained only for
  target-gone confirmation, and never made lease-eligible.
- Detach release preserves an exact non-OK `set_event` status tuple. Only a
  malformed `OK` tuple or false success output is canonicalized to
  `HOOK_RELEASE_FAILED`.
- `ThreadHookInjector` is explicitly documented as externally serialized.
  Synchronization remains owned by the Task C2 `ExplorerSession`; no implicit
  concurrency was added here.

### Fresh verification

```text
hook_injector: passed=32 failed=0
explorer_controller: passed=20 failed=0
WinExInfo.Unit: passed=380 failed=0
WinExInfo.CommandLineGateC: passed=2 failed=0
CTest: 100% tests passed, 0 failed
```

Gate B normal and fault:

```text
GATE_B_PASS iterations=1 target_handles_delta=0 target_threads_delta=0
controller_handles_delta=0 controller_threads_delta=0 target_exit=0
GATE_B_FAULT_PASS fault=unhook_failure module_retained=true
event_signaled=false target_exit=0
```

### Separate review corrections

Review of the first follow-up commit found two additional Important malformed-
operation paths. Focused tests were added and observed failing before correction:

```text
hook_injector.attach_set_event_status: expected PIPE_DISCONNECTED, got generic
hook_injector.sethook_uncertain_handle: expected retained_target_count=1
hook_injector.thread_lease_sethook_uncertain_handle: reverse release omitted
the exposed additional handle
```

Attach cleanup now preserves an exact non-OK `set_event` tuple while retaining
the unsignaled event for retry. Initial and additional hook installation now
retain any non-null `HHOOK` exposed alongside a non-exact result, mark the PID
release-started, reject more leases, and require explicit reverse release retry.
Malformed `OK` tuples remain canonicalized to non-OK failures.

The final verification counts above include these three adversarial tests. One
Gate B normal sample reached the documented safe-source `+31` handle / `+3`
thread lazy-init signature; the immediate fresh retry produced the recorded
zero-delta pass, and the fault-retention run passed.
