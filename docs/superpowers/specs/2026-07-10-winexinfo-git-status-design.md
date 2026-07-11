# WinExInfo Git 상태표시줄 설계

- 작성일: 2026-07-10
- 대상 저장소: `D:\PROJECTS\WinExInfo`
- 검토 상태: 2026-07-12 전체 제품 구현과 현재 PC의 실제 Explorer E2E 실행을 승인받았다.

## 1. 목적

WinExInfo는 Windows 11 파일 탐색기의 기존 하단 상태표시줄에 현재 폴더가 속한 Git 저장소의 상태를 표시한다. 표시 컨트롤은 외부 오버레이가 아니라 대상 `explorer.exe` 프로세스 내부에서 생성되고 Explorer 창 계층에 소속되어야 한다.

첫 버전은 현재 형님 PC의 정확한 Windows 및 Explorer 빌드만 지원한다. 지원 계약이 달라지면 다른 구조를 추측하거나 대체 경로를 시도하지 않고 안전하게 주입을 건너뛴다.

## 2. 대상 환경

| 구분 | 확정 값 |
|---|---|
| Windows | Windows 11 Pro 25H2, `10.0.26200.8655` |
| CPU 및 프로세스 | AMD64, x64 전용 |
| `explorer.exe` | `10.0.26100.8655` |
| `ExplorerFrame.dll` | `10.0.26100.8655` |
| `shell32.dll` | `10.0.26100.8655` |
| Git | `C:\Program Files\Git\cmd\git.exe`, `2.49.0.windows.1` |
| 빌드 도구 | Visual Studio Build Tools 2022 17.14, CMake 3.29.2, Ninja 1.12.0 |
| Windows SDK | `10.0.26100.0` |
| Explorer mitigation | `DisableExtensionPoints=OFF`, `MicrosoftSignedOnly=OFF`, `CFG=ON`, `StrictCFG=OFF` |
| Explorer signature | Catalog signature, signer subject `CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US` |

위 세 Explorer 파일 버전은 문자열 `FileVersion`이 아니라
`VS_FIXEDFILEINFO.dwFileVersionMS/LS`를 exact source로 사용한다. Host는 이
값이 Windows 10/11 동작으로 가상화 없이 반환되도록 application manifest에
`supportedOS={8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}`를 내장한다.

현재 Explorer 창을 점검한 결과, 실제 하단 상태표시줄은 `DUIViewWndClassName` 아래의 UI Automation `StatusBar` 요소로 노출된다. 같은 창에 존재하는 `msctls_statusbar32`는 너비가 0인 레거시 컨트롤이므로 삽입 대상으로 사용하지 않는다.

## 3. 범위

### 포함

- 현재 PC의 파일 탐색기 프로세스에 x64 네이티브 DLL 주입
- 기존 항목 수 영역과 보기 모드 영역 사이에 Git 상태 컨트롤 추가
- 활성 탭의 파일 시스템 폴더 판정
- 현재 폴더에서 가장 가까운 상위 Git 작업 트리 판정
- 저장소 전체의 staged, unstaged, untracked, conflict 상태 집계
- 마지막으로 로컬에 저장된 upstream ref 기준 ahead/behind 계산
- 폴더 이동과 저장소 파일 변경에 따른 자동 갱신
- 여러 Explorer 창과 탭 지원
- 사용자 계정 범위 설치 및 로그인 자동 실행
- 명확한 호환성 검사, 오류 표시, 진단 로그, 제거 절차
- 실제 Explorer 주입 전에 종료 가능한 UI 배치 및 DLL unload POC

### 제외

- Windows 10, ARM64, 다른 Windows 또는 Explorer 빌드 지원
- 외부 프로세스가 Explorer 위에 그리는 오버레이
- 비공개 DirectUI 객체 트리 자체의 수정
- 자동 `git fetch` 또는 그 밖의 네트워크 접근
- Git 상태 컨트롤의 클릭 명령
- 사용자 색상, 글꼴, 표시 형식 설정
- 원격 저장소의 실시간 상태 보장
- RTL Explorer UI 지원

## 4. 선택한 접근과 대안

### 선택: Explorer 내부 자식 HWND

`WinExInfoHook.dll`을 Explorer UI 스레드에 로드하고, 실제 Explorer 프로세스 안에서 `WinExInfo.StatusPane` 자식 컨트롤을 만든다. 컨트롤은 `DUIViewWndClassName`의 자식이며 기존 DirectUI 상태표시줄과 같은 화면 영역에 배치된다.

이 방식은 Explorer 내부 삽입 요구를 충족하면서 비공개 DirectUI 객체 ABI에 대한 의존을 피한다. 외부 오버레이 창은 만들지 않는다.

### 채택하지 않은 대안

1. DirectUI 객체 트리 직접 수정: 시각적 통합은 가장 깊지만 비공개 ABI와 빌드별 리버스 엔지니어링 범위가 커진다.
2. 기존 항목 수 텍스트 교체: 구현 범위는 작지만 기본 정보를 없애며 새 항목 추가 요구와 다르다.
3. 별도 프로세스 오버레이: Explorer 안정성에는 유리하지만 형님이 선택한 완전한 내부 삽입 요구와 다르다.

## 5. 구성 요소

모든 target은 C++20, MSVC, x64, Unicode로 빌드한다. Release runtime은 static CRT를 사용하고 Control Flow Guard를 켠다. CMake와 Ninja가 configure 및 build를 담당하며 Explorer 안에 CLR이나 제3자 runtime을 올리지 않는다.

### `WinExInfoHost.exe`

- 사용자 세션당 한 인스턴스만 실행한다.
- 단일 인스턴스 이름은 `Local\WinExInfo.Host.v1`이다.
- 정확한 대상 빌드, 프로세스 경로, 서명, 사용자, 세션, 아키텍처를 검사한다.
- 열린 Explorer와 새로 생성되는 Explorer 프로세스를 감지한다.
- 대상 UI 스레드에 x64 hook DLL을 로드한다.
- Git 실행, 상태 파싱, 캐시, 디렉터리 변경 감시를 담당한다.
- DLL과 현재 사용자 전용 named pipe로 통신한다.
- `--install`, `--uninstall`, `--status`, `--background` 명령을 제공한다.
- `--probe`에서 Explorer를 변경하지 않고 HWND, UIA, Shell view 매핑 계약을 진단한다.

### `WinExInfoHook.dll`

