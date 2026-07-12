[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [Parameter()]
    [switch]$HarnessSelfTest
)

$ErrorActionPreference = 'Stop'

function Test-ExactWindowIdentity {
    param($Actual, $Expected)
    return $null -ne $Actual -and $null -ne $Expected -and
        [long]$Actual.Hwnd -eq [long]$Expected.Hwnd -and
        [int]$Actual.Pid -eq [int]$Expected.Pid -and
        [int]$Actual.Tid -eq [int]$Expected.Tid -and
        [string]$Actual.ClassName -ceq [string]$Expected.ClassName -and
        [string]$Actual.LocationUrl -ceq [string]$Expected.LocationUrl
}

function Get-ExactControlledDelta {
    param([object[]]$Before, [object[]]$After, [string]$ExpectedUrl)
    foreach ($existing in $Before) {
        $matches = @($After | Where-Object { Test-ExactWindowIdentity $_ $existing })
        if ($matches.Count -ne 1) {
            throw "Baseline Shell identity changed: hwnd=0x$(([long]$existing.Hwnd).ToString('X16'))"
        }
    }
    $delta = @($After | Where-Object {
        $candidate = $_
        -not ($Before | Where-Object Hwnd -EQ $candidate.Hwnd)
    })
    if ($delta.Count -ne 1 -or
        [string]$delta[0].ClassName -cne 'CabinetWClass' -or
        [string]$delta[0].LocationUrl -cne $ExpectedUrl) {
        throw "Controlled Shell delta was not exact: count=$($delta.Count)"
    }
    return $delta[0]
}

function Assert-LedgerIdentity {
    param($Expected, [object[]]$Current)
    $matches = @($Current | Where-Object Hwnd -EQ $Expected.Hwnd)
    if ($matches.Count -ne 1 -or -not (Test-ExactWindowIdentity $matches[0] $Expected)) {
        throw "Controlled ledger identity changed: hwnd=0x$(([long]$Expected.Hwnd).ToString('X16'))"
    }
}

function Assert-IsolatedControlledProcess {
    param(
        [int]$ProcessId,
        [int[]]$BaselineProcessIds,
        [object[]]$ControlledWindows,
        [object[]]$AllShellWindows,
        [object[]]$AllTopLevelWindows
    )
    if ($ProcessId -eq 0 -or $BaselineProcessIds -contains $ProcessId) {
        throw 'Controlled Explorer PID was not newly created.'
    }
    $owned = @($ControlledWindows | Where-Object Pid -EQ $ProcessId)
    $shell = @($AllShellWindows | Where-Object Pid -EQ $ProcessId)
    if ($owned.Count -eq 0 -or $shell.Count -ne $owned.Count) {
        throw 'Controlled Explorer PID owns an untracked Shell window.'
    }
    foreach ($window in $shell) {
        Assert-LedgerIdentity $window $owned
    }
    $unexpected = @($AllTopLevelWindows | Where-Object {
        $_.Pid -eq $ProcessId -and $_.Visible -and
        $_.ClassName -cne 'CabinetWClass'
    })
    if ($unexpected.Count -ne 0) {
        throw 'Controlled Explorer PID owns an unexpected visible top-level window.'
    }
}

function Assert-ExactCleanupDiagnostic {
    param([string]$Line, [int]$ProcessId)
    $expected = "^LIFECYCLE_EVENT event=stop pid=$ProcessId ack_pid=$ProcessId request=[1-9][0-9]* result=0 ack_result=0 pane=0 tab_subclass=0 parent_subclass=0 refresh_worker=0 callback=0$"
    if ($Line -cnotmatch $expected) {
        throw "Stop diagnostic did not prove exact zero cleanup: $Line"
    }
}

function Assert-SafetyBaseline {
    param($Before, $After)
    foreach ($name in @('RunCount','ServiceCount','TaskCount','TcpCount','WinExInfoProcessCount')) {
        if ([int]$Before.$name -ne [int]$After.$name) {
            throw "Safety baseline changed: $name"
        }
    }
}

