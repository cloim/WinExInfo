# WinExInfo Gate A Evidence

Gate A: FAIL

## Scope

- Gate: read-only Explorer static contract probe
- Host command: `WinExInfoHost.exe --probe snapshot`
- Explorer mutation: none
- DLL load or window message: none
- Registry, service, or scheduled-task mutation: none
- Network request: none

## Safety baseline

The ignored raw baseline is `docs/superpowers/evidence/raw/2026-07-10-pre-gates.txt`.

- Explorer PID 8312: `WinExInfoHook.dll=absent`
- Explorer PID 32028: `WinExInfoHook.dll=absent`
- Exact Run value `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WinExInfo`: absent
- Exact service name `WinExInfo`: absent
- Exact scheduled task name `WinExInfo`: absent
- `WinExInfoHost.exe` and `WinExInfoTests.exe` owned outbound connections: absent

## Automated verification before live probe

Command:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Result: exit 0, 31 tests passed, 0 failed.

## Live probe result

Command:

```text
.\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
```

Result: exit 3.

Both visible `CabinetWClass` records passed OS, file identity, catalog signature,
principal, architecture, and mitigation validation. Both stopped at file-version
preflight before any UI Automation request.

```text
window.0.pid=8312
window.0.preflight.error_code=UNSUPPORTED_EXPLORER_BUILD
window.0.preflight.explorer_version=fail
window.0.preflight.explorer_frame_version=fail
window.0.preflight.shell32_version=fail
window.0.preflight.hresult=-2147024770
window.0.preflight.win32=126

window.1.pid=32028
window.1.preflight.error_code=UNSUPPORTED_EXPLORER_BUILD
window.1.preflight.explorer_version=fail
window.1.preflight.explorer_frame_version=fail
window.1.preflight.shell32_version=fail
window.1.preflight.hresult=-2147024770
window.1.preflight.win32=126
```

## Bounded read-only follow-up

The loaded files have the approved fixed versions:

| PID | Module entry | Fixed file version |
|---:|---|---|
| 8312 | `Explorer.EXE` | `10.0.26100.8457` |
| 8312 | `explorerframe.dll` | `10.0.26100.8457` |
| 8312 | `SHELL32.dll` | `10.0.26100.8521` |
| 32028 | `explorer.exe` | `10.0.26100.8457` |
| 32028 | `explorerframe.dll` | `10.0.26100.8457` |
| 32028 | `SHELL32.dll` | `10.0.26100.8521` |

The probe searched loaded module entry names with case-sensitive comparisons for
`ExplorerFrame.dll` and `shell32.dll`. The live module entries use different
letter casing, so the probe recorded `ERROR_MOD_NOT_FOUND` (126) even though the
opened modules have the exact approved fixed versions. No alternate version
source was consulted.

## Stop decision

The binding Gate A rule requires an immediate FAIL record after a preflight
failure. No selector fallback, case-handling change, second live probe, UIA
capture, Explorer injection, or Task 4 work was attempted.
