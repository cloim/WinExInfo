# WinExInfo Gate A Evidence

Gate A: FAIL

## Scope

- Gate: corrected read-only Explorer static contract probe
- Host command: `WinExInfoHost.exe --probe snapshot`
- Explorer mutation: none
- DLL load or window message: none
- Registry, service, or scheduled-task mutation: none
- Network request: none
- UI Automation request: none because target preflight failed first

## Safety baseline comparison

The initial ignored baseline is
`docs/superpowers/evidence/raw/2026-07-10-pre-gates.txt`. The corrected-run
baseline is
`docs/superpowers/evidence/raw/2026-07-10-pre-gates-review-fix.txt`.

Both baseline reads have the same exact state:

- Explorer PID 8312: `WinExInfoHook.dll=absent`
- Explorer PID 32028: `WinExInfoHook.dll=absent`
- Exact Run value `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WinExInfo`: absent
- Exact service name `WinExInfo`: absent
- Exact scheduled task name `WinExInfo`: absent
- `WinExInfoHost.exe` and `WinExInfoTests.exe` owned outbound connections: absent

## Automated verification

Command:

```text
pwsh -NoProfile -File .\scripts\build.ps1 -Configuration Debug -Test
```

Result: exit 0, 37 tests passed, 0 failed, CTest 1/1 passed.

## Corrected live probe

The prior Win32 126 result was superseded because it was produced by a
case-sensitive module-basename implementation defect. The implementation now
matches only `ExplorerFrame.dll` and `shell32.dll` with exact ordinal
case-insensitive Windows semantics and found both loaded modules.

Command:

```text
.\out\build\windows-x64-debug\bin\WinExInfoHost.exe --probe snapshot
```

Result: exit 1.

```text
window.0.pid=8312
window.0.preflight.os_version=pass
window.0.preflight.file_identity=pass
window.0.preflight.catalog_signature=pass
window.0.preflight.principal=pass
window.0.preflight.architecture=pass
window.0.preflight.explorer_version=fail
window.0.preflight.explorer_frame_version=fail
window.0.preflight.shell32_version=fail
window.0.preflight.mitigations=pass
window.0.preflight.error_code=UNSUPPORTED_EXPLORER_BUILD
window.0.preflight.hresult=1
window.0.preflight.win32=0

window.1.pid=32028
window.1.preflight.os_version=pass
window.1.preflight.file_identity=pass
window.1.preflight.catalog_signature=pass
window.1.preflight.principal=pass
window.1.preflight.architecture=pass
window.1.preflight.explorer_version=fail
window.1.preflight.explorer_frame_version=fail
window.1.preflight.shell32_version=fail
window.1.preflight.mitigations=pass
window.1.preflight.error_code=UNSUPPORTED_EXPLORER_BUILD
window.1.preflight.hresult=1
window.1.preflight.win32=0
```

`HRESULT=1` is `S_FALSE`, and `win32=0` confirms a contract mismatch rather
than a Win32 transport or module-enumeration failure.

## Exact fixed-resource mismatch

The contract reads `VS_FIXEDFILEINFO` numeric file-version parts only. A bounded
read-only inspection returned:

| File | Required fixed version | Observed fixed version |
|---|---|---|
| `C:\Windows\explorer.exe` | `10.0.26100.8457` | `10.0.26100.8655` |
| `C:\Windows\System32\explorerframe.dll` | `10.0.26100.8457` | `10.0.26100.8655` |
| `C:\Windows\System32\SHELL32.dll` | `10.0.26100.8521` | `10.0.26100.8655` |

The localized/string `FileVersion` fields still display 8457 or 8521 on this
machine, but those are not the approved fixed numeric source. They were not
used as an alternate source.

## Stop decision

The corrected preflight revealed a genuine exact fixed-resource mismatch. Gate
A therefore remains FAIL. No alternate version source, selector fallback,
second corrected live probe, UIA capture, Explorer injection, or Task 4 work
was attempted.
