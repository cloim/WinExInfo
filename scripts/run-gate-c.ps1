[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = 'D:\PROJECTS\WinExInfo'
$configurationName = $Configuration.ToLowerInvariant()
$binaryDirectory = Join-Path $PSScriptRoot "..\out\build\windows-x64-$configurationName\bin"
$hostExecutable = Join-Path $binaryDirectory 'WinExInfoHost.exe'
$hookDll = Join-Path $binaryDirectory 'WinExInfoHook.dll'
$stdoutPath = Join-Path $env:TEMP "winexinfo-gate-c-$Configuration-$PID.stdout"
$stderrPath = Join-Path $env:TEMP "winexinfo-gate-c-$Configuration-$PID.stderr"

if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf) -or
    -not (Test-Path -LiteralPath $hookDll -PathType Leaf)) {
    throw "Gate C binaries were not found in $binaryDirectory"
}

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase
$uiaReferences = @(
    [System.Windows.Automation.AutomationElement].Assembly.Location
    [System.Windows.Automation.AutomationIdentifier].Assembly.Location
    [Windows.Rect].Assembly.Location
    (Join-Path $PSHOME 'System.Threading.Thread.dll')
)
Add-Type -ReferencedAssemblies $uiaReferences -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Automation;

public sealed class GateCWindowIdentity {
    public long Hwnd;
    public int Pid;
    public int Tid;
    public string ClassName;
}

public sealed class GateCRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
    public override string ToString() { return Left + "," + Top + "," + Right + "," + Bottom; }
}

public sealed class GateCLayout {
    public long PaneHwnd;
    public long ParentHwnd;
    public int PanePid;
    public int PaneTid;
    public string PaneClass;
    public string PaneText;
    public string ParentClass;
    public GateCRect Parent;
    public GateCRect Status;
    public GateCRect LeftGroup;
    public GateCRect RightGroup;
    public GateCRect Pane;
    public GateCRect Expected;
    public int Dpi;
    public bool Overlap;
}

