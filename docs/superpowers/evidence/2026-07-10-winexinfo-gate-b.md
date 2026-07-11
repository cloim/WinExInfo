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