function Assert-AuthoritativeUpdateHistory {
    param($Window, [string[]]$Lines, [bool]$RequireTwoTabs, [bool]$RequireRemoval)
    $hwnd = '0x' + ([long]$Window.Hwnd).ToString('X16')
    $updates = @($Lines | Where-Object {
        $_ -like "LIFECYCLE_EVENT event=update pid=$($Window.Pid) * hwnd=$hwnd *"
    })
    if ($updates.Count -eq 0) { throw "No update diagnostic for hwnd=$hwnd" }
    $topGenerations = @()
    $tabCounts = @()
    foreach ($line in $updates) {
        if ($line -cnotmatch ' top_generation=([1-9][0-9]*) tabs=([^ ]+) result=0$') {
            throw "Malformed update diagnostic: $line"
        }
        $topGenerations += [uint64]$Matches[1]
        $tabs = @($Matches[2] -split ',')
        foreach ($tab in $tabs) {
            if ($tab -cnotmatch '^0x[0-9A-F]{16}:[1-9][0-9]*:[1-9][0-9]*$') {
                throw "Malformed tab generation diagnostic: $tab"
            }
        }
        $tabCounts += $tabs.Count
    }
    if (@($topGenerations | Sort-Object -Unique).Count -ne 1) {
        throw "Top-level generation changed without HWND reuse: hwnd=$hwnd"
    }
    if ($RequireTwoTabs -and (-not ($tabCounts -contains 1) -or -not ($tabCounts -contains 2))) {
        throw "One-to-two tab generation transition was not recorded: hwnd=$hwnd"
    }
    if ($RequireRemoval) {
        $removals = @($Lines | Where-Object {
            $_ -like "LIFECYCLE_EVENT event=remove pid=$($Window.Pid) * hwnd=$hwnd top_generation=$($topGenerations[0]) result=0"
        })
        if ($removals.Count -ne 1) { throw "Removal diagnostic cardinality hwnd=$hwnd count=$($removals.Count)" }
    }
    Write-Output "GENERATION hwnd=$hwnd pid=$($Window.Pid) top=$($topGenerations[0]) tab_counts=$(@($tabCounts | Sort-Object -Unique) -join ',')"
}

function Invoke-HarnessSelfTest {
    $window = [pscustomobject]@{
        Hwnd = [long]0x1000; Pid = 41; Tid = 11
        ClassName = 'CabinetWClass'; LocationUrl = 'file:///D:/controlled'
    }
    if (-not (Test-ExactWindowIdentity $window $window)) { throw 'identity_accept failed' }
    $drift = $window.PSObject.Copy(); $drift.Tid = 12
    if (Test-ExactWindowIdentity $window $drift) { throw 'identity_reject failed' }
    $baseline = @([pscustomobject]@{
        Hwnd = [long]0x900; Pid = 9; Tid = 8
        ClassName = 'CabinetWClass'; LocationUrl = 'file:///D:/existing'
    })
    $delta = Get-ExactControlledDelta $baseline @($baseline + $window) 'file:///D:/controlled'
    if ($delta.Hwnd -ne $window.Hwnd) { throw 'delta failed' }
    Assert-LedgerIdentity $window @($window)
    $ledgerRejected = $false
    try { Assert-LedgerIdentity $window @($drift) } catch { $ledgerRejected = $true }
    if (-not $ledgerRejected) { throw 'ledger_reject failed' }
    Assert-IsolatedControlledProcess 41 @(9) @($window) @($baseline + $window) @(
        [pscustomobject]@{ Pid=41; Visible=$true; ClassName='CabinetWClass' })
    $isolationRejected = $false
    try {
        Assert-IsolatedControlledProcess 41 @(9) @($window) @($baseline + $window) @(
            [pscustomobject]@{ Pid=41; Visible=$true; ClassName='UnexpectedWindow' })
    } catch { $isolationRejected = $true }
    if (-not $isolationRejected) { throw 'isolation_reject failed' }
    Assert-ExactCleanupDiagnostic 'LIFECYCLE_EVENT event=stop pid=41 ack_pid=41 request=5 result=0 ack_result=0 pane=0 tab_subclass=0 parent_subclass=0 refresh_worker=0 callback=0' 41
    Assert-SafetyBaseline ([pscustomobject]@{ RunCount=0; ServiceCount=0; TaskCount=0; TcpCount=0 }) ([pscustomobject]@{ RunCount=0; ServiceCount=0; TaskCount=0; TcpCount=0 })
    Write-Output 'HARNESS_SELF_TEST_PASS cases=8'
}

