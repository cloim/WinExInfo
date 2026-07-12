#pragma once

#include "common/status.h"
#include "common/win32_handle.h"
#include "host/command_line.h"
#include "host/explorer_session.h"
#include "probe/tab_identity.h"
#include "probe/win32_probe.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace winexinfo {

inline constexpr std::wstring_view kBackgroundMutexName =
    L"Local\\WinExInfo.Host.v1";

[[nodiscard]] Status BuildBackgroundStopEventName(
    DWORD processId, std::wstring* output);
[[nodiscard]] Status CreateBackgroundStopEvent(
    DWORD processId, UniqueHandle* output);

struct ExplorerProcessSnapshot final {
    DWORD process_id = 0;
    bool validated = false;
    std::vector<SessionWindowSnapshot> windows;

    bool operator==(const ExplorerProcessSnapshot&) const = default;
};

struct BackgroundSnapshot final {
    std::uint64_t sequence = 0;
    std::vector<ExplorerProcessSnapshot> processes;

    bool operator==(const BackgroundSnapshot&) const = default;
};

class BackgroundSession {
public:
    virtual ~BackgroundSession() = default;
    [[nodiscard]] virtual Status Reconcile(
        std::span<const SessionWindowSnapshot> windows) = 0;
    [[nodiscard]] virtual Status Stop() = 0;
};

struct BackgroundOperations final {
    std::function<Status()> acquire_single_instance;
    std::function<void()> release_single_instance;
    std::function<Status(BackgroundSnapshot*, bool*)> next_snapshot;
    std::function<Status(DWORD, std::unique_ptr<BackgroundSession>*)>
        create_session;
    std::function<void(std::unique_ptr<BackgroundSession>)> retain_session;
};

class BackgroundObserverTracker final {
public:
    [[nodiscard]] Status Reconcile(
        std::uint64_t sequence,
        std::span<const ExplorerWindowRecord> windows,
        std::span<const ObserverShellEntryMetadata> shellEntries,
        std::span<const ObserverTopLevelTabOrder> orders,
        std::span<const DWORD> validatedPids,
        BackgroundSnapshot* output);

private:
    std::vector<ObserverTabIdentity> tabs_;
    ObserverTabGenerationState generations_;
};

[[nodiscard]] Status ReconcileEmptyProductionBackgroundObservation(
    std::uint64_t sequence,
    std::span<const ExplorerWindowRecord> enumeratedWindows,
    BackgroundObserverTracker* tracker,
    BackgroundSnapshot* output);

[[nodiscard]] HostExitCode RunBackgroundCoordinator(
    const BackgroundOperations& operations);

[[nodiscard]] HostExitCode RunProductionBackgroundCoordinator(
    const std::wstring& hookDllPath);

[[nodiscard]] Status CaptureProductionBackgroundSnapshotOnce(
    std::uint64_t sequence,
    BackgroundSnapshot* output);

}  // namespace winexinfo