- 각 대상 Explorer 프로세스 안에서 실행한다.
- 각 Explorer 창에 하나의 `WinExInfo.StatusPane`을 생성한다.
- 활성 Shell view의 파일 시스템 경로를 얻는다.
- Host에 상태를 요청하고 가장 최신 응답만 렌더링한다.
- Explorer UI 스레드에서는 경로 판정, IPC 송수신 예약, 텍스트 교체만 수행한다.
- Host 연결 종료 시 컨트롤과 이벤트 구독을 정리하고 DLL을 언로드한다.

### `WinExInfoTests.exe`

- Git porcelain v2 파서, 표시 포맷터, IPC 계약, 세대 번호 처리, 캐시 동작을 검증한다.
- 런타임 설치 대상에는 포함하지 않는다.

## 6. 주입 및 Explorer UI 계약

### 대상 프로세스 검증

Host는 다음 조건을 모두 만족하는 프로세스만 대상으로 삼는다.

- `GetWindowsDirectoryW`로 얻은 Windows 디렉터리의 `explorer.exe`와 대상 process image가 같은 volume serial 및 `FILE_ID_INFO`를 가짐
- 대상 `explorer.exe`의 catalog signature가 cache-only Authenticode 검증에 성공함
- signer certificate subject가 2절의 값과 정확히 일치함
- 현재 사용자 SID 및 현재 로그인 세션과 일치함
- 무결성 수준이 Host와 같음
- AMD64 네이티브 프로세스임
- OS build, `explorer.exe`, `ExplorerFrame.dll`, `shell32.dll` 버전이 2절의 값과 정확히 일치함
- `DisableExtensionPoints` 및 `MicrosoftSignedOnly` mitigation이 꺼져 있음

OS major, minor, build는 `RtlGetVersion`에서 읽고 UBR은 `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`의 정확한 `UBR` 값 하나에서 읽는다. 파일 버전은 fixed version resource에서 읽는다. 해당 key나 resource가 없으면 다른 source를 대체로 조회하지 않는다.

경로 문자열은 case, 짧은 경로, junction 차이를 보정하는 근거로 사용하지 않고 열린 파일 handle의 identity를 비교한다. 조건이 하나라도 다르면 주입하지 않고 정확한 오류 코드를 기록한다. 권한 상승, 보안 경계 우회, 다른 사용자 또는 관리자 Explorer 주입은 시도하지 않는다.

### DLL 로드

Host는 시작할 때 기존 Explorer top-level 창을 열거하고, 이후 out-of-context WinEvent create/show 이벤트로 새 창을 감지한다. `GetWindowThreadProcessId`로 대상 `CabinetWClass`의 UI thread ID를 얻고, 그 thread 하나에 `SetWindowsHookExW(WH_CALLWNDPROC, ...)`를 설치한다. hook procedure는 x64 DLL에서 export한다.

Host는 Explorer process마다 attach 시도를 직렬화하고 `1..INT64_MAX` 범위의 `uint64 attach_id`를 증가시킨다. hook 설치 전에 current-user ACL의 manual-reset event `Local\WinExInfo.HookReleased.<PID>.<TID>.<attach_id decimal>`을 만든다.

Host와 DLL은 `RegisterWindowMessageW(L"WinExInfo.Attach.v1")`의 반환 message ID를 사용한다. Host의 injection worker는 `SendMessageTimeoutW(target_hwnd, attach_message, 0x57495831, attach_id, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 1000ms, ...)`로 callback을 한 번 트리거한다. callback은 `CWPSTRUCT.hwnd`, registered message ID, `wParam=0x57495831`, nonzero `lParam=attach_id`가 모두 일치할 때만 동작하고 모든 경우 즉시 `CallNextHookEx`로 전달한다.

callback은 먼저 exact HookReleased event를 연다. event open에 실패하면 module reference를 얻거나 초기화를 시작하지 않는다. 성공하면 첫 process attach에서만 `GetModuleHandleExW`로 명시적 module reference를 얻고 in-flight callback counter를 관리한 뒤 초기화를 예약한다. DLL은 pipe가 준비되면 `attach_result`를 `request_id=attach_id`로 보낸다. SendMessageTimeout의 성공값은 초기화 ACK로 사용하지 않는다. attach_result를 보내기 전에 HookReleased event가 이미 signaled라면 Host timeout으로 간주하고 성공 result나 pane을 만들지 않는다.

Host는 attach_result 성공·실패를 받거나 5초가 지나면 모든 경로에서 `UnhookWindowsHookEx`를 호출한다. 반환값이 `TRUE`일 때만 HookReleased event를 set한다. attach_result를 받은 경우 DLL이 이미 event handle을 열었으므로 Host handle을 닫을 수 있다. timeout 경로에서는 늦게 실행 중인 callback을 위해 event handle을 대상 process 종료 또는 DLL module 소멸까지 유지하고 같은 target에 재시도하지 않는다. DLL은 이 event가 signaled되기 전에는 Running으로 전환하거나 `FreeLibraryAndExitThread`를 호출하지 않는다.

Unhook이 `FALSE`를 반환하면 `GetLastError`와 `HOOK_RELEASE_FAILED`를 기록하고 HookReleased event를 signaled하지 않는다. 해당 DLL module을 강제로 unload하지 않고 target process가 종료될 때까지 event와 module을 유지하며 같은 target에 다시 attach하지 않는다.

첫 process attach가 실패하고 연결된 pane이 0개일 때만 event signal 뒤 unload state machine을 수행한다. 이미 Running인 process의 추가 window attach가 실패하면 해당 window 상태만 폐기하고 기존 pane과 module은 유지한다.

`SetWindowsHookExW` 실패는 `HOOK_INSTALL_FAILED`, SendMessage timeout 또는 실패는 성공한 unhook과 event set 후 `HOOK_TRIGGER_FAILED`, attach_result timeout은 성공한 unhook과 event set 후 `DLL_INITIALIZATION_FAILED`로 처리한다. unhook 자체가 실패하면 앞의 오류보다 `HOOK_RELEASE_FAILED`를 최종 상태로 사용한다.

Unhook 반환 시 실행 중인 callback이 있어도 명시적 module reference가 DLL을 유지한다. 프로세스 공용 상태는 Explorer 프로세스마다 한 번 초기화하고, 창 연결 상태는 top-level HWND와 UI thread 조합마다 한 번 초기화한다.

Explorer가 새로 시작되면 Host가 새 프로세스를 검증한 후 다시 주입한다. Explorer가 종료되면 해당 프로세스의 DLL과 컨트롤도 운영체제에 의해 함께 정리된다.

### DLL 정지 및 unload