if ($HarnessSelfTest) {
    Invoke-HarnessSelfTest
    exit 0
}

. (Join-Path $PSScriptRoot 'run-gate-c.ps1') -Configuration $Configuration -ExportHelpers

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class LifecycleNative {
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] private static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder path, ref int size);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] private static extern IntPtr OpenEvent(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool SetEvent(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder value, int count);

    public static void SignalExactHost(int pid, string expectedPath) {
        const uint PROCESS_QUERY_LIMITED_INFORMATION=0x1000, EVENT_MODIFY_STATE=0x0002;
        IntPtr process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,false,pid);
        if(process==IntPtr.Zero) throw new InvalidOperationException("open_host_process="+Marshal.GetLastWin32Error());
        try {
            StringBuilder actual=new StringBuilder(32768); int size=actual.Capacity;
            if(!QueryFullProcessImageName(process,0,actual,ref size) || !String.Equals(actual.ToString(),expectedPath,StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("host_identity_mismatch");
        } finally { CloseHandle(process); }
        string name="Local\\WinExInfo.Host.Stop.v1."+pid;
        IntPtr stop=OpenEvent(EVENT_MODIFY_STATE,false,name);
        if(stop==IntPtr.Zero) throw new InvalidOperationException("open_stop_event="+Marshal.GetLastWin32Error());
        try { if(!SetEvent(stop)) throw new InvalidOperationException("set_stop_event="+Marshal.GetLastWin32Error()); }
        finally { CloseHandle(stop); }
    }

    public static string[] VisibleTopLevelClasses(int expectedPid) {
        List<string> result=new List<string>(); string failure=null;
        EnumWindowsProc callback=delegate(IntPtr hwnd, IntPtr unused) {
            uint pid; GetWindowThreadProcessId(hwnd,out pid);
            if(pid==(uint)expectedPid && IsWindowVisible(hwnd)) {
                StringBuilder name=new StringBuilder(256);
                if(GetClassName(hwnd,name,name.Capacity)==0) { failure="class="+Marshal.GetLastWin32Error(); return false; }
                result.Add(name.ToString());
            }
            return true;
        };
        if(!EnumWindows(callback,IntPtr.Zero) && failure==null) failure="enumeration="+Marshal.GetLastWin32Error();
        GC.KeepAlive(callback);
        if(failure!=null) throw new InvalidOperationException(failure);
        return result.ToArray();
    }
}
'@

function Wait-NewControlledWindow {
    param([object[]]$Before, [string]$ExpectedUrl, [int]$TimeoutMs = 8000)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $stable = $null
    do {
        $now = @(Get-ShellWindows | Sort-Object Hwnd)
        try { $candidate = Get-ExactControlledDelta $Before $now $ExpectedUrl } catch {
            if ($_.Exception.Message -notlike 'Controlled Shell delta was not exact: count=0*') { throw }
            $candidate = $null
        }
        if ($null -ne $candidate) {
            $signature = "$($candidate.Hwnd)|$($candidate.Pid)|$($candidate.Tid)|$($candidate.LocationUrl)"
            if ($signature -ceq $stable) { return $candidate }
            $stable = $signature
        } else { $stable = $null }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Controlled Explorer window did not stabilize: $ExpectedUrl"
}

function Open-ControlledWindow {
    param([object[]]$Before, [string]$Path, [string]$Url)
    Start-Process -FilePath "$env:WINDIR\explorer.exe" -ArgumentList @('/separate,','/n,',$Path)
    return Wait-NewControlledWindow $Before $Url
}

function Assert-CurrentControlledWindow {
    param($Window)
    Assert-LedgerIdentity $Window @(Get-StableShellWindows)
}

function Get-ExplorerResourceState {
    param([int]$ProcessId)
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    [pscustomobject]@{ Handles=[int]$process.HandleCount; Threads=@($process.Threads).Count }
}

function Wait-ExactPane {
    param($Window, [int]$TimeoutMs = 8000)
    $script:lastLayoutDiagnostic = 'none'
    return Wait-Until -TimeoutMs $TimeoutMs -Failure 'Exact lifecycle pane was not observed.' -Condition {
        try {
            Assert-CurrentControlledWindow $Window
            $layout = [GateCHelper]::Capture([long]$Window.Hwnd)
            $identity = [GateCHelper]::Identity([long]$Window.Hwnd)
            Assert-Layout $layout $identity 'lifecycle'
            $layout
        } catch { $script:lastLayoutDiagnostic=$_.Exception.Message; $null }
    }
}

$configurationName = $Configuration.ToLowerInvariant()
$binaryDirectory = Join-Path $PSScriptRoot "..\out\build\windows-x64-$configurationName\bin"
$hostExecutable = [IO.Path]::GetFullPath((Join-Path $binaryDirectory 'WinExInfoHost.exe'))
$hookDll = [IO.Path]::GetFullPath((Join-Path $binaryDirectory 'WinExInfoHook.dll'))
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf) -or -not (Test-Path -LiteralPath $hookDll -PathType Leaf)) {
    throw "Lifecycle binaries are missing: $binaryDirectory"
}
$runRoot = Join-Path $env:TEMP "WinExInfoLifecycle-$Configuration-$PID"
$pathA = Join-Path $runRoot 'window-a'
$pathB = Join-Path $runRoot 'window-b'
$pathRestart = Join-Path $runRoot 'window-restart'
New-Item -ItemType Directory -Path $pathA,$pathB,$pathRestart -Force | Out-Null
$urlA = ([Uri]([IO.Path]::GetFullPath($pathA))).AbsoluteUri
$urlB = ([Uri]([IO.Path]::GetFullPath($pathB))).AbsoluteUri
$urlRestart = ([Uri]([IO.Path]::GetFullPath($pathRestart))).AbsoluteUri
$stdoutPath = Join-Path $env:TEMP "winexinfo-lifecycle-$Configuration-$PID.stdout"
$stderrPath = Join-Path $env:TEMP "winexinfo-lifecycle-$Configuration-$PID.stderr"
Remove-Item -LiteralPath $stdoutPath,$stderrPath -ErrorAction SilentlyContinue

