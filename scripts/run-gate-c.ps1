[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [Parameter(DontShow)]
    [switch]$ExportHelpers
)

$ErrorActionPreference = 'Stop'
$root = 'D:\PROJECTS\WinExInfo'
$configurationName = $Configuration.ToLowerInvariant()
$binaryDirectory = Join-Path $PSScriptRoot "..\out\build\windows-x64-$configurationName\bin"
$hostExecutable = Join-Path $binaryDirectory 'WinExInfoHost.exe'
$hookDll = Join-Path $binaryDirectory 'WinExInfoHook.dll'
$stdoutPath = Join-Path $env:TEMP "winexinfo-gate-c-$Configuration-$PID.stdout"
$stderrPath = Join-Path $env:TEMP "winexinfo-gate-c-$Configuration-$PID.stderr"
$hostDurationMs = 15000
$hostCleanupBudgetMs = 10000

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
    public long ActiveTabHwnd;
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

public sealed class GateCTabTransition {
    public int Count;
    public string OriginalIdentity;
    public string AddedIdentity;
    public bool OriginalSelectedObserved;
    public bool AddedSelectedObserved;
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
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT value);
    [DllImport("user32.dll")] private static extern bool SetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT value);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr hwnd, EnumWindowsProc callback, IntPtr value);
    [DllImport("kernel32.dll")] private static extern void Sleep(uint milliseconds);

    private const uint GA_ROOT = 2;
    private const uint GW_CHILD = 5;
    private const uint GW_HWNDNEXT = 2;
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
    private static AutomationElement Exact(string label, AutomationElement root, TreeScope scope, params Condition[] conditions) {
        Condition condition = conditions.Length == 1 ? conditions[0] : new AndCondition(conditions);
        AutomationElementCollection matches = root.FindAll(scope, condition);
        if (matches.Count != 1) throw new InvalidOperationException(label + "_cardinality=" + matches.Count);
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
    public static void CloseExact(long value, int expectedPid, int expectedTid, string expectedClass) {
        IntPtr hwnd = new IntPtr(value);
        if(!IsWindow(hwnd)||GetAncestor(hwnd,GA_ROOT)!=hwnd) throw new InvalidOperationException("close_target_not_top_level");
        uint pid;
        uint tid=GetWindowThreadProcessId(hwnd,out pid);
        if(pid!=(uint)expectedPid||tid!=(uint)expectedTid||ClassOf(hwnd)!=expectedClass)
            throw new InvalidOperationException("close_target_identity_mismatch");
        if (!PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("PostMessage(WM_CLOSE) failed");
    }
    public static bool Exists(long value) { return IsWindow(new IntPtr(value)); }
    public static int ExactPaneCount(long value) {
        int count = 0;
        string failure = null;
        EnumWindowsProc callback = delegate(IntPtr child, IntPtr unused) {
            try { if (ClassOf(child) == "WinExInfo.StatusPane") count++; }
            catch(Exception error) { failure=error.Message; return false; }
            return true;
        };
        bool enumerated=EnumChildWindows(new IntPtr(value), callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        if(failure!=null) throw new InvalidOperationException("pane_class_query_failed="+failure);
        if (!enumerated)
            throw new InvalidOperationException("EnumChildWindows failed");
        return count;
    }

    private static string RuntimeIdentity(AutomationElement element) {
        int[] value=element.GetRuntimeId();
        if(value==null||value.Length==0) throw new InvalidOperationException("tab_runtime_identity_missing");
        StringBuilder result=new StringBuilder();
        for(int index=0;index<value.Length;index++) {
            if(index!=0) result.Append('.');
            result.Append(value[index]);
        }
        return result.ToString();
    }

    private static string DirectChildZOrder(IntPtr top) {
        StringBuilder result = new StringBuilder();
        for (IntPtr child=GetWindow(top,GW_CHILD); child!=IntPtr.Zero; child=GetWindow(child,GW_HWNDNEXT))
            result.Append(child.ToInt64().ToString("X16")).Append(';');
        return result.ToString();
    }
    private static IntPtr ActiveView(IntPtr top, out IntPtr activeTab, out string before) {
        before=DirectChildZOrder(top);
        activeTab=IntPtr.Zero;
        for(IntPtr child=GetWindow(top,GW_CHILD); child!=IntPtr.Zero; child=GetWindow(child,GW_HWNDNEXT)) {
            if(ClassOf(child)=="ShellTabWindowClass" && IsWindowVisible(child)) { activeTab=child; break; }
        }
        if(activeTab==IntPtr.Zero) throw new InvalidOperationException("active_tab_hwnd=0");
        int viewCount=0;
        IntPtr view=IntPtr.Zero;
        EnumWindowsProc callback=delegate(IntPtr child, IntPtr unused) {
            if(ClassOf(child)=="DUIViewWndClassName") { viewCount++; view=child; }
            return true;
        };
        if(!EnumChildWindows(activeTab,callback,IntPtr.Zero)) throw new InvalidOperationException("active_view_enumeration_failed");
        GC.KeepAlive(callback);
        if(viewCount!=1) throw new InvalidOperationException("active_tab_hwnd=0x"+activeTab.ToInt64().ToString("X16")+" active_view_cardinality="+viewCount);
        if(!IsWindowVisible(activeTab)||!IsWindowVisible(view)) throw new InvalidOperationException("active_tab_or_view_not_visible");
        return view;
    }

    public static GateCLayout Capture(long topValue) {
        IntPtr top = new IntPtr(topValue);
        uint topPid;
        uint topTid=GetWindowThreadProcessId(top,out topPid);
        IntPtr activeTab;
        string before;
        IntPtr activeView=ActiveView(top,out activeTab,out before);
        AutomationElement root = AutomationElement.FromHandle(activeView);
        AutomationElement status = Exact("status",root, TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"DirectUI"),
            Property(AutomationElement.ControlTypeProperty,ControlType.StatusBar),
            Property(AutomationElement.AutomationIdProperty,"StatusBarModuleInner"),
            Property(AutomationElement.ClassNameProperty,"StatusBarModuleInner"),
            Property(AutomationElement.NativeWindowHandleProperty,0));
        AutomationElement left = Exact("left_group",status, TreeScope.Children,
            Property(AutomationElement.ControlTypeProperty,ControlType.Group),
            Property(AutomationElement.AutomationIdProperty,"System.StatusBarViewItemCount"),
            Property(AutomationElement.NativeWindowHandleProperty,0));
        AutomationElement right = Exact("right_group",status, TreeScope.Children,
            Property(AutomationElement.ControlTypeProperty,ControlType.Group),
            Property(AutomationElement.AutomationIdProperty,"ViewButtonsGroup"),
            Property(AutomationElement.NativeWindowHandleProperty,0));
        int paneCount=0;
        IntPtr paneHwnd=IntPtr.Zero;
        EnumWindowsProc paneCallback=delegate(IntPtr child, IntPtr unused) {
            if(ClassOf(child)=="WinExInfo.StatusPane") { paneCount++; paneHwnd=child; }
            return true;
        };
        if(!EnumChildWindows(top,paneCallback,IntPtr.Zero)) throw new InvalidOperationException("pane_enumeration_failed");
        GC.KeepAlive(paneCallback);
        if(paneCount!=1) throw new InvalidOperationException("active_tab_hwnd=0x"+activeTab.ToInt64().ToString("X16")+" active_view_hwnd=0x"+activeView.ToInt64().ToString("X16")+" pane_cardinality="+paneCount);
        IntPtr parentHwnd = GetParent(paneHwnd);
        uint panePid;
        uint paneTid = GetWindowThreadProcessId(paneHwnd, out panePid);
        uint parentPid;
        uint parentTid=GetWindowThreadProcessId(parentHwnd,out parentPid);
        string after=DirectChildZOrder(top);
        if(before!=after) throw new InvalidOperationException("active_z_order_changed");
        if(parentHwnd!=activeView || ClassOf(parentHwnd)!="DUIViewWndClassName" ||
           !IsWindowVisible(activeTab)||!IsWindowVisible(parentHwnd)||!IsWindowVisible(paneHwnd) ||
           panePid!=topPid||parentPid!=topPid||paneTid!=topTid||parentTid!=topTid)
            throw new InvalidOperationException("active_tab_hwnd=0x"+activeTab.ToInt64().ToString("X16")+" active_view_hwnd=0x"+activeView.ToInt64().ToString("X16")+" pane_parent_hwnd=0x"+parentHwnd.ToInt64().ToString("X16")+" active_parent_or_identity_mismatch");
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
            ActiveTabHwnd=activeTab.ToInt64(), PaneHwnd=paneHwnd.ToInt64(), ParentHwnd=parentHwnd.ToInt64(), PanePid=(int)panePid, PaneTid=(int)paneTid,
            PaneClass=ClassOf(paneHwnd), PaneText=TextOf(paneHwnd), ParentClass=ClassOf(parentHwnd),
            Parent=parentRect, Status=statusRect, LeftGroup=leftRect, RightGroup=rightRect, Pane=paneRect,
            Expected=expected, Dpi=dpi, Overlap=Intersects(paneRect,leftRect)||Intersects(paneRect,rightRect)
        };
    }
    public static GateCTabTransition AddAndSwitchTab(long topValue) {
        AutomationElement root = AutomationElement.FromHandle(new IntPtr(topValue));
        AutomationElement tabView = Exact("tab_view",root, TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"XAML"),
            Property(AutomationElement.ControlTypeProperty,ControlType.Tab),
            Property(AutomationElement.AutomationIdProperty,"TabView"),
            Property(AutomationElement.ClassNameProperty,"Microsoft.UI.Xaml.Controls.TabView"));
        AutomationElement list = Exact("tab_list",tabView, TreeScope.Children,
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
        string originalIdentity=RuntimeIdentity(before[0]);
        AutomationElement add = Exact("add_button",root, TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"XAML"),
            Property(AutomationElement.ControlTypeProperty,ControlType.Button),
            Property(AutomationElement.AutomationIdProperty,"AddButton"),
            Property(AutomationElement.ClassNameProperty,"Button"));
        ((InvokePattern)add.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Stopwatch wait = Stopwatch.StartNew();
        AutomationElementCollection after;
        do { Sleep(25); after=list.FindAll(TreeScope.Children,itemCondition); } while(after.Count!=2 && wait.ElapsedMilliseconds<3000);
        if (after.Count != 2) throw new InvalidOperationException("Tab add did not produce exactly two direct tab items");
        string firstIdentity=RuntimeIdentity(after[0]);
        string secondIdentity=RuntimeIdentity(after[1]);
        if(firstIdentity!=originalIdentity||secondIdentity==originalIdentity) throw new InvalidOperationException("tab_runtime_identity_transition_invalid");
        SelectionItemPattern original=(SelectionItemPattern)after[0].GetCurrentPattern(SelectionItemPattern.Pattern);
        SelectionItemPattern added=(SelectionItemPattern)after[1].GetCurrentPattern(SelectionItemPattern.Pattern);
        original.Select();
        Sleep(250);
        bool originalSelected=original.Current.IsSelected;
        if(!originalSelected) throw new InvalidOperationException("original_tab_selection_not_observed");
        added.Select();
        Sleep(250);
        bool addedSelected=added.Current.IsSelected;
        if(!addedSelected) throw new InvalidOperationException("added_tab_selection_not_observed");
        return new GateCTabTransition { Count=after.Count, OriginalIdentity=originalIdentity,
            AddedIdentity=secondIdentity, OriginalSelectedObserved=originalSelected,
            AddedSelectedObserved=addedSelected };
    }
    public static string RestoreSingleTab(long topValue) {
        AutomationElement root = AutomationElement.FromHandle(new IntPtr(topValue));
        AutomationElement tabView = Exact("tab_view",root, TreeScope.Descendants,
            Property(AutomationElement.ControlTypeProperty,ControlType.Tab), Property(AutomationElement.AutomationIdProperty,"TabView"),
            Property(AutomationElement.ClassNameProperty,"Microsoft.UI.Xaml.Controls.TabView"));
        AutomationElement list = Exact("tab_list",tabView, TreeScope.Children,
            Property(AutomationElement.AutomationIdProperty,"TabListView"), Property(AutomationElement.ClassNameProperty,"ListView"));
        Condition itemCondition = new AndCondition(Property(AutomationElement.ControlTypeProperty,ControlType.TabItem),Property(AutomationElement.ClassNameProperty,"ListViewItem"));
        AutomationElementCollection items=list.FindAll(TreeScope.Children,itemCondition);
        if(items.Count!=2) throw new InvalidOperationException("Expected two tabs before restore");
        ((SelectionItemPattern)items[1].GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        Sleep(150);
        AutomationElement close = Exact("close_button",items[1], TreeScope.Descendants,
            Property(AutomationElement.FrameworkIdProperty,"XAML"), Property(AutomationElement.ControlTypeProperty,ControlType.Button),
            Property(AutomationElement.AutomationIdProperty,"CloseButton"), Property(AutomationElement.ClassNameProperty,"Button"));
        ((InvokePattern)close.GetCurrentPattern(InvokePattern.Pattern)).Invoke();
        Stopwatch wait=Stopwatch.StartNew();
        do { Sleep(25); items=list.FindAll(TreeScope.Children,itemCondition); } while(items.Count!=1 && wait.ElapsedMilliseconds<3000);
        if(items.Count!=1) throw new InvalidOperationException("Tab restore did not return to one tab");
        SelectionItemPattern remaining=(SelectionItemPattern)items[0].GetCurrentPattern(SelectionItemPattern.Pattern);
        if(!remaining.Current.IsSelected) throw new InvalidOperationException("restored_tab_selection_not_observed");
        return RuntimeIdentity(items[0]);
    }
}
'@

function Get-ShellWindows {
    $shell = New-Object -ComObject Shell.Application
    try {
        @($shell.Windows() | ForEach-Object {
            $hwnd = [long]$_.HWND
            $identity = [GateCHelper]::Identity($hwnd)
            $url = [string]$_.LocationURL
            if ($identity.ClassName -ceq 'CabinetWClass') {
                [pscustomobject]@{ Hwnd=$hwnd; Pid=$identity.Pid; Tid=$identity.Tid; ClassName=$identity.ClassName; LocationUrl=$url }
            }
        })
    }
    finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
}

function Get-StableShellWindows {
    $first = @(Get-ShellWindows | Sort-Object Hwnd)
    Start-Sleep -Milliseconds 100
    $second = @(Get-ShellWindows | Sort-Object Hwnd)
    $firstSignature = @($first | ForEach-Object { "$($_.Hwnd)|$($_.Pid)|$($_.Tid)|$($_.ClassName)|$($_.LocationUrl)" }) -join ';'
    $secondSignature = @($second | ForEach-Object { "$($_.Hwnd)|$($_.Pid)|$($_.Tid)|$($_.ClassName)|$($_.LocationUrl)" }) -join ';'
    if ($firstSignature -cne $secondSignature) { throw "Shell window snapshot was not stable: first=$firstSignature second=$secondSignature" }
    return $second
}

function Assert-ControlledShellWindow($Expected) {
    $matches = @(Get-StableShellWindows | Where-Object Hwnd -EQ $Expected.Hwnd)
    if ($matches.Count -ne 1 -or $matches[0].Pid -ne $Expected.Pid -or
        $matches[0].Tid -ne $Expected.Tid -or $matches[0].ClassName -cne 'CabinetWClass' -or
        $matches[0].LocationUrl -cne 'file:///D:/PROJECTS/WinExInfo') {
        throw 'Controlled Shell window failed exact pre-close revalidation.'
    }
}

function Wait-ControlledShellWindow($Before, [int]$TimeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $previousCandidateSignature = $null
    do {
        $now = @(Get-ShellWindows | Sort-Object Hwnd)
        foreach ($existing in $Before) {
            $same = @($now | Where-Object { $_.Hwnd -eq $existing.Hwnd -and $_.Pid -eq $existing.Pid -and $_.Tid -eq $existing.Tid -and $_.ClassName -ceq $existing.ClassName -and $_.LocationUrl -ceq $existing.LocationUrl })
            if ($same.Count -ne 1) { throw "Before Shell identity set changed for hwnd=0x$($existing.Hwnd.ToString('X16'))" }
        }
        $delta = @($now | Where-Object { $candidate=$_; -not ($Before | Where-Object Hwnd -EQ $candidate.Hwnd) })
        if ($delta.Count -gt 1) { throw "Ambiguous controlled Explorer delta count=$($delta.Count)" }
        if ($delta.Count -eq 1) {
            if ($delta[0].ClassName -cne 'CabinetWClass' -or $delta[0].LocationUrl -cne 'file:///D:/PROJECTS/WinExInfo') { throw 'New Shell delta did not have the exact controlled identity.' }
            $script:controlledCandidate = $delta[0]
            $signature = "$($delta[0].Hwnd)|$($delta[0].Pid)|$($delta[0].Tid)|$($delta[0].ClassName)|$($delta[0].LocationUrl)"
            if ($signature -ceq $previousCandidateSignature) { return $delta[0] }
            $previousCandidateSignature = $signature
        }
        else { $previousCandidateSignature = $null }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Exactly one stable controlled Explorer window was not created.'
}

function Get-SafetyState {
    $allProcesses = @(Get-Process -ErrorAction Stop)
    $hostProcesses = @($allProcesses | Where-Object { $_.ProcessName -ceq 'WinExInfoHost' -or $_.ProcessName -ceq 'WinExInfoTests' })
    $explorerProcesses = @($allProcesses | Where-Object ProcessName -CEQ 'explorer')
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $runPresent = Test-Path -LiteralPath $runKey -ErrorAction Stop
    $runValue = $null
    if ($runPresent) {
        $runProperties = Get-ItemProperty -LiteralPath $runKey -ErrorAction Stop
        $runProperty = $runProperties.PSObject.Properties['WinExInfo']
        if ($null -ne $runProperty) { $runValue = $runProperty.Value }
    }
    $serviceCount = @(Get-Service -ErrorAction Stop | Where-Object Name -CEQ 'WinExInfo').Count
    $taskCount = @(Get-ScheduledTask -ErrorAction Stop | Where-Object TaskName -CEQ 'WinExInfo').Count
    $tcpCount = 0
    if ($hostProcesses.Count -gt 0) {
        $ids = @($hostProcesses.Id)
        $tcpCount = @(Get-NetTCPConnection -ErrorAction Stop | Where-Object { $ids -contains $_.OwningProcess }).Count
    }
    [pscustomobject]@{ ExplorerWindows=@(Get-StableShellWindows).Count; ExplorerPids=$explorerProcesses.Count; RunCount=[int]($null -ne $runValue); ServiceCount=$serviceCount; TaskCount=$taskCount; WinExInfoProcessCount=$hostProcesses.Count; TcpCount=$tcpCount }
}

function Get-ExactModuleCount([int]$ProcessId) {
    $process = @(Get-Process -ErrorAction Stop | Where-Object Id -EQ $ProcessId)
    if ($process.Count -eq 0) { return 0 }
    if ($process.Count -ne 1) { throw "Controlled process cardinality was $($process.Count)" }
    $modules = @($process[0].Modules)
    return @($modules | Where-Object { $_.FileName -ceq $hookDll }).Count
}

function Wait-Until([scriptblock]$Condition, [int]$TimeoutMs, [string]$Failure) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $result = & $Condition
        if ($null -ne $result -and $result -ne $false) { return $result }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "$Failure diagnostic=$script:lastLayoutDiagnostic"
}

function Assert-Layout([GateCLayout]$Layout, [GateCWindowIdentity]$Target, [string]$Stage) {
    if ($Layout.PanePid -ne $Target.Pid -or $Layout.PaneTid -ne $Target.Tid -or
        $Layout.PaneClass -cne 'WinExInfo.StatusPane' -or $Layout.PaneText -cne 'WinExInfo Gate B' -or
        $Layout.ParentClass -cne 'DUIViewWndClassName' -or $Layout.Overlap -or
        $Layout.Pane.ToString() -cne $Layout.Expected.ToString()) {
        throw "Exact pane contract failed at $Stage"
    }
    Write-Output "LAYOUT stage=$Stage active_tab_hwnd=0x$($Layout.ActiveTabHwnd.ToString('X16')) pane_hwnd=0x$($Layout.PaneHwnd.ToString('X16')) parent_hwnd=0x$($Layout.ParentHwnd.ToString('X16')) pid=$($Layout.PanePid) tid=$($Layout.PaneTid) class=$($Layout.PaneClass) text=$($Layout.PaneText -replace ' ','_') dpi=$($Layout.Dpi) parent=$($Layout.Parent) status=$($Layout.Status) left=$($Layout.LeftGroup) right=$($Layout.RightGroup) pane=$($Layout.Pane) expected=$($Layout.Expected) overlap=$($Layout.Overlap.ToString().ToLowerInvariant())"
}

function Assert-Equal([long]$Actual, [long]$Expected, [string]$Contract) {
    if ($Actual -ne $Expected) { throw "$Contract actual=0x$($Actual.ToString('X16')) expected=0x$($Expected.ToString('X16'))" }
}

function Assert-RectEqual($Actual, $Expected, [string]$Contract) {
    if ($Actual.ToString() -cne $Expected.ToString()) { throw "$Contract actual=$Actual expected=$Expected" }
}

if ($ExportHelpers) { return }

$before = Get-SafetyState
$beforeWindows = @(Get-StableShellWindows)
$controlled = $null
$script:controlledCandidate = $null
$originalPlacement = $null
$hostProcess = $null
$hostStartedAt = $null
$tabAdded = $false
$failure = $null
$cleanupDiagnostics = @()
$hostLogsEmitted = $false
$script:lastLayoutDiagnostic = 'none'
Remove-Item -LiteralPath $stdoutPath,$stderrPath -ErrorAction SilentlyContinue

try {
    Start-Process -FilePath "$env:WINDIR\explorer.exe" -ArgumentList @('/n,', $root)
    Start-Sleep -Milliseconds 500
    $controlled = Wait-ControlledShellWindow $beforeWindows 5000
    $target = [GateCHelper]::Identity($controlled.Hwnd)
    if ($target.ClassName -cne 'CabinetWClass' -or $target.Pid -ne $controlled.Pid -or $target.Tid -ne $controlled.Tid) { throw 'Controlled target identity changed.' }
    $originalPlacement = [GateCHelper]::Placement($controlled.Hwnd)
    Write-Output "CONTROLLED hwnd=0x$($controlled.Hwnd.ToString('X16')) pid=$($target.Pid) tid=$($target.Tid) class=$($target.ClassName) url=$($controlled.LocationUrl)"
    Write-Output "SAFETY stage=before explorer_windows=$($before.ExplorerWindows) explorer_pids=$($before.ExplorerPids) run=$($before.RunCount) service=$($before.ServiceCount) task=$($before.TaskCount) processes=$($before.WinExInfoProcessCount) tcp=$($before.TcpCount)"

    $hwndText = '0x' + $controlled.Hwnd.ToString('X16')
    $hostStartedAt = [Diagnostics.Stopwatch]::StartNew()
    $hostProcess = Start-Process -FilePath $hostExecutable -ArgumentList @('--gate-c-place','--hwnd',$hwndText,'--duration-ms',[string]$hostDurationMs) -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
    $initial = Wait-Until -TimeoutMs 5000 -Failure 'Pane did not reach exact initial state within 5000ms.' -Condition {
        try { $value=[GateCHelper]::Capture($controlled.Hwnd); $script:lastLayoutDiagnostic='none'; $value } catch { $script:lastLayoutDiagnostic=$_.Exception.Message; $null }
    }
    Assert-Layout $initial $target 'initial'

    [GateCHelper]::Resize($controlled.Hwnd, -160, -90)
    $resized = Wait-Until -TimeoutMs 3000 -Failure 'Pane did not reflow after resize within 3000ms.' -Condition {
        try { $value=[GateCHelper]::Capture($controlled.Hwnd); $script:lastLayoutDiagnostic='none'; if ($value.Pane.ToString() -ne $initial.Pane.ToString()) { $value } } catch { $script:lastLayoutDiagnostic=$_.Exception.Message; $null }
    }
    Assert-Layout $resized $target 'resized'
    Assert-Equal $resized.PaneHwnd $initial.PaneHwnd 'resize_pane_continuity'
    Assert-Equal $resized.ActiveTabHwnd $initial.ActiveTabHwnd 'resize_active_tab_continuity'
    Assert-Equal $resized.ParentHwnd $initial.ParentHwnd 'resize_parent_continuity'
    if ($resized.Pane.ToString() -ceq $initial.Pane.ToString()) { throw 'Resize did not change pane rectangle.' }

    $tabTransition = [GateCHelper]::AddAndSwitchTab($controlled.Hwnd)
    $tabAdded = $true
    $switched = Wait-Until -TimeoutMs 3000 -Failure 'Pane did not reach exact state after tab switch within 3000ms.' -Condition {
        try { $value=[GateCHelper]::Capture($controlled.Hwnd); $script:lastLayoutDiagnostic='none'; $value } catch { $script:lastLayoutDiagnostic=$_.Exception.Message; $null }
    }
    Assert-Layout $switched $target 'tab_switched'
    Assert-Equal $switched.PaneHwnd $initial.PaneHwnd 'switch_pane_continuity'
    if ($switched.ActiveTabHwnd -eq $initial.ActiveTabHwnd -or $switched.ParentHwnd -eq $initial.ParentHwnd) { throw 'Tab switch did not change exact active tab and parent.' }
    if (-not $tabTransition.OriginalSelectedObserved -or -not $tabTransition.AddedSelectedObserved) { throw 'Tab selection state was not observed.' }
    Write-Output "TAB_TRANSITION before=1 after_add=$($tabTransition.Count) original_identity=$($tabTransition.OriginalIdentity) added_identity=$($tabTransition.AddedIdentity) selected_original=$($tabTransition.OriginalSelectedObserved.ToString().ToLowerInvariant()) selected_added=$($tabTransition.AddedSelectedObserved.ToString().ToLowerInvariant())"

    $restoredTabIdentity = [GateCHelper]::RestoreSingleTab($controlled.Hwnd)
    $tabAdded = $false
    if ($restoredTabIdentity -cne $tabTransition.OriginalIdentity) { throw "Restored tab identity mismatch actual=$restoredTabIdentity expected=$($tabTransition.OriginalIdentity)" }
    [GateCHelper]::Restore($controlled.Hwnd, $originalPlacement)
    $restored = Wait-Until -TimeoutMs 3000 -Failure 'Pane did not reach exact restored state within 3000ms.' -Condition {
        try { $value=[GateCHelper]::Capture($controlled.Hwnd); $script:lastLayoutDiagnostic='none'; $value } catch { $script:lastLayoutDiagnostic=$_.Exception.Message; $null }
    }
    Assert-Layout $restored $target 'restored'
    Assert-Equal $restored.PaneHwnd $initial.PaneHwnd 'restore_pane_continuity'
    Assert-Equal $restored.ActiveTabHwnd $initial.ActiveTabHwnd 'restore_active_tab_continuity'
    Assert-Equal $restored.ParentHwnd $initial.ParentHwnd 'restore_parent_continuity'
    Assert-RectEqual $restored.Pane $initial.Pane 'restore_pane_rectangle'
    Assert-RectEqual $restored.Expected $initial.Expected 'restore_expected_rectangle'

    $hostElapsedMs = [int][Math]::Min([int]::MaxValue, $hostStartedAt.ElapsedMilliseconds)
    $hostRemainingMs = [Math]::Max(0, $hostDurationMs - $hostElapsedMs)
    $hostWaitBudgetMs = $hostRemainingMs + $hostCleanupBudgetMs
    if (-not $hostProcess.WaitForExit($hostWaitBudgetMs)) {
        throw "Host exceeded its bounded completion wait. elapsed_ms=$hostElapsedMs remaining_ms=$hostRemainingMs cleanup_budget_ms=$hostCleanupBudgetMs wait_budget_ms=$hostWaitBudgetMs"
    }
    $hostExit = $hostProcess.ExitCode
    $hostOut = @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
    $hostErr = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
    $hostOut | ForEach-Object { Write-Output "HOST_STDOUT $_" }
    $hostErr | ForEach-Object { Write-Output "HOST_STDERR $_" }
    $hostLogsEmitted = $true
    $accepted = "TARGET_ACCEPTED protocol=1 pid=$($target.Pid) tid=$($target.Tid) hwnd=$hwndText"
    if ($hostExit -ne 0 -or @($hostOut | Where-Object { $_ -ceq $accepted }).Count -ne 1 -or $hostErr.Count -ne 0) {
        throw "Host contract failed: exit=$hostExit stdout=$($hostOut -join '|') stderr=$($hostErr -join '|')"
    }
    $paneAbsent = Wait-Until -TimeoutMs 5000 -Failure 'Pane cleanup was not observed within 5000ms.' -Condition {
        ([GateCHelper]::ExactPaneCount($controlled.Hwnd) -eq 0)
    }
    [void](Wait-Until -TimeoutMs 5000 -Failure 'Exact module cleanup was not observed within 5000ms.' -Condition {
        $count = Get-ExactModuleCount $target.Pid
        if ($count -eq 0) { return 'zero' }
        $script:lastLayoutDiagnostic = "exact_module_count=$count"
        return $false
    })
    $moduleCount = 0
    if (-not $paneAbsent -or $moduleCount -ne 0) { throw "Cleanup contract failed: pane_absent=$paneAbsent module_count=$moduleCount" }
    Write-Output $accepted
    Write-Output "CLEANUP pane_absent=true exact_module_count=$moduleCount host_exit=$hostExit"
}
catch {
    $failure = $_
}
finally {
    if ($tabAdded -and $null -ne $controlled -and [GateCHelper]::Exists($controlled.Hwnd)) {
        try { [void][GateCHelper]::RestoreSingleTab($controlled.Hwnd) } catch { $cleanupDiagnostics += "restore_tab=$($_.Exception.Message)" }
    }
    if ($null -ne $originalPlacement -and $null -ne $controlled -and [GateCHelper]::Exists($controlled.Hwnd)) {
        try { [GateCHelper]::Restore($controlled.Hwnd, $originalPlacement) } catch { $cleanupDiagnostics += "restore_placement=$($_.Exception.Message)" }
    }
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
        try { [void]$hostProcess.WaitForExit(10000) } catch { $cleanupDiagnostics += "host_normal_wait=$($_.Exception.Message)" }
    }
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
        try { Stop-Process -Id $hostProcess.Id -Force -ErrorAction Stop } catch { $cleanupDiagnostics += "host_force_stop=$($_.Exception.Message)" }
        try { [void]$hostProcess.WaitForExit(5000) } catch { $cleanupDiagnostics += "host_force_wait=$($_.Exception.Message)" }
    }
    if (-not $hostLogsEmitted) {
        if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) { try { @(Get-Content -LiteralPath $stdoutPath -ErrorAction Stop) | ForEach-Object { Write-Output "HOST_STDOUT $_" } } catch { $cleanupDiagnostics += "host_stdout=$($_.Exception.Message)" } } else { Write-Output 'HOST_STDOUT <not-created>' }
        if (Test-Path -LiteralPath $stderrPath -PathType Leaf) { try { @(Get-Content -LiteralPath $stderrPath -ErrorAction Stop) | ForEach-Object { Write-Output "HOST_STDERR $_" } } catch { $cleanupDiagnostics += "host_stderr=$($_.Exception.Message)" } } else { Write-Output 'HOST_STDERR <not-created>' }
    }
    if ($null -eq $controlled -and $null -ne $script:controlledCandidate) { $controlled = $script:controlledCandidate }
    $closeAuthorized = $false
    if ($null -ne $controlled -and [GateCHelper]::Exists($controlled.Hwnd)) {
        try { Assert-ControlledShellWindow $controlled; $closeAuthorized=$true } catch { $cleanupDiagnostics += "close_revalidation=$($_.Exception.Message)" }
    }
    if ($closeAuthorized) {
        try { [GateCHelper]::CloseExact($controlled.Hwnd, $controlled.Pid, $controlled.Tid, 'CabinetWClass') } catch { $cleanupDiagnostics += "close=$($_.Exception.Message)" }
        try { [void](Wait-Until -TimeoutMs 5000 -Failure 'Controlled Explorer window did not close within 5000ms.' -Condition { -not [GateCHelper]::Exists($controlled.Hwnd) }) } catch { $cleanupDiagnostics += "close_wait=$($_.Exception.Message)" }
    }
    try { Remove-Item -LiteralPath $stdoutPath,$stderrPath -ErrorAction Stop } catch { if ((Test-Path -LiteralPath $stdoutPath) -or (Test-Path -LiteralPath $stderrPath)) { $cleanupDiagnostics += "temp_remove=$($_.Exception.Message)" } }
}