DLL state는 `Starting`, `Running`, `Stopping`, `Stopped` 네 값만 가진다. detach 또는 pipe disconnect를 받으면 한 번만 `Stopping`으로 전환한다.

1. 새 상태 요청과 새 창 연결을 받지 않는다.
2. pending pipe I/O와 UIA 작업을 취소한다.
3. 연결된 각 Explorer UI thread에 전용 detach message를 보내 pane destroy, subclass 제거, COM event 해제를 수행하고 ACK를 받는다.
4. 현재 detach ACK를 보낼 pipe writer 하나를 제외한 IPC 및 UIA worker 종료와 in-flight hook callback counter 0을 기다린다.
5. process 공용 handle과 COM apartment를 정리한다.
6. Host가 보낸 detach에는 `detach_result` success를 전송하고 pipe를 닫는다. 이 success는 `ready_to_unload`를 뜻하며 아직 module이 사라졌다는 뜻은 아니다.
7. HookReleased event가 signaled임을 다시 확인한 뒤 전용 unload thread가 보유한 module reference로 `FreeLibraryAndExitThread`를 호출한다.

각 단계는 5초 제한을 가지며 실패 시 DLL을 강제로 해제하지 않고 `DLL_UNLOAD_TIMEOUT`을 Host에 보고한다. Explorer 프로세스가 종료 중이면 운영체제 종료 경로에 맡긴다. DllMain에서는 위 작업을 수행하지 않는다.

### UI 구조 검증

DLL은 대상 창 안에서 다음 구조를 정확히 확인한다.

- class가 `CabinetWClass`인 대상 top-level Explorer 창
- `CabinetWClass` direct-child z-order를 `GetTopWindow`부터
  `GetWindow(..., GW_HWNDNEXT)`로 읽었을 때 처음 나오는 exact
  `ShellTabWindowClass`; 캡처 전후 z-order가 동일하고 첫 후보가 visible이어야 하며,
  다른 visible ShellTab sibling의 존재는 허용한다
- 정확히 하나의 `DUIViewWndClassName`
- FrameworkId=`DirectUI`, ControlType=`StatusBar`, AutomationId=`StatusBarModuleInner`, ClassName=`StatusBarModuleInner`인 UIA 요소 정확히 하나
- StatusBar의 direct child이며 AutomationId=`System.StatusBarViewItemCount`인 Group 정확히 하나
- StatusBar의 direct child이며 AutomationId=`ViewButtonsGroup`인 Group 정확히 하나

현재 빌드에서 세 UIA 요소의 NativeWindowHandle은 모두 0이며, StatusBar는 HWND가 아닌 DirectUI 요소임을 확인했다. lookup은 위 property 조합만 사용한다. 한국어 표시 이름, 창 제목, 주소창 텍스트는 lookup 키로 사용하지 않는다. 필수 요소가 없거나 개수가 정확히 일치하지 않으면 `EXPLORER_UI_CONTRACT_MISMATCH`로 처리하며 다른 class, AutomationId, 레거시 상태표시줄을 대신 사용하지 않는다.

UI Automation 구조 탐색과 속성 조회는 DLL의 전용 MTA worker에서 수행한다. Explorer UI 스레드는 worker의 완료를 기다리지 않으며, worker가 검증한 위치와 상태만 UI 스레드 메시지로 전달한다.

### 배치와 렌더링

- `WinExInfo.StatusPane`은 DirectUI layout child가 아니라 같은 Explorer 프로세스에서 사용되지 않는 상태표시줄 gap 위에 놓는 자식 HWND다.
- parent 후보는 active `ShellTabWindowClass` 아래의 `DUIViewWndClassName`이며, screen 좌표인 UIA BoundingRectangle을 `ScreenToClient`로 parent client 좌표로 변환한다.
- 왼쪽 경계는 `System.StatusBarViewItemCount`의 right + 8 DIP, 오른쪽 경계는 `ViewButtonsGroup`의 left - 8 DIP다.
- 사용 가능한 폭이 96 DIP보다 작으면 컨트롤을 숨긴다.
- child HWND는 `DUIViewWndClassName`의 `DirectUIHWND`보다 위 z-order에 두고 parent client 영역으로 clip한다.
- 기존 Explorer 요소의 경계를 침범하지 않는다.
- 한 줄 끝 말줄임표를 사용하며 hover tooltip에 전체 문자열과 저장소 루트를 표시한다.
- Windows 시스템 글꼴, 현재 테마 색상, high contrast, per-monitor DPI를 따른다.
- 표시 전용이며 키보드 focus나 창 활성화를 가져가지 않는다.
- resize, DPI, theme, 탭 전환, view 재생성 시 위치와 스타일을 다시 계산한다.

위 parent, z-order, clipping 가정은 실제 Explorer 주입 전에 별도 UI 배치 POC로 검증한다. POC는 control PID가 Explorer PID와 같고, parent class와 gap 경계가 위 계약과 일치하며, resize와 탭 전환 후에도 기존 두 Group을 덮지 않는다는 증거를 남겨야 한다. 하나라도 실패하면 외부 overlay나 DirectUI patch로 전환하지 않고 구현을 중단한 뒤 형님과 설계를 다시 결정한다.

## 7. 활성 폴더 판정

주소창이나 창 제목을 해석하지 않는다. 다음 Shell COM 경로는 현재 빌드에서 검증해야 하는 POC 계약이다.

1. `IShellWindows`에서 entry의 `IWebBrowser2::get_HWND`가 현재 Explorer top-level HWND와 일치하는 entry를 모두 열거한다.
2. 각 `IWebBrowser2`에서 `IServiceProvider`를 얻는다.
3. `IServiceProvider::QueryService(SID_STopLevelBrowser, IID_IShellBrowser)`로 `IShellBrowser`를 얻는다.
4. `IShellBrowser::GetWindow`가 위 first-z-order `ShellTabWindowClass` HWND와 exact match인 entry를 고른다.
5. `IShellBrowser::QueryActiveShellView`와 `IShellView::GetWindow`로 view HWND를 얻는다.
6. view HWND가 선택된 `ShellTabWindowClass`의 descendant이며 실제로 보이는 entry를 고른다.
7. browser HWND와 view 조건을 모두 만족하는 entry가 정확히 하나일 때만 그 view를 활성 view로 확정한다.
8. `IFolderView::GetFolder`로 현재 폴더의 `IShellFolder`를 얻는다.
9. `SHGetIDListFromObject`로 absolute PIDL을 얻는다.
10. `SHCreateItemFromIDList`로 `IShellItem`을 만든다.
11. `IShellItem::GetAttributes(SFGAO_FILESYSTEM)` 결과가 `S_OK`이면서 bit가 설정된 경우만 파일 시스템 view로 확정한다. `S_FALSE`이면서 bit가 해제된 경우만 정상적인 비파일시스템 view로 판정한다.
12. 파일 시스템 view에만 `SIGDN_FILESYSPATH` 경로를 요청하며 non-empty 성공값을 요구한다.