$before = Get-SafetyState
$baselineWindows = @(Get-StableShellWindows)
$baselinePids = @(Get-Process -Name explorer -ErrorAction Stop | ForEach-Object Id)
$controlled = @()
$hostProcess = $null
$failure = $null
$cleanupDiagnostics = @()
$tabsChanged = $false
$restartObserved = $false

try {
    if ($before.WinExInfoProcessCount -ne 0) { throw 'A WinExInfo process already existed before the run.' }
    foreach ($existing in $baselineWindows) {
        if ([GateCHelper]::ExactPaneCount([long]$existing.Hwnd) -ne 0) {
            throw "A baseline Explorer window already contained a WinExInfo pane."
        }
    }
    foreach ($existingPid in $baselinePids) {
        if ((Get-ExactModuleCount $existingPid) -ne 0) {
            throw "A baseline Explorer process already contained the exact Hook DLL. pid=$existingPid"
        }
    }
    $windowA = Open-ControlledWindow $baselineWindows $pathA $urlA
    $controlled += $windowA
    $beforeB = @(Get-StableShellWindows)
    $windowB = Open-ControlledWindow $beforeB $pathB $urlB
    $controlled += $windowB
    foreach ($window in $controlled) {
        if ([GateCHelper]::ExactPaneCount([long]$window.Hwnd) -ne 0 -or
            (Get-ExactModuleCount $window.Pid) -ne 0) {
            throw 'A controlled Explorer target was not clean before Host start.'
        }
    }
    $resourceBefore = Get-ExplorerResourceState $windowB.Pid
    $hostProcess = Start-Process -FilePath $hostExecutable -ArgumentList '--background' -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru

    $initialA = Wait-ExactPane $windowA
    $initialB = Wait-ExactPane $windowB
    [GateCHelper]::Resize([long]$windowA.Hwnd,-120,-80)
    $resizedA = Wait-ExactPane $windowA
    if ($resizedA.Pane.ToString() -ceq $initialA.Pane.ToString()) { throw 'Controlled resize did not reflow pane.' }
    $transitionA = [GateCHelper]::AddAndSwitchTab([long]$windowA.Hwnd)
    [void](Wait-ExactPane $windowA)
    $transitionB = [GateCHelper]::AddAndSwitchTab([long]$windowB.Hwnd)
    [void](Wait-ExactPane $windowB)
    $tabsChanged = $transitionA.Count -eq 2 -and $transitionB.Count -eq 2
    [void][GateCHelper]::RestoreSingleTab([long]$windowA.Hwnd)
    [void][GateCHelper]::RestoreSingleTab([long]$windowB.Hwnd)

    $ownedA = @($controlled | Where-Object Pid -EQ $windowA.Pid)
    if ($ownedA.Count -ne 1) {
        throw 'Restart source PID did not own exactly one controlled Explorer window.'
    }
    $topClasses = @([LifecycleNative]::VisibleTopLevelClasses([int]$windowA.Pid) | ForEach-Object {
        [pscustomobject]@{ Pid=[int]$windowA.Pid; Visible=$true; ClassName=$_ }
    })
    Assert-IsolatedControlledProcess $windowA.Pid $baselinePids $ownedA @(Get-StableShellWindows) $topClasses
    Assert-CurrentControlledWindow $windowA
    [GateCHelper]::CloseExact([long]$windowA.Hwnd,[int]$windowA.Pid,[int]$windowA.Tid,'CabinetWClass')
    [void](Wait-Until -TimeoutMs 5000 -Failure 'Controlled restart source window did not close.' -Condition { -not [GateCHelper]::Exists([long]$windowA.Hwnd) })
    [void](Wait-Until -TimeoutMs 8000 -Failure 'Isolated controlled Explorer process did not exit naturally.' -Condition { $null -eq (Get-Process -Id $windowA.Pid -ErrorAction SilentlyContinue) })
    $beforeRestart = @(Get-StableShellWindows)
    $restartWindow = Open-ControlledWindow $beforeRestart $pathRestart $urlRestart
    $controlled += $restartWindow
    if ($restartWindow.Pid -eq $windowA.Pid) { throw 'Explorer restart reused the exited PID.' }
    [void](Wait-ExactPane $restartWindow)
    $restartObserved = $true

    [LifecycleNative]::SignalExactHost([int]$hostProcess.Id,$hostExecutable)
    if (-not $hostProcess.WaitForExit(30000)) { throw 'Host did not stop gracefully within 30000ms.' }
    if ($hostProcess.ExitCode -ne 0) { throw "Host exit=$($hostProcess.ExitCode)" }
    $hostOut = @(Get-Content -LiteralPath $stdoutPath -ErrorAction Stop)
    $hostErr = @(Get-Content -LiteralPath $stderrPath -ErrorAction Stop)
    if ($hostErr.Count -ne 0 -or @($hostOut | Where-Object { $_ -like '*status=incomplete*' }).Count -ne 0) {
        throw "Host diagnostics failed: stderr=$($hostErr -join '|')"
    }
    $attachedPids = @($hostOut | Where-Object { $_ -like 'LIFECYCLE_EVENT event=attach * result=0' } | ForEach-Object {
        if ($_ -match ' pid=([0-9]+) ') { [int]$Matches[1] }
    } | Sort-Object -Unique)
    if ($attachedPids.Count -eq 0) { throw 'No authoritative attach diagnostic was emitted.' }
    foreach ($attachedPid in $attachedPids) {
        $stopLines = @($hostOut | Where-Object { $_ -like "LIFECYCLE_EVENT event=stop pid=$attachedPid *" })
        if ($stopLines.Count -ne 1) { throw "Stop diagnostic cardinality pid=$attachedPid count=$($stopLines.Count)" }
        Assert-ExactCleanupDiagnostic $stopLines[0] $attachedPid
        if ((Get-ExactModuleCount $attachedPid) -ne 0) { throw "Hook module remained pid=$attachedPid" }
    }
    Assert-AuthoritativeUpdateHistory $windowA $hostOut $true $true
    Assert-AuthoritativeUpdateHistory $windowB $hostOut $true $false
    Assert-AuthoritativeUpdateHistory $restartWindow $hostOut $false $false
    foreach ($window in @($windowB,$restartWindow)) {
        if ([GateCHelper]::Exists([long]$window.Hwnd) -and [GateCHelper]::ExactPaneCount([long]$window.Hwnd) -ne 0) {
            throw "Pane remained hwnd=0x$(([long]$window.Hwnd).ToString('X16'))"
        }
    }
    foreach ($remaining in @(Get-StableShellWindows)) {
        if ([GateCHelper]::ExactPaneCount([long]$remaining.Hwnd) -ne 0) {
            throw "Pane remained in Explorer window hwnd=0x$(([long]$remaining.Hwnd).ToString('X16'))"
        }
    }
    $resourceAfter = Get-ExplorerResourceState $windowB.Pid
    if ($resourceAfter.Handles -ne $resourceBefore.Handles -or $resourceAfter.Threads -ne $resourceBefore.Threads) {
        throw "Explorer resource baseline changed handles=$($resourceBefore.Handles)->$($resourceAfter.Handles) threads=$($resourceBefore.Threads)->$($resourceAfter.Threads)"
    }
    $hostOut | ForEach-Object { Write-Output "HOST_STDOUT $_" }
}
catch { $failure = $_ }
finally {
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
        try { [LifecycleNative]::SignalExactHost([int]$hostProcess.Id,$hostExecutable) } catch { $cleanupDiagnostics += "host_stop=$($_.Exception.Message)" }
        try { [void]$hostProcess.WaitForExit(30000) } catch { $cleanupDiagnostics += "host_wait=$($_.Exception.Message)" }
        if (-not $hostProcess.HasExited) { $cleanupDiagnostics += 'host_remained_running=true' }
    }
    $cleanupWindows = @($controlled)
    [array]::Reverse($cleanupWindows)
    foreach ($window in $cleanupWindows) {
        if ([GateCHelper]::Exists([long]$window.Hwnd)) {
            try {
                Assert-CurrentControlledWindow $window
                try { [void][GateCHelper]::RestoreSingleTab([long]$window.Hwnd) } catch {}
                [GateCHelper]::CloseExact([long]$window.Hwnd,[int]$window.Pid,[int]$window.Tid,'CabinetWClass')
                [void](Wait-Until -TimeoutMs 5000 -Failure 'Controlled cleanup window did not close.' -Condition { -not [GateCHelper]::Exists([long]$window.Hwnd) })
            } catch { $cleanupDiagnostics += "window_cleanup=$($_.Exception.Message)" }
        }
    }
    try { Remove-Item -LiteralPath $runRoot -Recurse -Force -ErrorAction Stop } catch { $cleanupDiagnostics += "temp_cleanup=$($_.Exception.Message)" }
}

$after = $null
try { $after = Get-SafetyState } catch { $cleanupDiagnostics += "after_safety=$($_.Exception.Message)" }
if ($null -eq $failure -and $null -ne $after) {
    try { Assert-SafetyBaseline $before $after } catch { $failure = $_ }
    if ($after.ExplorerWindows -ne $before.ExplorerWindows) { $failure = [Exception]'Explorer window baseline changed.' }
}
if ($null -ne $failure -or $cleanupDiagnostics.Count -ne 0) {
    $failureReason = if ($failure -is [System.Management.Automation.ErrorRecord]) {
        $failure.Exception.Message
    } elseif ($failure -is [Exception]) {
        $failure.Message
    } else {
        $cleanupDiagnostics -join '|'
    }
    Write-Output "LIFECYCLE_FAIL configuration=$Configuration reason=$($failureReason -replace ' ','_')"
    if ($null -ne $failure) { throw $failure }
    throw "Lifecycle cleanup failed: $($cleanupDiagnostics -join '|')"
}
if (-not $tabsChanged -or -not $restartObserved) { throw 'Lifecycle transitions were incomplete.' }
Write-Output 'LIFECYCLE_PASS windows=2 tabs_changed=true restart=true cleanup=true'
