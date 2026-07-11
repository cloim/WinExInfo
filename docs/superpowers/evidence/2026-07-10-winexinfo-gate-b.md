# WinExInfo Gate B Evidence

## Scope

- Execution date: 2026-07-12 (Asia/Seoul)
- Configuration: Debug, x64, static CRT, CFG enabled
- Injection target: `WinExInfoTests.exe --hook-target` only
- Explorer injection: not performed
- DLL: `WinExInfoHook.dll`, exact export `WinExInfoCallWndProc`

## Target and transport contracts

- The target emitted one canonical `READY protocol=1 pid=<pid> tid=<tid> hwnd=<16-hex>` record within 5000 ms.
- The controller retained the target process handle and accepted exactly one window of class `WinExInfo.GateBTarget.v1`.
- READY and `TARGET_ACCEPTED` PID/TID/HWND fields matched exactly.
- Validation required the current test executable file identity, same user, session and integrity level, native AMD64, a signaled exact ready event, and one exact target-class top-level window.
- The current-user-only byte pipe used protocol `WXI1` version 1. Attach and detach result request IDs matched the controller IDs.

## Commands and results

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Result: exit 0; CTest `1/1` passed; unit summary `passed=270 failed=0`.

```powershell
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Iterations 1
```

Result: exit 0.

```text
GATE_B_PASS iterations=1 target_handles_delta=0 target_threads_delta=0 controller_handles_delta=0 controller_threads_delta=0 target_exit=0
```

```powershell
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Iterations 100
```

Result: exit 0. Every cycle retained a live target until detach completed, received the UI-thread cleanup acknowledgement, found no `WinExInfo.StatusPane` child after detach, observed the exact DLL disappear within 5000 ms, and returned target/controller handle and thread counts to their per-cycle baselines.

```text
GATE_B_PASS iterations=100 target_handles_delta=0 target_threads_delta=0 controller_handles_delta=0 controller_threads_delta=0 target_exit=0
```

```powershell
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Debug -Fault UnhookFailure
.\out\build\windows-x64-debug\bin\WinExInfoTests.exe --unit --filter hook_injector.unhook_failure
```

Result: both exit 0. The injected module remained loaded, the exact HookReleased event remained nonsignaled, no unhook retry or forced unload occurred, and the target exited cleanly only after those facts were checked.

```text
GATE_B_FAULT_PASS fault=unhook_failure module_retained=true event_signaled=false target_exit=0
[PASS] hook_injector.unhook_failure
[SUMMARY] passed=1 failed=0
```

## Cleanup and safety

- Pane/subclass: the UI thread removed the exact subclass and destroyed the pane before acknowledging cleanup; the controller confirmed no pane child remained after every cycle.
- Worker/pipe: target and controller thread/handle deltas were zero after every normal cycle; the pipe was closed before module disappearance.
- Module: `WinExInfoHook.dll` disappeared from the still-live test target after every normal detach.
- Fault retention: an unsignaled HookReleased event retained the module until target exit.
- Explorer check after the stress and fault runs: `EXPLORER_HOOK_MODULE_COUNT=0`.
- No network endpoint, registry write, service, scheduled task, autostart registration, or Explorer injection was used by the Gate B harness.

## Verdict

Gate B: PASS

## Release verification (2026-07-12)

Release initially exposed a one-time `comctl32` status-pane/subclass handle
initialization in the target. A minimal comparison ruled out named-pipe
initialization and confirmed the pane/subclass call as the source. The target
test mode now creates and removes the exact pane/subclass before the resource
baseline so every measured cycle reports lifecycle deltas rather than Windows'
one-time UI initialization cost.

```powershell
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Release -Test
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Release -Iterations 100
pwsh -NoProfile -File .\scripts\run-gate-b.ps1 -Configuration Release -Fault UnhookFailure
```

```text
unit summary: passed=271 failed=0
RESOURCE_COUNTS target_handles_start=177 target_handles_end=177 target_threads_start=5 target_threads_end=5 controller_handles_start=168 controller_handles_end=168 controller_threads_start=5 controller_threads_end=5
GATE_B_PASS iterations=100 target_handles_delta=0 target_threads_delta=0 controller_handles_delta=0 controller_threads_delta=0 target_exit=0
GATE_B_FAULT_PASS fault=unhook_failure module_retained=true event_signaled=false target_exit=0
```

All three commands exited 0. Release `WinExInfoHost.exe`,
`WinExInfoTests.exe`, and `WinExInfoHook.dll` exist in the Release `bin`
directory. Gate B remains PASS in Release.

## Final safety review closure (2026-07-12)

The final review added a combined atomic hook-callback rundown state containing
the closed flag and in-flight count.
Stop now rejects new callback work and waits at most 5000 ms for the count to
reach zero before unload; timeout retains the module. A valid non-detach frame
now closes the connection and follows the same bounded cleanup path. The
controller also requires `GetNamedPipeClientProcessId` to equal the validated
target PID and compares Toolhelp module entries to the expected DLL volume
serial/file ID rather than its basename. Module disappearance is accepted only
after a successful complete Toolhelp enumeration; snapshot/enumeration errors
remain errors rather than being interpreted as absence.

Fresh Debug verification after those changes:

```text
unit summary: passed=271 failed=0
RESOURCE_COUNTS target_handles_start=185 target_handles_end=185 target_threads_start=5 target_threads_end=5 controller_handles_start=176 controller_handles_end=176 controller_threads_start=5 controller_threads_end=5
GATE_B_PASS iterations=100 target_handles_delta=0 target_threads_delta=0 controller_handles_delta=0 controller_threads_delta=0 target_exit=0
GATE_B_FAULT_PASS fault=unhook_failure module_retained=true event_signaled=false target_exit=0
```

Fresh Release verification is the result above. Before both measured runs, the
harness required five consecutive stable 100 ms handle/thread samples and then
required both every-cycle equality and whole-run starting/ending equality.

Final safety state:

```json
{"run_value":"<absent>","service_count":0,"task_count":0,"winex_process_count":0,"explorer_module_count":0,"outbound_connection_count":0}
```