$after = $null
try { $after = Get-SafetyState } catch { $cleanupDiagnostics += "after_safety=$($_.Exception.Message)" }
if ($null -ne $after) { Write-Output "SAFETY stage=after explorer_windows=$($after.ExplorerWindows) explorer_pids=$($after.ExplorerPids) run=$($after.RunCount) service=$($after.ServiceCount) task=$($after.TaskCount) processes=$($after.WinExInfoProcessCount) tcp=$($after.TcpCount)" }
if ($null -ne $failure) {
    if ($cleanupDiagnostics.Count -ne 0) { Write-Output "CLEANUP_DIAGNOSTIC $($cleanupDiagnostics -join '|')" }
    Write-Output "GATE_C_FAIL configuration=$Configuration reason=$($failure.Exception.Message -replace ' ','_')"
    throw $failure
}
if ($cleanupDiagnostics.Count -ne 0) {
    Write-Output "CLEANUP_DIAGNOSTIC $($cleanupDiagnostics -join '|')"
    Write-Output "GATE_C_FAIL configuration=$Configuration reason=cleanup_diagnostic"
    throw "Cleanup failed: $($cleanupDiagnostics -join '|')"
}
if ($null -eq $after -or $after.ExplorerWindows -ne $before.ExplorerWindows -or $after.RunCount -ne $before.RunCount -or
    $after.ServiceCount -ne $before.ServiceCount -or $after.TaskCount -ne $before.TaskCount -or
    $after.WinExInfoProcessCount -ne $before.WinExInfoProcessCount -or $after.TcpCount -ne $before.TcpCount) {
    Write-Output "GATE_C_FAIL configuration=$Configuration reason=safety_state_mismatch"
    throw 'Safety state did not return to the before baseline.'
}
Write-Output "GATE_C_PASS configuration=$Configuration pane_owner=explorer overlap=false cleanup=true"