public static class GateCHelper {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct WINDOWPLACEMENT {
        public int length, flags, showCmd;
        public POINT ptMinPosition, ptMaxPosition;
        public RECT rcNormalPosition;
    }
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder value, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr hwnd, StringBuilder value, int count);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT value);
    [DllImport("user32.dll")] private static extern bool SetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT value);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr hwnd, EnumWindowsProc callback, IntPtr value);
    [DllImport("kernel32.dll")] private static extern void Sleep(uint milliseconds);

    private const uint GA_ROOT = 2;
    private const uint WM_CLOSE = 0x0010;
    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_NOACTIVATE = 0x0010;

    private static string ClassOf(IntPtr hwnd) {
        StringBuilder value = new StringBuilder(256);
        if (GetClassName(hwnd, value, value.Capacity) == 0) throw new InvalidOperationException("GetClassName failed");
        return value.ToString();
    }
    private static string TextOf(IntPtr hwnd) {
        StringBuilder value = new StringBuilder(256);
        GetWindowText(hwnd, value, value.Capacity);
        return value.ToString();
    }
    private static GateCRect RectOf(IntPtr hwnd) {
        RECT value;
        if (!GetWindowRect(hwnd, out value)) throw new InvalidOperationException("GetWindowRect failed");
        return new GateCRect { Left=value.Left, Top=value.Top, Right=value.Right, Bottom=value.Bottom };
    }
    private static GateCRect BoundsOf(AutomationElement element) {
        System.Windows.Rect value = element.Current.BoundingRectangle;
        return new GateCRect {
            Left=(int)Math.Round(value.Left), Top=(int)Math.Round(value.Top),
            Right=(int)Math.Round(value.Right), Bottom=(int)Math.Round(value.Bottom)
        };
    }
    private static AutomationElement Exact(AutomationElement root, TreeScope scope, params Condition[] conditions) {
        Condition condition = conditions.Length == 1 ? conditions[0] : new AndCondition(conditions);
        AutomationElementCollection matches = root.FindAll(scope, condition);
        if (matches.Count != 1) throw new InvalidOperationException("UIA exact cardinality was " + matches.Count);
        return matches[0];
    }
    private static Condition Property(AutomationProperty property, object value) { return new PropertyCondition(property, value); }
    private static bool Intersects(GateCRect a, GateCRect b) {
        return Math.Max(a.Left,b.Left) < Math.Min(a.Right,b.Right) && Math.Max(a.Top,b.Top) < Math.Min(a.Bottom,b.Bottom);
    }

    public static GateCWindowIdentity Identity(long value) {
        IntPtr hwnd = new IntPtr(value);
        if (!IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) throw new InvalidOperationException("HWND is not a top-level window");
        uint pid;
        uint tid = GetWindowThreadProcessId(hwnd, out pid);
        return new GateCWindowIdentity { Hwnd=value, Pid=(int)pid, Tid=(int)tid, ClassName=ClassOf(hwnd) };
    }
    public static WINDOWPLACEMENT Placement(long value) {
        WINDOWPLACEMENT result = new WINDOWPLACEMENT();
        result.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
        if (!GetWindowPlacement(new IntPtr(value), ref result)) throw new InvalidOperationException("GetWindowPlacement failed");
        return result;
    }
    public static void Resize(long value, int widthDelta, int heightDelta) {
        GateCRect rect = RectOf(new IntPtr(value));
        if (!SetWindowPos(new IntPtr(value), IntPtr.Zero, rect.Left, rect.Top,
                Math.Max(640, rect.Right-rect.Left+widthDelta), Math.Max(480, rect.Bottom-rect.Top+heightDelta),
                SWP_NOZORDER|SWP_NOACTIVATE)) throw new InvalidOperationException("SetWindowPos failed");
    }
    public static void Restore(long value, WINDOWPLACEMENT placement) {
        if (IsWindow(new IntPtr(value)) && !SetWindowPlacement(new IntPtr(value), ref placement))
            throw new InvalidOperationException("SetWindowPlacement failed");
    }
    public static void Close(long value) {
        IntPtr hwnd = new IntPtr(value);
        if (IsWindow(hwnd) && !PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("PostMessage(WM_CLOSE) failed");
    }
    public static bool Exists(long value) { return IsWindow(new IntPtr(value)); }
    public static int ExactPaneCount(long value) {
        int count = 0;
        EnumWindowsProc callback = delegate(IntPtr child, IntPtr unused) {
            try { if (ClassOf(child) == "WinExInfo.StatusPane") count++; } catch { }
            return true;
        };
        if (!EnumChildWindows(new IntPtr(value), callback, IntPtr.Zero))
            throw new InvalidOperationException("EnumChildWindows failed");
        GC.KeepAlive(callback);
        return count;
    }

    public static GateCLayout Capture(long topValue) {
        IntPtr top = new IntPtr(topValue);
        AutomationElement root = AutomationElement.FromHandle(top);
        AutomationElement status = Exact(root, TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"DirectUI"),
            Property(AutomationElement.ControlTypeProperty,ControlType.StatusBar),
            Property(AutomationElement.AutomationIdProperty,"StatusBarModuleInner"),
            Property(AutomationElement.ClassNameProperty,"StatusBarModuleInner"),
            Property(AutomationElement.NativeWindowHandleProperty,0));
        AutomationElement left = Exact(status, TreeScope.Children,
            Property(AutomationElement.ControlTypeProperty,ControlType.Group),
            Property(AutomationElement.AutomationIdProperty,"System.StatusBarViewItemCount"),
            Property(AutomationElement.NativeWindowHandleProperty,0));
        AutomationElement right = Exact(status, TreeScope.Children,
            Property(AutomationElement.ControlTypeProperty,ControlType.Group),
            Property(AutomationElement.AutomationIdProperty,"ViewButtonsGroup"),
            Property(AutomationElement.NativeWindowHandleProperty,0));
        AutomationElement pane = Exact(root, TreeScope.Descendants,
            Property(AutomationElement.ClassNameProperty,"WinExInfo.StatusPane"));
        IntPtr paneHwnd = new IntPtr(pane.Current.NativeWindowHandle);
        IntPtr parentHwnd = GetParent(paneHwnd);
        uint panePid;
        uint paneTid = GetWindowThreadProcessId(paneHwnd, out panePid);
        GateCRect parentRect = RectOf(parentHwnd);
        GateCRect statusRect = BoundsOf(status), leftRect = BoundsOf(left), rightRect = BoundsOf(right), paneRect = RectOf(paneHwnd);
        int dpi = (int)GetDpiForWindow(parentHwnd);
        int margin = (8*dpi+48)/96;
        GateCRect expected = new GateCRect {
            Left=Math.Max(leftRect.Right+margin, Math.Max(parentRect.Left,statusRect.Left)),
            Top=Math.Max(parentRect.Top,statusRect.Top),
            Right=Math.Min(rightRect.Left-margin, Math.Min(parentRect.Right,statusRect.Right)),
            Bottom=Math.Min(parentRect.Bottom,statusRect.Bottom)
        };
        return new GateCLayout {
            PaneHwnd=paneHwnd.ToInt64(), ParentHwnd=parentHwnd.ToInt64(), PanePid=(int)panePid, PaneTid=(int)paneTid,
            PaneClass=ClassOf(paneHwnd), PaneText=TextOf(paneHwnd), ParentClass=ClassOf(parentHwnd),
            Parent=parentRect, Status=statusRect, LeftGroup=leftRect, RightGroup=rightRect, Pane=paneRect,
            Expected=expected, Dpi=dpi, Overlap=Intersects(paneRect,leftRect)||Intersects(paneRect,rightRect)
        };
    }
    public static int AddAndSwitchTab(long topValue) {
        AutomationElement root = AutomationElement.FromHandle(new IntPtr(topValue));
        AutomationElement tabView = Exact(root, TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"XAML"),
            Property(AutomationElement.ControlTypeProperty,ControlType.Tab),
            Property(AutomationElement.AutomationIdProperty,"TabView"),
            Property(AutomationElement.ClassNameProperty,"Microsoft.UI.Xaml.Controls.TabView"));
        AutomationElement list = Exact(tabView, TreeScope.Children,
            Property(AutomationElement.FrameworkIdProperty,"XAML"),
            Property(AutomationElement.ControlTypeProperty,ControlType.List),
            Property(AutomationElement.AutomationIdProperty,"TabListView"),
            Property(AutomationElement.ClassNameProperty,"ListView"));
        Condition itemCondition = new AndCondition(
            Property(AutomationElement.FrameworkIdProperty,"XAML"),
            Property(AutomationElement.ControlTypeProperty,ControlType.TabItem),
            Property(AutomationElement.ClassNameProperty,"ListViewItem"));
        AutomationElementCollection before = list.FindAll(TreeScope.Children,itemCondition);
        if (before.Count != 1) throw new InvalidOperationException("Controlled window did not begin with exactly one tab");
        AutomationElement add = Exact(root, TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"XAML"),
            Property(AutomationElement.ControlTypeProperty,ControlType.Button),
            Property(AutomationElement.AutomationIdProperty,"AddButton"),
            Property(AutomationElement.ClassNameProperty,"Button"));
        ((InvokePattern)add.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Stopwatch wait = Stopwatch.StartNew();
        AutomationElementCollection after;
        do { Sleep(25); after=list.FindAll(TreeScope.Children,itemCondition); } while(after.Count!=2 && wait.ElapsedMilliseconds<3000);
        if (after.Count != 2) throw new InvalidOperationException("Tab add did not produce exactly two direct tab items");
        ((SelectionItemPattern)after[0].GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        Sleep(250);
        ((SelectionItemPattern)after[1].GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        Sleep(250);
        return after.Count;
    }
    public static void RestoreSingleTab(long topValue) {
        AutomationElement root = AutomationElement.FromHandle(new IntPtr(topValue));
        AutomationElement tabView = Exact(root, TreeScope.Descendants,
            Property(AutomationElement.ControlTypeProperty,ControlType.Tab), Property(AutomationElement.AutomationIdProperty,"TabView"),
            Property(AutomationElement.ClassNameProperty,"Microsoft.UI.Xaml.Controls.TabView"));
        AutomationElement list = Exact(tabView, TreeScope.Children,
            Property(AutomationElement.AutomationIdProperty,"TabListView"), Property(AutomationElement.ClassNameProperty,"ListView"));
        Condition itemCondition = new AndCondition(Property(AutomationElement.ControlTypeProperty,ControlType.TabItem),Property(AutomationElement.ClassNameProperty,"ListViewItem"));
        AutomationElementCollection items=list.FindAll(TreeScope.Children,itemCondition);
        if(items.Count!=2) throw new InvalidOperationException("Expected two tabs before restore");
        ((SelectionItemPattern)items[1].GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        Sleep(150);
        AutomationElement close = Exact(items[1], TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"XAML"), Property(AutomationElement.ControlTypeProperty,ControlType.Button),
            Property(AutomationElement.AutomationIdProperty,"CloseButton"), Property(AutomationElement.ClassNameProperty,"Button"));
        ((InvokePattern)close.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Stopwatch wait=Stopwatch.StartNew();
        do { Sleep(25); items=list.FindAll(TreeScope.Children,itemCondition); } while(items.Count!=1 && wait.ElapsedMilliseconds<3000);
        if(items.Count!=1) throw new InvalidOperationException("Tab restore did not return to one tab");
    }
}
'@

function Get-ShellWindows {
    $shell = New-Object -ComObject Shell.Application
    try {
        @($shell.Windows() | ForEach-Object {
            try {
                $hwnd = [long]$_.HWND
                $identity = [GateCHelper]::Identity($hwnd)
                if ($identity.ClassName -ceq 'CabinetWClass') {
                    [pscustomobject]@{ Hwnd=$hwnd; Pid=$identity.Pid; Tid=$identity.Tid; LocationUrl=[string]$_.LocationURL }
                }
            }
            catch { }
        })
    }
    finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
}

function Get-SafetyState {
    $hostProcesses = @(Get-Process -Name WinExInfoHost,WinExInfoTests -ErrorAction SilentlyContinue)
    $runPresent = Test-Path -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $runValue = $null
    if ($runPresent) { $runValue = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name WinExInfo -ErrorAction SilentlyContinue).WinExInfo }
    $serviceCount = @(Get-Service -Name WinExInfo -ErrorAction SilentlyContinue).Count
    $taskCount = @(Get-ScheduledTask -TaskName WinExInfo -ErrorAction SilentlyContinue).Count
    $tcpCount = 0
    if ($hostProcesses.Count -gt 0) {
        $ids = @($hostProcesses.Id)
        $tcpCount = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $ids -contains $_.OwningProcess }).Count
    }
    [pscustomobject]@{ ExplorerWindows=@(Get-ShellWindows).Count; ExplorerPids=@(Get-Process explorer -ErrorAction SilentlyContinue).Count; RunCount=[int]($null -ne $runValue); ServiceCount=$serviceCount; TaskCount=$taskCount; WinExInfoProcessCount=$hostProcesses.Count; TcpCount=$tcpCount }
}