현재 view가 내 PC, 빠른 실행, 검색 결과 같은 비파일시스템 위치라서 파일 시스템 경로를 제공하지 않으면 정상적인 숨김 상태로 처리한다. 주소창, URL, 창 제목을 이용한 대체 판정은 두지 않는다.

read-only probe는 각 창에 `shell_terminal_stage`를 정확히 한 번 기록한다. 허용값은 `not_started`, `co_create_shell_windows`, `ishellwindows_get_count`, `ishellwindows_item`, `idispatch_query_iwebbrowser2`, `iwebbrowser2_get_hwnd`, `iwebbrowser2_query_iserviceprovider`, `iserviceprovider_query_top_level_browser`, `ishellbrowser_get_window`, `ishellbrowser_query_active_shell_view`, `ishellview_get_window`, `validate_active_view`, `ishellview_query_ifolderview`, `ifolderview_get_folder`, `sh_get_id_list_from_object`, `sh_create_item_from_id_list`, `ishellitem_get_attributes`, `ishellitem_get_display_name`, `complete`뿐이다. Shell 시작 전 조기 종료는 `not_started`, 성공은 `complete`, cardinality·path-state·non-failed HRESULT/output 불일치는 `validate_active_view`, `FAILED(hresult)` capture 실패는 실패한 boundary를 보존한다.

snapshot과 observe의 Shell coordinator는 `COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE`로 초기화한 전용 STA다. 이 STA가 `IShellWindows`, `IWebBrowser2`, `IServiceProvider`, `IShellBrowser`, `IShellView`와 Shell connection point의 생성, 호출, Advise, Unadvise, 해제를 모두 소유한다. observe STA는 message pump를 유지하며 callback은 immutable event record만 queue하고 remap은 callback 반환 뒤 STA에서 수행한다. UIA는 별도의 단일 windowless MTA가 모든 UIA object와 handler add/remove를 소유하며 snapshot의 기존 MTA UIA worker도 유지한다. apartment 사이는 immutable HWND, cookie, sequence, path availability와 path value만 전달하고 raw COM pointer를 넘기지 않는다.

Shell entry 생성과 제거는 `DShellWindowsEvents::WindowRegistered`와 `WindowRevoked`로 감지한다. 각 `IWebBrowser2`의 폴더 이동은 `DIID_DWebBrowserEvents2` connection point의 `DISPID_NAVIGATECOMPLETE2`로 감지한다.

탭 활성화는 top-level UIA tree에서 FrameworkId=`XAML`, ControlType=`Tab`, AutomationId=`TabView`, ClassName=`Microsoft.UI.Xaml.Controls.TabView`인 요소 정확히 하나를 찾는다. 그 direct child 중 FrameworkId=`XAML`, ControlType=`List`, AutomationId=`TabListView`, ClassName=`ListView`인 요소 정확히 하나를 고른다.

selection handler는 TabListView element에 event ID `UIA_SelectionItem_ElementSelectedEventId`, `TreeScope_Children`으로 등록한다. event sender는 TabListView의 direct child이고 FrameworkId=`XAML`, ControlType=`TabItem`, ClassName=`ListViewItem`이어야 한다. TabItem의 Name이나 빈 AutomationId는 lookup에 사용하지 않는다.

structure handler는 같은 TabListView element에 `TreeScope_Subtree`로 등록한다. sender가 TabListView 또는 그 descendant일 때만 Shell entry와 TabListView direct children을 다시 열거한다. 두 handler는 non-null cache request를 사용하며 AutomationElementMode=`Full`, cache TreeScope=`Element`, cached property는 FrameworkId, ControlType, ClassName, AutomationId, ProcessId, IsOffscreen이다. detach 시 두 handler를 등록에 사용한 동일 element와 handler identity로 제거한다.

탭 selection event에서는 기존 표시를 즉시 숨기고 active view를 다시 판정한다. view가 아직 정확히 하나가 아니면 주기적으로 재시도하지 않고 다음 NavigateComplete2 또는 structure-changed event까지 숨김을 유지한다. event 뒤에도 1~12단계가 0개 또는 여러 view를 반환하면 `ACTIVE_VIEW_CONTRACT_MISMATCH`를 기록한다.

read-only `--probe`는 다중 탭 창에서 위 각 event가 active view와 정확히 연결되고 1~12단계가 정확히 하나의 view를 반환함을 입증해야 한다. 이 매핑이 성립하지 않으면 주소창, 탭 제목, UIA Name을 대체 key로 사용하지 않고 설계를 다시 결정한다.

상태 pane은 top-level Explorer 창마다 하나만 활성화한다. 탭 전환으로 active `DUIViewWndClassName`이 달라지면 이전 pane을 해당 UI thread에서 destroy하고 새 active parent에 다시 create한다. HWND를 다른 UI thread로 reparent하지 않는다.

각 요청은 DLL process 전체에서 단조 증가하는 `request_id`를 가지며 top-level HWND를 함께 저장한다. DLL은 현재 top-level HWND와 `request_id`가 모두 일치하는 응답만 적용하여 이전 폴더에서 늦게 도착한 결과가 새 탭에 표시되지 않게 한다.

## 8. 저장소 판정과 Git 실행

### Git 실행 파일

Host는 Git 실행 파일을 정확히 `C:\Program Files\Git\cmd\git.exe`에서만 연다. 다른 PATH, registry, 설치 위치를 대체로 시도하지 않는다. 파일이 없으면 `GIT_EXECUTABLE_NOT_FOUND`로 처리한다. 시작 시 `git.exe --version`의 UTF-8 출력에서 끝의 CRLF 한 개만 제거한 뒤 `git version 2.49.0.windows.1`과 정확히 비교한다. 다르면 `UNSUPPORTED_GIT_VERSION`으로 처리한다.

Git은 absolute path를 `lpApplicationName`에 지정한 `CreateProcessW`로 직접 실행한다. Windows command line은 하나의 검증된 quoting 함수로 각 argument의 quote와 trailing backslash를 처리한다. `cmd.exe`, PowerShell 또는 임의 shell 문자열 결합은 사용하지 않는다. quoting test는 빈 argument, 공백, quote, trailing backslash, 비 ASCII 경로를 포함한다.

