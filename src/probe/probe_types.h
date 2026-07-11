#pragma once

#include "common/status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace winexinfo {

enum class ProbeMode {
    Snapshot,
    Observe,
};

enum class ObservedEventKind {
    WindowRegistered,
    WindowRevoked,
    NavigateComplete2,
    TabSelected,
    TabStructureChanged,
};

enum class ObservedEventTransition {
    Pending,
    Remapped,
    Revoked,
    Mismatch,
    Reconciled,
};

enum class ObservedStructureChangeType {
    None,
    ChildAdded,
    ChildRemoved,
    ChildrenInvalidated,
    ChildrenBulkAdded,
    ChildrenBulkRemoved,
    ChildrenReordered,
};

enum class ShellProbeTerminalStage {
    NotStarted,
    CoCreateShellWindows,
    IShellWindowsGetCount,
    IShellWindowsItem,
    IDispatchQueryIWebBrowser2,
    IWebBrowser2GetHwnd,
    IWebBrowser2QueryIServiceProvider,
    IServiceProviderQueryTopLevelBrowser,
    IShellBrowserGetWindow,
    IShellBrowserQueryActiveShellView,
    IShellViewGetWindow,
    ValidateActiveView,
    IShellViewQueryIFolderView,
    IFolderViewGetFolder,
    ShGetIdListFromObject,
    ShCreateItemFromIdList,
    IShellItemGetAttributes,
    IShellItemGetDisplayName,
    Complete,
};

struct ReportField final {
    std::string key;
    std::string value;
};

struct ReportSection final {
    std::vector<ReportField> fields;
};

struct ProbeReport final {
    ProbeMode mode;
    bool passed;
    std::vector<ReportSection> sections;
    ErrorCode error_code;
};

struct ActiveShellViewSnapshot final {
    Status status;
    ShellProbeTerminalStage terminal_stage;
    std::size_t top_level_entry_count;
    std::size_t shell_tab_match_count;
    std::size_t active_view_count;
    HWND active_view;
    bool filesystem_path_available;
    std::wstring filesystem_path;
};

struct ObservedEventRecord final {
    std::uint64_t sequence;
    std::uint64_t generation;
    ObservedEventKind kind;
    ObservedEventTransition transition;
    HWND source_top_level;
    bool source_shell_tab_present;
    HWND source_shell_tab;
    bool source_was_active;
    bool shell_cookie_present;
    LONG shell_cookie;
    ObservedStructureChangeType structure_change_type;
    HWND previous_active_view;
    HWND current_active_view;
    std::size_t active_view_count;
    bool previous_filesystem_path_available;
    std::wstring previous_filesystem_path;
    bool current_filesystem_path_available;
    std::wstring current_filesystem_path;
    Status status;
    std::size_t previous_tab_count;
    std::size_t current_tab_count;
    std::size_t added_tab_count;
    std::size_t removed_tab_count;
    std::size_t retained_tab_count;
    HWND reconciled_active_shell_tab;
};

struct ObservedEventKindCounts final {
    std::size_t window_registered;
    std::size_t window_revoked;
    std::size_t navigate_complete2;
    std::size_t tab_selected;
    std::size_t tab_structure_changed;
};

struct EventObservationSnapshot final {
    std::uint32_t duration_ms;
    std::size_t event_count;
    std::size_t ignored_event_count;
    std::size_t late_event_count;
    ObservedEventKindCounts kind_counts;
    Status runtime_status;
    Status cleanup_status;
    std::vector<ObservedEventRecord> events;
    std::string runtime_stage;
};

}  // namespace winexinfo