function Wait-Until([scriptblock]$Condition, [int]$TimeoutMs, [string]$Failure) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $result = & $Condition
        if ($null -ne $result -and $result -ne $false) { return $result }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Failure
}

function Assert-Layout([GateCLayout]$Layout, [GateCWindowIdentity]$Target, [string]$Stage) {
    if ($Layout.PanePid -ne $Target.Pid -or $Layout.PaneTid -ne $Target.Tid -or
        $Layout.PaneClass -cne 'WinExInfo.StatusPane' -or $Layout.PaneText -cne 'WinExInfo Gate B' -or
        $Layout.ParentClass -cne 'DUIViewWndClassName' -or $Layout.Overlap -or
        $Layout.Pane.ToString() -cne $Layout.Expected.ToString()) {
        throw "Exact pane contract failed at $Stage"
    }
    Write-Output "LAYOUT stage=$Stage pane_hwnd=0x$($Layout.PaneHwnd.ToString('X16')) parent_hwnd=0x$($Layout.ParentHwnd.ToString('X16')) pid=$($Layout.PanePid) tid=$($Layout.PaneTid) class=$($Layout.PaneClass) text=$($Layout.PaneText -replace ' ','_') dpi=$($Layout.Dpi) parent=$($Layout.Parent) status=$($Layout.Status) left=$($Layout.LeftGroup) right=$($Layout.RightGroup) pane=$($Layout.Pane) expected=$($Layout.Expected) overlap=$($Layout.Overlap.ToString().ToLowerInvariant())"
}