각 Git 실행은 `CREATE_SUSPENDED`, `CREATE_NO_WINDOW`, `CREATE_UNICODE_ENVIRONMENT`로 만들고 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`가 설정된 전용 Job Object에 assign한 뒤 resume한다. 5초 timeout이면 Job 전체를 종료하여 helper를 포함한 process tree를 남기지 않는다.

### 가장 가까운 작업 트리

Host는 현재 폴더부터 부모 방향으로 정확한 이름 `.git`인 디렉터리 또는 파일을 찾는다. 처음 발견한 위치만 후보로 사용한다. 후보가 없으면 Git 저장소 밖으로 판정하고 항목을 숨긴다.

후보가 있으면 다음 Git 명령으로 canonical worktree root와 Git directory를 각각 검증한다.

- `git.exe -C <후보> rev-parse --path-format=absolute --show-toplevel`
- `git.exe -C <후보> rev-parse --path-format=absolute --absolute-git-dir`
- `git.exe -C <후보> rev-parse --path-format=absolute --git-common-dir`

후보가 있는데 검증 명령이 실패하면 저장소 밖으로 흡수하지 않고 `GIT_COMMAND_FAILED`로 처리한다. worktree와 submodule의 `.git` 파일을 지원하며 `.git` 이외의 대체 marker는 사용하지 않는다. 세 경로는 canonical absolute path로 정규화하고 watcher 중복만 제거한다.

### 상태 명령

상태는 다음 한 가지 계약으로만 조회한다.

`git.exe --no-optional-locks -c core.fsmonitor=false -C <저장소 루트> status --porcelain=v2 --branch --ahead-behind --find-renames=50% --ignore-submodules=none -z --untracked-files=all`

- `-z`의 NUL 구분 바이트를 직접 파싱하여 특수 문자가 포함된 파일명도 안전하게 처리한다.
- `fetch`, `pull`, remote API, DNS, 인터넷 연결 확인을 실행하지 않는다.
- repository 설정에 등록된 fsmonitor hook을 실행하지 않는다.
- ahead/behind는 마지막 fetch로 로컬에 저장된 upstream ref와 비교한 값이다.
- 오프라인이어도 같은 로컬 결과를 정상 표시한다.
- ignored 파일은 집계하지 않는다.

파서는 `branch.oid`, `branch.head`, optional `branch.upstream`, optional `branch.ab` header와 record type `1`, `2`, `u`, `?`만 허용한다. branch.oid와 branch.head는 정확히 한 번 있어야 한다. branch.upstream이 없으면 branch.ab도 없어야 하며 화살표를 생략한다. branch.upstream이 있는데 branch.ab가 없으면 `GIT_UPSTREAM_UNAVAILABLE`로 처리한다. 알 수 없는 header, record type, 중복 필수 header, field 수 또는 numeric 형식 불일치는 `GIT_FORMAT_ERROR`로 처리한다.

## 9. Git 상태 집계 및 표시 계약

### 카운터

- `S`: porcelain v2 일반 변경 record의 index 상태가 변경된 경로 수
- `M`: porcelain v2 일반 변경 record의 worktree 상태가 변경된 경로 수
- `?`: untracked record 수
- `C`: unmerged record 수

한 경로가 staged 상태와 그 이후의 unstaged 변경을 함께 가지면 `S`와 `M`에 각각 한 번 집계한다. rename 또는 copy record는 한 경로 변경으로 집계한다. 값이 0인 `S`, `M`, `?`, `C` 토큰은 표시하지 않는다.

표시 문법은 `<head> · <state> [· ↑<ahead> ↓<behind>]`다. `<head>`는 branch name 또는 detached short object ID다. 일반 변경 token 순서는 `C`, `S`, `M`, `?`로 고정한다. initial branch는 `no commits`를 먼저 하나의 segment로 표시하고 변경 token이 있으면 다음 segment에 둔다. initial이 아니고 모든 counter가 0일 때만 state를 `clean`으로 표시한다.

### 표시 문자열

| 상황 | 정확한 표시 형식 |
|---|---|
| 변경 없음 | `main · clean` |
| 변경 있음 | `main · S2 M1 ?3` |
| 충돌 있음 | `main · C2 M1` |
| upstream 차이 있음 | `main · clean · ↑1 ↓0` |
| 아직 커밋 없음 | `main · no commits` |
| 아직 커밋 없고 변경 있음 | `main · no commits · ?3` |
| detached HEAD | `@a1b2c3d · clean` |
| upstream 없음 | 화살표 토큰 생략 |
| upstream 있으며 ahead/behind가 모두 0 | 화살표 토큰 생략 |
| upstream ref를 로컬에서 계산할 수 없음 | `Git upstream unavailable` |
| Git 저장소 밖 | 컨트롤 숨김 |

upstream이 있고 ahead 또는 behind 중 하나라도 0보다 크면 두 값을 모두 `↑<ahead> ↓<behind>` 순서로 표시한다. detached HEAD는 full object ID의 앞 7자를 사용한다. tooltip 문자열은 `<말줄임 없는 전체 표시 문자열>\r\n<canonical 저장소 루트>` 형식이다.

## 10. 갱신, 캐시, 동시성

- 폴더 이동 또는 탭 활성화 시 기존 폴더 결과를 즉시 숨기고 새 상태를 요청한다.
- 같은 저장소의 파일 변경 중에는 현재 결과를 유지하고 500ms 디바운스가 끝난 뒤 교체한다.
- Host는 canonical 저장소 루트별로 Git snapshot과 watcher를 하나씩 유지한다.
- 여러 Explorer 창과 탭이 같은 저장소를 표시하면 snapshot과 watcher를 공유한다.
- worktree root, absolute Git directory, Git common directory를 canonical path로 중복 제거한 뒤 각각 `ReadDirectoryChangesW`로 감시한다.
- watcher는 overlapped I/O, 64KiB buffer, `bWatchSubtree=TRUE`를 사용한다.
- notify filter는 file name, directory name, last write, size, creation 변경이다.
- watcher 이벤트는 500ms 동안 합쳐 한 번의 상태 명령으로 만든다.
- 0-byte completion, `ERROR_NOTIFY_ENUM_DIR`, buffer overflow가 발생하면 cache를 즉시 무효화하고 전체 Git 상태를 다시 조회한 뒤 watcher를 재등록한다.
- worktree 또는 Git directory가 사라지면 저장소 판정을 처음부터 다시 수행한다.
- 유효한 구독자가 사라지면 watcher와 cache를 해제한다.
- 주기적 polling은 사용하지 않는다.
- Git 실행과 파일 감시는 Host background worker에서 수행하며 Explorer UI 스레드를 기다리게 하지 않는다.

## 11. IPC 계약

- pipe 이름: `\\.\pipe\WinExInfo.v1.<현재 사용자 SID>`
- CLI control pipe 이름: `\\.\pipe\WinExInfo.control.v1.<현재 사용자 SID>`
- remote client는 거부한다.
- ACL은 현재 사용자 SID에만 연결 권한을 부여한다.
- Host가 byte-mode pipe server이고 각 injected Explorer 프로세스가 client 연결 하나를 가진다.
- 모든 integer는 unsigned little-endian이다.
- 모든 string은 `uint32 byte_length` 뒤에 NUL 없는 UTF-8 byte를 둔다.
- frame 최대 크기는 256KiB이며 개별 string도 256KiB보다 작아야 한다.
- frame header는 magic 4바이트 ASCII `WXI1`, `uint16 protocol_version=1`, `uint16 message_type`, `uint32 payload_length`, `uint64 request_id` 순서다.
- `message_type=1`은 `status_request`, `2`는 `status_result`, `3`은 `detach_request`, `4`는 `attach_result`, `5`는 `detach_result`다.
- `status_request` payload는 `uint32 explorer_pid`, `uint64 top_level_hwnd`, `string folder_path` 순서다.
- `status_result` payload는 `uint32 state`, `string repository_root`, `string display_text`, `string tooltip_text`, `string error_code` 순서다.
- state `1`은 `repository`, `2`는 `not_repository`, `3`은 `error`다.
- repository state는 root, display, tooltip이 non-empty이고 error code가 empty여야 한다.
- not_repository state는 네 string이 모두 empty여야 한다.
- error state는 root가 empty이고 display, tooltip, error code가 non-empty여야 한다.
- `detach_request`는 Host가 만든 nonzero `detach_id`를 request_id로 사용하고 payload가 없어야 한다.
- `attach_result`는 request_id에 attach_id를 사용하며 payload는 `uint32 explorer_pid`, `uint32 ui_thread_id`, `uint64 top_level_hwnd`, `uint32 result`, `string error_code` 순서다.
- `detach_result`는 request_id에 detach_id를 사용하며 payload는 `uint32 explorer_pid`, `uint32 result`, `string error_code` 순서다.
- attach/detach result의 success는 result 0과 empty error code, failure는 nonzero result와 non-empty error code를 요구한다.
- detach_result success는 DLL의 모든 작업이 정리되어 pipe close와 module unload를 시작할 준비가 됐다는 ACK다. Host는 pipe close 뒤 대상 process module 목록에서 DLL이 사라진 사실까지 확인해야 unload 완료로 판정한다.
- `request_id`는 DLL process 전체에서 유일하고 증가하며, status request를 보낸 top-level HWND와 함께 correlation table에 보관한다.
- 길이, magic, version, message type, field 순서, state별 empty 규칙이 계약과 다르면 연결을 닫고 `IPC_PROTOCOL_ERROR`를 기록한다.
- legacy version, 다른 key, 누락 필드를 보정하는 decoder는 두지 않는다.

control pipe도 같은 frame header와 제한을 사용한다. `message_type=10`은 `control_request`, `11`은 `control_result`다. control request payload는 `uint32 command` 하나이며 `1=status`, `2=shutdown`만 허용한다. control result payload는 `uint32 result`, `string status_text`, `string error_code` 순서이며 success는 result 0과 empty error code, failure는 nonzero result와 non-empty error code를 요구한다.

## 12. 설치, 자동 실행, 제거

### 설치

- runtime 설치 위치: `%LOCALAPPDATA%\WinExInfo`
- 설치 파일: `WinExInfoHost.exe`, `WinExInfoHook.dll`
- 자동 실행 registry 경로: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- 값 이름: `WinExInfo`
- 값 형식: `REG_SZ`
- 값 데이터: `"<설치 시 해석한 LOCALAPPDATA 절대 경로>\WinExInfo\WinExInfoHost.exe" --background`
- 관리자 권한, Windows service, scheduled task는 사용하지 않는다.

`WinExInfoHost.exe --install`은 release 디렉터리에서 실행하며 runtime 파일을 복사하고 `%LOCALAPPDATA%`를 설치 시점에 한 번 해석한 절대 경로로 정확한 Run 값을 등록한 뒤 installed Host를 시작한다. registry 값 안에 `%LOCALAPPDATA%` 문자열을 그대로 저장하지 않는다. 기존 값이나 파일의 내용이 기대 계약과 다르면 덮어써서 흡수하지 않고 `INSTALL_CONTRACT_MISMATCH`를 보고한다.

### 상태 확인

`WinExInfoHost.exe --status`는 다음 정보를 읽기 전용으로 출력한다.

- 대상 OS 및 Explorer 버전 검사 결과
- Host PID와 설치 경로
- 발견한 Explorer PID 및 DLL 주입 상태
- 마지막 오류 코드

### 제거

`WinExInfoHost.exe --uninstall`은 release 디렉터리의 바이너리에서 실행한다.

1. Run 값, 설치 파일 hash, installed Host identity가 설치 계약과 정확히 일치하는지 preflight한다.
2. installed Host에 `shutdown`을 요청한다.
3. installed Host가 모든 injected process에 `detach`를 전송한다.
4. 각 DLL의 unload ACK와 대상 process module 목록에서 DLL이 사라진 사실을 확인한다.
5. installed Host가 pipe, watcher, worker, log를 닫고 종료한 뒤 release uninstaller가 process handle signaled 상태를 확인한다.
6. Run 값, 설치된 Host, DLL, log, 설치 디렉터리를 제거한다.

preflight 불일치가 있거나 5초 안에 DLL 또는 Host가 종료되지 않으면 registry와 설치 파일을 제거하지 않고 `UNINSTALL_CONTRACT_MISMATCH` 또는 `UNINSTALL_ACTIVE_MODULE`로 중단한다. 재부팅 예약 삭제나 임의 shell 삭제를 대체로 사용하지 않는다.

실제 Explorer 최초 주입과 자동 실행 등록은 빌드 및 비주입 테스트를 통과한 후 형님께 다시 승인받고 수행한다.

## 13. 오류 및 진단 계약

| 오류 코드 | 사용자 표시 또는 동작 |
|---|---|
| `UNSUPPORTED_OS_BUILD` | 주입 건너뜀 |
| `UNSUPPORTED_EXPLORER_BUILD` | 주입 건너뜀 |
| `TARGET_VALIDATION_FAILED` | 해당 프로세스 주입 건너뜀 |
| `TARGET_MITIGATION_BLOCKED` | 해당 프로세스 주입 건너뜀 |
| `HOOK_INSTALL_FAILED` | 해당 프로세스 주입 건너뜀 |
| `HOOK_TRIGGER_FAILED` | 해당 창 주입 건너뜀 |
| `HOOK_RELEASE_FAILED` | event와 DLL module을 유지하고 해당 target 재시도 금지 |
| `DLL_INITIALIZATION_FAILED` | 해당 프로세스의 컨트롤 생성 건너뜀 |
| `WINDOW_ATTACH_FAILED` | 해당 창의 컨트롤 생성 건너뜀 |
| `DLL_UNLOAD_TIMEOUT` | DLL을 강제 해제하지 않고 정지 실패 보고 |
| `EXPLORER_UI_CONTRACT_MISMATCH` | 해당 창 컨트롤 생성 건너뜀 |
| `ACTIVE_VIEW_CONTRACT_MISMATCH` | 해당 창의 Git 항목 숨김 |
| `GIT_EXECUTABLE_NOT_FOUND` | `Git unavailable` |
| `UNSUPPORTED_GIT_VERSION` | `Git unavailable` |
| `GIT_COMMAND_FAILED` | `Git error` |
| `GIT_TIMEOUT` | `Git timeout` |
| `GIT_UPSTREAM_UNAVAILABLE` | `Git upstream unavailable` |
| `GIT_FORMAT_ERROR` | `Git format error` |
| `IPC_PROTOCOL_ERROR` | 연결 종료 및 해당 컨트롤 제거 |
| `PIPE_DISCONNECTED` | 해당 컨트롤 제거 및 DLL 정리 |
| `INSTALL_CONTRACT_MISMATCH` | 설치 중단 및 불일치 보고 |
| `UNINSTALL_CONTRACT_MISMATCH` | 제거 중단 및 불일치 보고 |
| `UNINSTALL_ACTIVE_MODULE` | registry와 파일을 유지하고 제거 중단 |

진단 로그 위치는 `%LOCALAPPDATA%\WinExInfo\logs\WinExInfo.log`다. 로그는 1MiB에서 회전하며 현재 파일과 이전 파일 4개만 유지한다. 타임스탬프, 프로세스 ID, 창 handle, 단계, 오류 코드, HRESULT 또는 Win32 오류 번호를 기록한다. 저장소 경로, 파일명, Git stdout 원문, 환경 변수, credential은 기록하지 않는다.

Git 저장소 밖, 파일 시스템이 아닌 Shell view, upstream 없음, 오프라인은 오류가 아니다.

## 14. 보안 및 안정성 원칙

- 같은 사용자와 같은 세션의 Microsoft 서명 Explorer만 대상으로 한다.
- 현재 Explorer의 catalog signature는 catalog hash 조회 후 `WINTRUST_CATALOG_INFO`로 검증한다.
- Authenticode 검증은 `WTD_CACHE_ONLY_URL_RETRIEVAL`과 revocation network 비활성 정책을 사용하여 인증서 확인 때문에 네트워크 연결을 만들지 않는다.
- 관리자 권한을 요청하거나 권한 경계를 넘지 않는다.
- remote thread memory patch, DirectUI vtable patch, 함수 detour를 사용하지 않는다.
- hook callback은 초기화 신호만 처리하고 즉시 다음 hook으로 전달한다.
- Host와 DLL은 Control Flow Guard를 켠 상태로 빌드한다.
- Explorer UI 스레드에서 Git 실행, 파일 I/O, pipe 대기, lock 대기를 수행하지 않는다.
- Host와 DLL 사이의 모든 입력 길이와 enum 값을 검증한다.
- Git 인수는 정확한 Windows command-line quoting 뒤 `CreateProcessW`에 전달하며 shell을 거치지 않는다.
- 네트워크 호출과 원격 상태 갱신을 포함하지 않는다.

## 15. 선행 POC 및 진행 게이트

전체 기능 구현 전에 다음 게이트를 순서대로 통과해야 한다.

### Gate A: 읽기 전용 Explorer 계약 probe

`WinExInfoHost.exe --probe`는 DLL 주입이나 창 변경 없이 다음 증거를 출력한다.

- single-tab 및 multi-tab `CabinetWClass`의 HWND, PID, thread ID
- stable direct-child z-order의 first exact `ShellTabWindowClass`와 그 아래
  `DUIViewWndClassName`, `DirectUIHWND` parent chain
- 6절의 StatusBar 및 Group UIA property와 BoundingRectangle
- 같은 top-level HWND를 공유하는 각 `IShellWindows` entry와 active Shell view HWND 매핑
- Shell entry 등록·해제, `NavigateComplete2`, TabView selection·structure event와 active view 및 파일 시스템 경로 변화

다중 탭에서 active view가 정확히 하나로 결정되고 event가 그 view와 연결될 때만 Gate A가 통과한다. 실패하면 주소창이나 title 기반 판정을 추가하지 않고 설계를 다시 검토한다.

### Gate B: test target hook 및 unload

`WinExInfoTests.exe --hook-target`은 별도 test window와 message loop를 제공한다. 두 번째 `WinExInfoTests.exe --hook-controller <target PID>`가 동일 hook DLL과 주입 library를 사용해 test process에 thread-specific hook을 설치하고 module reference, callback counter, child HWND 생성 및 destroy, worker 종료, `FreeLibraryAndExitThread` 순서를 검증한다. production Host의 Explorer-only 대상 검증을 완화하는 test flag는 만들지 않는다.

다음 조건을 모두 만족해야 한다.

- test target process가 계속 실행되는 동안 hook DLL module이 사라짐
- child HWND, subclass, worker thread, pipe handle이 남지 않음
- timeout이나 loader-lock 경고가 없음
- 반복 100회 load/unload에서 handle 및 thread 수가 증가하지 않음
- test seam에서 Unhook 실패를 반환하면 HookReleased event가 미신호 상태이고 `FreeLibraryAndExitThread`가 호출되지 않으며 module이 target 종료까지 유지됨

### Gate C: 실제 Explorer UI 배치 POC

Gate A와 B 통과 및 2026-07-12 승인을 근거로 현재 Explorer 창 하나에 먼저 실행한다. Git, watcher, 자동 실행, 설치는 포함하지 않고 고정된 `WinExInfo POC` 문자열만 표시한다. exact UI 배치 또는 cleanup 계약이 실패하면 다른 배치 방식으로 우회하지 않고 실패 증거를 남긴다.

다음 조건을 모두 만족해야 한다.

- control HWND의 PID가 대상 Explorer PID와 일치함
- parent가 active `DUIViewWndClassName`이고 z-order가 `DirectUIHWND` 위임
- 두 UIA Group 사이의 8 DIP padding과 96 DIP minimum 규칙을 지킴
- resize와 multi-tab 전환 후 기존 상태표시줄 요소를 덮지 않음
- detach 후 control과 DLL이 사라지고 Explorer는 계속 정상 동작함

Gate A, B, C 중 하나라도 실패하면 full implementation으로 진행하지 않는다. 외부 overlay, DirectUI patch, 주소창 parsing, 강제 module unload를 대체로 시도하지 않고 형님과 새 설계를 승인한다.

## 16. 검증 계획

### 단위 테스트

- porcelain v2 clean, staged, unstaged, untracked, rename, conflict record
- 한 경로의 staged 및 unstaged 중복 집계
- initial branch, detached HEAD, upstream 없음
- ahead/behind가 각각 0 또는 양수인 조합
- NUL 구분 및 특수 문자가 포함된 파일명
- 표시 token 순서와 0 값 생략
- IPC magic, version, 길이, message type, 필수 field 거부
- Windows command-line quoting의 empty, space, quote, trailing backslash, Unicode 사례
- 오래된 `request_id` 응답 폐기
- 500ms debounce와 repository cache 공유
- watcher overflow 시 cache 무효화, 전체 재조회, 재등록

### Git 통합 테스트

임시 로컬 저장소만 사용한다.

- clean 저장소
- staged, unstaged, untracked, conflict 상태
- 커밋이 없는 저장소
- detached HEAD
- 저장소 하위 폴더
- 중첩 저장소에서 가장 가까운 `.git` 선택
- worktree 및 `.git` 파일
- linked worktree의 Git directory 및 common directory 변경
- 로컬 bare remote를 이용한 ahead/behind
- 5초 timeout과 process tree 종료
- 어떤 테스트도 인터넷 연결을 요구하지 않음

### Host 및 DLL 비주입 통합 테스트

- test window에 `WinExInfo.StatusPane` 생성 및 제거
- light, dark, high contrast 및 DPI metric 반영
- named pipe ACL과 remote client 거부
- Host 정상 종료 및 비정상 연결 종료 시 DLL cleanup
- hook callback in-flight 상태의 detach 및 module reference 해제
- unsupported build와 UI contract mismatch의 안전한 건너뛰기
- extension point 또는 binary signature mitigation이 켜진 대상의 안전한 건너뛰기

### 실제 Explorer E2E

2026-07-12 승인에 따라 현재 PC에서 수행한다.

- control HWND의 소유 PID가 대상 `explorer.exe` PID와 일치함
- 기존 항목 수와 보기 모드 컨트롤이 유지됨
- 저장소와 일반 폴더 사이 이동 시 표시 및 숨김이 정확함
- 파일 변경 후 1.5초 이내 새 상태가 표시됨
- 여러 탭과 여러 창에서 각 활성 저장소 상태가 정확함
- resize와 탭 전환 중 기존 상태표시줄 요소와 겹치지 않음
- Explorer 재시작 후 Host가 새 프로세스에 다시 주입함
- Host 종료 후 컨트롤 제거와 DLL 언로드가 확인됨
- Host와 그 Job Object 안의 Git 및 helper process 전체에서 outbound network connection이 생성되지 않음

### 설치 및 제거 검증

- runtime 파일이 정확한 설치 위치에만 복사됨
- Run 값 이름과 데이터가 정확함
- 로그인 실행 인수가 정확히 `--background`임
- `--status`가 현재 프로세스와 오류 상태를 읽기 전용으로 보고함
- `--uninstall` 후 Run 값, 프로세스, DLL, log, 설치 디렉터리가 남지 않음

## 17. 완료 기준

다음 조건을 모두 충족하면 첫 버전 구현이 완료된 것으로 본다.

1. CMake configure, x64 Release build, CTest가 오류 없이 끝난다.
2. 15절의 세 POC gate와 16절의 단위 및 비주입 통합 테스트가 모두 통과한다.
3. 현재 PC의 실제 Explorer E2E가 모두 통과한다.
4. Git 상태 변경은 작은 로컬 검증 저장소에서 파일 이벤트 발생 후 1.5초 안에 반영된다.
5. Explorer UI 스레드는 Git 또는 IPC 완료를 기다리지 않는다.
6. 컨트롤은 Explorer 프로세스 소유 HWND이며 외부 overlay 창이 아니다.
7. 오프라인 상태에서 네트워크 시도 없이 마지막 로컬 upstream ref 기준 값을 표시한다.
8. 제거 검증 후 WinExInfo가 만든 자동 실행 값, 프로세스, DLL, 설치 파일, 로그가 남지 않는다.

## 18. 참고 자료

- [IShellWindows](https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nn-exdisp-ishellwindows)
- [DShellWindowsEvents](https://learn.microsoft.com/en-us/windows/win32/shell/dshellwindowsevents)
- [DWebBrowserEvents2](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa768283%28v%3Dvs.85%29)
- [NavigateComplete2](https://learn.microsoft.com/en-us/previous-versions/aa768334%28v%3Dvs.85%29)
- [IShellBrowser](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ishellbrowser)
- [IShellBrowser::QueryActiveShellView](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellbrowser-queryactiveshellview)
- [IFolderView::GetFolder](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview-getfolder)
- [SHGetIDListFromObject](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-shgetidlistfromobject)
- [SHCreateItemFromIDList](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-shcreateitemfromidlist)
- [WINTRUST_DATA](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_data)
- [CryptCATAdminCalcHashFromFileHandle2](https://learn.microsoft.com/en-us/windows/win32/api/mscat/nf-mscat-cryptcatadmincalchashfromfilehandle2)
- [SetWindowsHookExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw)
- [UnhookWindowsHookEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwindowshookex)
- [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook)
- [IUIAutomation::AddAutomationEventHandler](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-addautomationeventhandler)
- [IUIAutomation::AddStructureChangedEventHandler](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-addstructurechangedeventhandler)
- [UI Automation event identifiers](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-event-ids)
- [PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-process_mitigation_extension_point_disable_policy)
- [ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
- [Git status porcelain v2](https://git-scm.com/docs/git-status)
- [Working with Shell Extensions](https://learn.microsoft.com/en-us/windows/win32/shell/shell-exts)
- [IShellBrowser::SendControlMsg](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellbrowser-sendcontrolmsg)