$before = Get-SafetyState
$beforeWindows = @(Get-ShellWindows)
$controlled = $null
$originalPlacement = $null
$hostProcess = $null
$tabAdded = $false
$failure = $null
Remove-Item -LiteralPath $stdoutPath,$stderrPath -ErrorAction SilentlyContinue

try {
    Start-Process -FilePath "$env:WINDIR\explorer.exe" -ArgumentList @('/n,', $root)
    $controlled = Wait-Until -TimeoutMs 5000 -Failure 'Exactly one controlled Explorer window was not created.' -Condition {
        $now = @(Get-ShellWindows)
        $delta = @($now | Where-Object { $candidate=$_; -not ($beforeWindows | Where-Object Hwnd -EQ $candidate.Hwnd) })
        if ($delta.Count -eq 1 -and $delta[0].LocationUrl -ceq 'file:///D:/PROJECTS/WinExInfo') { $delta[0] }
    }
    $target = [GateCHelper]::Identity($controlled.Hwnd)
    if ($target.ClassName -cne 'CabinetWClass' -or $target.Pid -ne $controlled.Pid -or $target.Tid -ne $controlled.Tid) { throw 'Controlled target identity changed.' }
    $originalPlacement = [GateCHelper]::Placement($controlled.Hwnd)
    Write-Output "CONTROLLED hwnd=0x$($controlled.Hwnd.ToString('X16')) pid=$($target.Pid) tid=$($target.Tid) class=$($target.ClassName) url=$($controlled.LocationUrl)"
    Write-Output "SAFETY stage=before explorer_windows=$($before.ExplorerWindows) explorer_pids=$($before.ExplorerPids) run=$($before.RunCount) service=$($before.ServiceCount) task=$($before.TaskCount) processes=$($before.WinExInfoProcessCount) tcp=$($before.TcpCount)"

    $hwndText = '0x' + $controlled.Hwnd.ToString('X16')
    $hostProcess = Start-Process -FilePath $hostExecutable -ArgumentList @('--gate-c-place','--hwnd',$hwndText,'--duration-ms','15000') -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
    $initial = Wait-Until -TimeoutMs 5000 -Failure 'Pane did not reach exact initial state within 5000ms.' -Condition {
        try { [GateCHelper]::Capture($controlled.Hwnd) } catch { $null }
    }
    Assert-Layout $initial $target 'initial'

    [GateCHelper]::Resize($controlled.Hwnd, -160, -90)
    $resized = Wait-Until -TimeoutMs 3000 -Failure 'Pane did not reflow after resize within 3000ms.' -Condition {
        try { $value=[GateCHelper]::Capture($controlled.Hwnd); if ($value.Pane.ToString() -ne $initial.Pane.ToString()) { $value } } catch { $null }
    }
    Assert-Layout $resized $target 'resized'

    $tabCount = [GateCHelper]::AddAndSwitchTab($controlled.Hwnd)
    $tabAdded = $true
    $switched = Wait-Until -TimeoutMs 3000 -Failure 'Pane did not reach exact state after tab switch within 3000ms.' -Condition {
        try { [GateCHelper]::Capture($controlled.Hwnd) } catch { $null }
    }
    Assert-Layout $switched $target 'tab_switched'
    Write-Output "TAB_TRANSITION before=1 after_add=$tabCount selected_original=true selected_added=true"

    [GateCHelper]::RestoreSingleTab($controlled.Hwnd)
    $tabAdded = $false
    [GateCHelper]::Restore($controlled.Hwnd, $originalPlacement)
    $restored = Wait-Until -TimeoutMs 3000 -Failure 'Pane did not reach exact restored state within 3000ms.' -Condition {
        try { [GateCHelper]::Capture($controlled.Hwnd) } catch { $null }
    }
    Assert-Layout $restored $target 'restored'

    if (-not $hostProcess.WaitForExit(10000)) { throw 'Host exceeded its bounded completion wait.' }
    $hostExit = $hostProcess.ExitCode
    $hostOut = @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
    $hostErr = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
    $accepted = "TARGET_ACCEPTED protocol=1 pid=$($target.Pid) tid=$($target.Tid) hwnd=$hwndText"
    if ($hostExit -ne 0 -or @($hostOut | Where-Object { $_ -ceq $accepted }).Count -ne 1 -or $hostErr.Count -ne 0) {
        throw "Host contract failed: exit=$hostExit stdout=$($hostOut -join '|') stderr=$($hostErr -join '|')"
    }
    $paneAbsent = Wait-Until -TimeoutMs 5000 -Failure 'Pane cleanup was not observed within 5000ms.' -Condition {
        ([GateCHelper]::ExactPaneCount($controlled.Hwnd) -eq 0)
    }
    $moduleCount = @(Get-Process -Id $target.Pid -ErrorAction Stop | Select-Object -ExpandProperty Modules | Where-Object { $_.FileName -ceq $hookDll }).Count
    if (-not $paneAbsent -or $moduleCount -ne 0) { throw "Cleanup contract failed: pane_absent=$paneAbsent module_count=$moduleCount" }
    Write-Output $accepted
    Write-Output "CLEANUP pane_absent=true exact_module_count=$moduleCount host_exit=$hostExit"
}
catch {
    $failure = $_
}
finally {
    if ($tabAdded -and $null -ne $controlled -and [GateCHelper]::Exists($controlled.Hwnd)) {
        try { [GateCHelper]::RestoreSingleTab($controlled.Hwnd) } catch { }
    }
    if ($null -ne $originalPlacement -and $null -ne $controlled -and [GateCHelper]::Exists($controlled.Hwnd)) {
        try { [GateCHelper]::Restore($controlled.Hwnd, $originalPlacement) } catch { }
    }
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
        Stop-Process -Id $hostProcess.Id -Force -ErrorAction SilentlyContinue
        [void]$hostProcess.WaitForExit(5000)
    }
    if ($null -ne $controlled -and [GateCHelper]::Exists($controlled.Hwnd)) {
        [GateCHelper]::Close($controlled.Hwnd)
        try { [void](Wait-Until -TimeoutMs 5000 -Failure 'Controlled Explorer window did not close within 5000ms.' -Condition { -not [GateCHelper]::Exists($controlled.Hwnd) }) } catch { if ($null -eq $failure) { throw } }
    }
    Remove-Item -LiteralPath $stdoutPath,$stderrPath -ErrorAction SilentlyContinue
}

$after = Get-SafetyState
Write-Output "SAFETY stage=after explorer_windows=$($after.ExplorerWindows) explorer_pids=$($after.ExplorerPids) run=$($after.RunCount) service=$($after.ServiceCount) task=$($after.TaskCount) processes=$($after.WinExInfoProcessCount) tcp=$($after.TcpCount)"
if ($null -ne $failure) {
    Write-Output "GATE_C_FAIL configuration=$Configuration reason=$($failure.Exception.Message -replace ' ','_')"
    throw $failure
}
if ($after.ExplorerWindows -ne $before.ExplorerWindows -or $after.RunCount -ne $before.RunCount -or
    $after.ServiceCount -ne $before.ServiceCount -or $after.TaskCount -ne $before.TaskCount -or
    $after.WinExInfoProcessCount -ne $before.WinExInfoProcessCount -or $after.TcpCount -ne $before.TcpCount) {
    Write-Output "GATE_C_FAIL configuration=$Configuration reason=safety_state_mismatch"
    throw 'Safety state did not return to the before baseline.'
}
Write-Output "GATE_C_PASS configuration=$Configuration pane_owner=explorer overlap=false cleanup=true"
