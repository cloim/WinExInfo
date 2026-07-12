#include "host/background_coordinator.h"

#include "common/win32_handle.h"
#include "probe/shell_probe.h"
#include "probe/target_validator.h"
#include "probe/uia_probe.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ExDisp.h>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <objbase.h>
#include <set>
#include <thread>
#include <utility>
#include <wrl/client.h>

namespace winexinfo {
namespace {

bool ExactSuccess(const Status& status) noexcept {
    return status.code == ErrorCode::OK && status.hresult == S_OK &&
        status.win32 == ERROR_SUCCESS;
}

bool ValidSnapshot(const BackgroundSnapshot& snapshot) {
    if (snapshot.sequence == 0) return false;
    std::vector<DWORD> pids;
    pids.reserve(snapshot.processes.size());
    for (const ExplorerProcessSnapshot& process : snapshot.processes) {
        if (process.process_id == 0 ||
            std::find(pids.begin(), pids.end(), process.process_id) != pids.end()) {
            return false;
        }
        pids.push_back(process.process_id);
    }
    return true;
}

class MutexRelease final {
public:
    explicit MutexRelease(const BackgroundOperations* operations)
        : operations_(operations) {}
    ~MutexRelease() {
        if (operations_ != nullptr && operations_->release_single_instance) {
            operations_->release_single_instance();
        }
    }
private:
    const BackgroundOperations* operations_;
};

struct SessionState final {
    std::unique_ptr<BackgroundSession> session;
    std::size_t creation_order = 0;
};

std::atomic<HANDLE> g_background_stop_event{nullptr};

BOOL WINAPI BackgroundConsoleHandler(const DWORD controlType) {
    if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT &&
        controlType != CTRL_CLOSE_EVENT && controlType != CTRL_LOGOFF_EVENT &&
        controlType != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }
    const HANDLE event = g_background_stop_event.load(std::memory_order_acquire);
    return event != nullptr && SetEvent(event) != FALSE;
}

class ProductionUiaMtaWorker final {
public:
    ~ProductionUiaMtaWorker() { Stop(); }

    Status Start() {
        std::unique_lock lock{mutex_};
        if (thread_.joinable()) return InvalidStatus();
        thread_ = std::thread([this]() { Run(); });
        ready_cv_.wait(lock, [this]() { return ready_; });
        return startup_;
    }

    Status Validate(
        std::vector<std::pair<HWND, HWND>> targets,
        std::vector<bool>* const valid) {
        if (valid == nullptr) return InvalidStatus();
        std::unique_lock lock{mutex_};
        if (!ExactSuccess(startup_) || stopping_ || pending_) {
            return InvalidStatus();
        }
        targets_ = std::move(targets);
        pending_ = true;
        completed_ = false;
        request_cv_.notify_one();
        response_cv_.wait(lock, [this]() { return completed_; });
        *valid = std::move(valid_);
        return response_;
    }

private:
    static Status InvalidStatus() noexcept {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG,
                ERROR_INVALID_PARAMETER};
    }

    void Stop() noexcept {
        {
            std::scoped_lock lock{mutex_};
            stopping_ = true;
            request_cv_.notify_one();
        }
        if (thread_.joinable()) thread_.join();
    }

    void Run() noexcept {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Microsoft::WRL::ComPtr<IUIAutomation> automation;
        Status startup = initialized == S_OK || initialized == S_FALSE
            ? Status{ErrorCode::OK, S_OK, ERROR_SUCCESS}
            : Status{ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                     initialized, ERROR_SUCCESS};
        if (startup.ok()) {
            const HRESULT created = CoCreateInstance(
                CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&automation));
            if (FAILED(created)) {
                startup = {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                           created, ERROR_SUCCESS};
            }
        }
        {
            std::scoped_lock lock{mutex_};
            startup_ = startup;
            ready_ = true;
            ready_cv_.notify_one();
        }
        if (!startup.ok()) {
            if (initialized == S_OK || initialized == S_FALSE) CoUninitialize();
            return;
        }

        for (;;) {
            std::vector<std::pair<HWND, HWND>> targets;
            {
                std::unique_lock lock{mutex_};
                request_cv_.wait(lock, [this]() { return pending_ || stopping_; });
                if (stopping_) break;
                targets = std::move(targets_);
                pending_ = false;
            }
            std::vector<bool> valid;
            Status response{ErrorCode::OK, S_OK, ERROR_SUCCESS};
            try {
                valid.reserve(targets.size());
                for (const auto& [topLevel, activeView] : targets) {
                    RetainedUiaContractCapture capture{};
                    const Status captured = CaptureRetainedUiaContract(
                        automation.Get(), topLevel, activeView, &capture);
                    valid.push_back(captured.ok());
                }
            } catch (const std::bad_alloc&) {
                response = {ErrorCode::PIPE_DISCONNECTED,
                            E_OUTOFMEMORY, ERROR_SUCCESS};
            }
            {
                std::scoped_lock lock{mutex_};
                valid_ = std::move(valid);
                response_ = response;
                completed_ = true;
                response_cv_.notify_one();
            }
        }
        automation.Reset();
        CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::condition_variable request_cv_;
    std::condition_variable response_cv_;
    std::thread thread_;
    Status startup_{ErrorCode::INVALID_ARGUMENT, E_PENDING, ERROR_IO_PENDING};
    Status response_{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    std::vector<std::pair<HWND, HWND>> targets_;
    std::vector<bool> valid_;
    bool ready_ = false;
    bool pending_ = false;
    bool completed_ = false;
    bool stopping_ = false;
};

class ProductionBackgroundObserver final {
public:
    Status Start() {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        const HRESULT apartment = CoGetApartmentType(&type, &qualifier);
        if (FAILED(apartment) ||
            (type != APTTYPE_STA && type != APTTYPE_MAINSTA)) {
            return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    FAILED(apartment) ? apartment : E_UNEXPECTED,
                    ERROR_INVALID_STATE};
        }
        const HRESULT created = CoCreateInstance(
            CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&shell_windows_));
        if (FAILED(created)) {
            return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    created, ERROR_SUCCESS};
        }
        return mta_.Start();
    }

    Status Capture(const std::uint64_t sequence, BackgroundSnapshot* output) {
        if (output == nullptr || sequence == 0 || !shell_windows_) {
            return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG,
                    ERROR_INVALID_PARAMETER};
        }
        std::vector<ExplorerWindowRecord> enumerated;
        Status status = EnumerateExplorerWindows(&enumerated);
        if (!status.ok()) return status;

        std::vector<ExplorerWindowRecord> windows;
        std::vector<ObserverTopLevelTabOrder> orders;
        std::vector<std::pair<HWND, HWND>> uiaTargets;
        std::set<DWORD> invalidPids;
        try {
            for (const ExplorerWindowRecord& window : enumerated) {
                Win32ClassTree tree{};
                status = CaptureWin32ClassTree(window.hwnd, &tree);
                if (!status.ok()) {
                    invalidPids.insert(window.process_id);
                    continue;
                }
                const Win32ContractResult contract = ValidateWin32Contract(tree);
                if (!contract.status.ok()) {
                    invalidPids.insert(window.process_id);
                    continue;
                }
                ObserverTopLevelTabOrder order{window.hwnd, {}};
                order.tabs.reserve(contract.ordered_shell_tabs.size());
                for (const HWND tab : contract.ordered_shell_tabs) {
                    const auto node = std::find_if(
                        tree.nodes.begin(), tree.nodes.end(),
                        [tab](const Win32ClassNode& item) {
                            return item.hwnd == tab;
                        });
                    if (node == tree.nodes.end() || node->parent != window.hwnd) {
                        invalidPids.insert(window.process_id);
                        order.tabs.clear();
                        break;
                    }
                    order.tabs.push_back({tab, node->visible});
                }
                if (order.tabs.empty()) continue;
                windows.push_back(window);
                orders.push_back(std::move(order));
                uiaTargets.push_back({window.hwnd, contract.active_view});
            }

            std::vector<HWND> topLevels;
            topLevels.reserve(windows.size());
            for (const ExplorerWindowRecord& window : windows) {
                topLevels.push_back(window.hwnd);
            }
            if (topLevels.empty()) {
                return ReconcileEmptyProductionBackgroundObservation(
                    sequence, enumerated, &tracker_, output);
            }
            ShellBrowserSetCapture shellCapture{};
            status = CaptureShellBrowserSet(
                shell_windows_.Get(), topLevels, &shellCapture);
            if (!status.ok()) return status;
            std::vector<ObserverShellEntryMetadata> metadata;
            metadata.reserve(shellCapture.entries.size());
            for (const ShellBrowserEntryCapture& entry : shellCapture.entries) {
                metadata.push_back({
                    reinterpret_cast<std::uintptr_t>(entry.canonical_identity.Get()),
                    entry.target_matched,
                    entry.top_level,
                    entry.shell_tab,
                });
            }

            std::vector<bool> uiaValid;
            status = mta_.Validate(std::move(uiaTargets), &uiaValid);
            if (!status.ok() || uiaValid.size() != windows.size()) return status;
            std::set<DWORD> validPids;
            for (std::size_t index = 0; index < windows.size(); ++index) {
                if (!uiaValid[index]) invalidPids.insert(windows[index].process_id);
            }
            for (const ExplorerWindowRecord& window : windows) {
                if (invalidPids.contains(window.process_id) ||
                    validPids.contains(window.process_id)) {
                    continue;
                }
                TargetValidationEvidence evidence{};
                const Status captured = CaptureTargetValidationEvidence(
                    window.process_id, &evidence);
                const TargetValidationResult validation =
                    captured.ok() ? ValidateTargetEvidence(evidence)
                                  : TargetValidationResult{captured, {}};
                if (captured.ok() && validation.status.ok()) {
                    validPids.insert(window.process_id);
                } else {
                    invalidPids.insert(window.process_id);
                }
            }
            std::vector<DWORD> validated(validPids.begin(), validPids.end());
            status = tracker_.Reconcile(
                sequence, windows, metadata, orders, validated, output);
            if (!status.ok()) return status;
            for (const ExplorerWindowRecord& window : enumerated) {
                if (!invalidPids.contains(window.process_id)) continue;
                const auto found = std::find_if(
                    output->processes.begin(), output->processes.end(),
                    [&](const ExplorerProcessSnapshot& process) {
                        return process.process_id == window.process_id;
                    });
                if (found == output->processes.end()) {
                    output->processes.push_back({window.process_id, false, {}});
                } else {
                    found->validated = false;
                }
            }
            return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
        } catch (const std::bad_alloc&) {
            return {ErrorCode::PIPE_DISCONNECTED, E_OUTOFMEMORY, ERROR_SUCCESS};
        }
    }

private:
    Microsoft::WRL::ComPtr<IShellWindows> shell_windows_;
    ProductionUiaMtaWorker mta_;
    BackgroundObserverTracker tracker_;
};

class ProductionBackgroundSession final : public BackgroundSession {
public:
    ProductionBackgroundSession(const DWORD pid, ExplorerSessionOperations operations)
        : session_(pid, std::move(operations)) {}
    Status Reconcile(
        const std::span<const SessionWindowSnapshot> windows) override {
        return session_.Reconcile(windows);
    }
    Status Stop() override { return session_.Stop(); }
private:
    ExplorerSession session_;
};

struct ProductionBackgroundState final {
    explicit ProductionBackgroundState(std::wstring path)
        : hook_dll_path(std::move(path)) {}
    std::wstring hook_dll_path;
    UniqueHandle mutex;
    UniqueHandle stop_event;
    ProductionBackgroundObserver observer;
    std::uint64_t sequence = 0;
    bool console_handler = false;
};

std::vector<std::unique_ptr<BackgroundSession>>& RetainedProductionSessions() {
    static auto* sessions =
        new std::vector<std::unique_ptr<BackgroundSession>>();
    return *sessions;
}

}  // namespace

Status BackgroundObserverTracker::Reconcile(
    const std::uint64_t sequence,
    const std::span<const ExplorerWindowRecord> windows,
    const std::span<const ObserverShellEntryMetadata> shellEntries,
    const std::span<const ObserverTopLevelTabOrder> orders,
    const std::span<const DWORD> validatedPids,
    BackgroundSnapshot* const output) {
    if (sequence == 0 || output == nullptr) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    ObserverTabSetReconciliation reconciliation{};
    const Status status = ReconcileObserverTabSet(
        tabs_, shellEntries, orders, generations_, &reconciliation);
    if (!status.ok()) return status;

    try {
        BackgroundSnapshot candidate{sequence, {}};
        for (const ExplorerWindowRecord& window : windows) {
            auto process = std::find_if(
                candidate.processes.begin(), candidate.processes.end(),
                [&](const ExplorerProcessSnapshot& item) {
                    return item.process_id == window.process_id;
                });
            if (process == candidate.processes.end()) {
                candidate.processes.push_back({
                    window.process_id,
                    std::find(validatedPids.begin(), validatedPids.end(),
                              window.process_id) != validatedPids.end(),
                    {},
                });
                process = std::prev(candidate.processes.end());
            }
            const auto order = std::find_if(
                orders.begin(), orders.end(), [&](const auto& item) {
                    return item.top_level == window.hwnd;
                });
            if (order == orders.end()) {
                return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                        S_FALSE, ERROR_INVALID_DATA};
            }
            SessionWindowSnapshot sessionWindow{window.hwnd, 0, {}};
            for (const ObserverOrderedTab& orderedTab : order->tabs) {
                const auto identity = std::find_if(
                    reconciliation.current.begin(), reconciliation.current.end(),
                    [&](const ObserverTabIdentity& item) {
                        return item.top_level == window.hwnd &&
                            item.shell_tab == orderedTab.shell_tab;
                    });
                if (identity == reconciliation.current.end()) {
                    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                            S_FALSE, ERROR_INVALID_DATA};
                }
                if (sessionWindow.top_level_generation == 0) {
                    sessionWindow.top_level_generation =
                        identity->top_level_generation;
                } else if (sessionWindow.top_level_generation !=
                           identity->top_level_generation) {
                    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                            S_FALSE, ERROR_INVALID_DATA};
                }
                sessionWindow.tabs.push_back({
                    reinterpret_cast<std::uint64_t>(identity->shell_tab),
                    identity->tab_generation,
                    window.thread_id,
                });
            }
            if (sessionWindow.top_level_generation == 0 ||
                sessionWindow.tabs.empty()) {
                return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                        S_FALSE, ERROR_INVALID_DATA};
            }
            process->windows.push_back(std::move(sessionWindow));
        }
        tabs_ = reconciliation.current;
        generations_ = reconciliation.generations;
        *output = std::move(candidate);
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    } catch (const std::bad_alloc&) {
        return {ErrorCode::PIPE_DISCONNECTED, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

Status ReconcileEmptyProductionBackgroundObservation(
    const std::uint64_t sequence,
    const std::span<const ExplorerWindowRecord> enumeratedWindows,
    BackgroundObserverTracker* const tracker,
    BackgroundSnapshot* const output) {
    if (tracker == nullptr || output == nullptr) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG,
                ERROR_INVALID_PARAMETER};
    }
    const std::vector<ExplorerWindowRecord> windows;
    const std::vector<ObserverShellEntryMetadata> metadata;
    const std::vector<ObserverTopLevelTabOrder> orders;
    const std::vector<DWORD> validated;
    Status status = tracker->Reconcile(
        sequence, windows, metadata, orders, validated, output);
    if (!status.ok()) return status;
    try {
        for (const ExplorerWindowRecord& window : enumeratedWindows) {
            if (window.process_id == 0) {
                return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                        S_FALSE, ERROR_INVALID_DATA};
            }
            if (std::find_if(
                    output->processes.begin(), output->processes.end(),
                    [&](const ExplorerProcessSnapshot& process) {
                        return process.process_id == window.process_id;
                    }) == output->processes.end()) {
                output->processes.push_back({window.process_id, false, {}});
            }
        }
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    } catch (const std::bad_alloc&) {
        return {ErrorCode::PIPE_DISCONNECTED, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

HostExitCode RunBackgroundCoordinator(const BackgroundOperations& operations) {
    if (!operations.acquire_single_instance ||
        !operations.release_single_instance || !operations.next_snapshot ||
        !operations.create_session || !operations.retain_session) {
        return HostExitCode::InvalidCli;
    }
    if (!ExactSuccess(operations.acquire_single_instance())) {
        return HostExitCode::Win32ComFailure;
    }
    MutexRelease release{&operations};

    std::map<DWORD, SessionState> sessions;
    std::size_t nextCreationOrder = 0;
    std::uint64_t lastSequence = 0;

    const auto retain = [&](SessionState& state) {
        if (state.session) operations.retain_session(std::move(state.session));
    };
    const auto stopAll = [&](const DWORD exceptPid) {
        HostExitCode result = HostExitCode::Pass;
        std::vector<std::pair<DWORD, SessionState*>> ordered;
        ordered.reserve(sessions.size());
        for (auto& [pid, state] : sessions) ordered.push_back({pid, &state});
        std::ranges::sort(ordered, [](const auto& left, const auto& right) {
            return left.second->creation_order > right.second->creation_order;
        });
        for (auto& [pid, state] : ordered) {
            if (!state->session) continue;
            if (pid == exceptPid) {
                retain(*state);
                result = HostExitCode::Win32ComFailure;
                continue;
            }
            const Status stopped = state->session->Stop();
            if (!ExactSuccess(stopped)) {
                retain(*state);
                result = HostExitCode::Win32ComFailure;
            } else {
                state->session.reset();
            }
        }
        return result;
    };

    try {
        for (;;) {
            BackgroundSnapshot incoming{};
            bool stop = false;
            const Status received = operations.next_snapshot(&incoming, &stop);
            if (!ExactSuccess(received)) {
                static_cast<void>(stopAll(0));
                return HostExitCode::Win32ComFailure;
            }
            if (stop) {
                return stopAll(0);
            }
            const BackgroundSnapshot snapshot = incoming;
            if (!ValidSnapshot(snapshot) || snapshot.sequence <= lastSequence) {
                static_cast<void>(stopAll(0));
                return HostExitCode::ContractFailure;
            }
            lastSequence = snapshot.sequence;

            std::vector<DWORD> desired;
            desired.reserve(snapshot.processes.size());
            for (const ExplorerProcessSnapshot& process : snapshot.processes) {
                if (!process.validated) continue;
                desired.push_back(process.process_id);
                auto found = sessions.find(process.process_id);
                if (found == sessions.end()) {
                    if (process.windows.empty()) continue;
                    std::unique_ptr<BackgroundSession> created;
                    const Status creation =
                        operations.create_session(process.process_id, &created);
                    if (!ExactSuccess(creation) || !created) {
                        static_cast<void>(stopAll(0));
                        return HostExitCode::Win32ComFailure;
                    }
                    found = sessions.emplace(
                        process.process_id,
                        SessionState{std::move(created), nextCreationOrder++})
                        .first;
                }
                const Status reconciled =
                    found->second.session->Reconcile(process.windows);
                if (!ExactSuccess(reconciled)) {
                    static_cast<void>(stopAll(process.process_id));
                    return HostExitCode::Win32ComFailure;
                }
            }

            std::vector<DWORD> absent;
            for (const auto& [pid, state] : sessions) {
                if (state.session &&
                    std::find(desired.begin(), desired.end(), pid) == desired.end()) {
                    absent.push_back(pid);
                }
            }
            std::ranges::sort(absent, [&](const DWORD left, const DWORD right) {
                return sessions.at(left).creation_order >
                    sessions.at(right).creation_order;
            });
            for (const DWORD pid : absent) {
                SessionState& state = sessions.at(pid);
                const Status stopped = state.session->Stop();
                if (!ExactSuccess(stopped)) {
                    retain(state);
                    static_cast<void>(stopAll(0));
                    return HostExitCode::Win32ComFailure;
                }
                sessions.erase(pid);
            }
        }
    } catch (const std::bad_alloc&) {
        static_cast<void>(stopAll(0));
        return HostExitCode::Win32ComFailure;
    }
}

Status CaptureProductionBackgroundSnapshotOnce(
    const std::uint64_t sequence,
    BackgroundSnapshot* const output) {
    ProductionBackgroundObserver observer;
    const Status started = observer.Start();
    return started.ok() ? observer.Capture(sequence, output) : started;
}

HostExitCode RunProductionBackgroundCoordinator(const std::wstring& hookDllPath) {
    if (hookDllPath.empty()) return HostExitCode::InvalidCli;
    auto state = std::make_shared<ProductionBackgroundState>(hookDllPath);
    const BackgroundOperations operations{
        [state]() {
            state->mutex.reset(CreateMutexW(
                nullptr, TRUE, std::wstring{kBackgroundMutexName}.c_str()));
            if (!state->mutex) {
                const DWORD error = GetLastError();
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              HRESULT_FROM_WIN32(error), error};
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                state->mutex.reset();
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                              ERROR_ALREADY_EXISTS};
            }
            state->stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!state->stop_event) {
                const DWORD error = GetLastError();
                ReleaseMutex(state->mutex.get());
                state->mutex.reset();
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              HRESULT_FROM_WIN32(error), error};
            }
            const Status observer = state->observer.Start();
            if (!observer.ok()) {
                ReleaseMutex(state->mutex.get());
                state->mutex.reset();
                state->stop_event.reset();
                return observer;
            }
            g_background_stop_event.store(
                state->stop_event.get(), std::memory_order_release);
            if (SetConsoleCtrlHandler(BackgroundConsoleHandler, TRUE) == FALSE) {
                const DWORD error = GetLastError();
                g_background_stop_event.store(nullptr, std::memory_order_release);
                ReleaseMutex(state->mutex.get());
                state->mutex.reset();
                state->stop_event.reset();
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              HRESULT_FROM_WIN32(error), error};
            }
            state->console_handler = true;
            return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
        },
        [state]() {
            g_background_stop_event.store(nullptr, std::memory_order_release);
            if (state->console_handler) {
                SetConsoleCtrlHandler(BackgroundConsoleHandler, FALSE);
                state->console_handler = false;
            }
            state->stop_event.reset();
            if (state->mutex) {
                ReleaseMutex(state->mutex.get());
                state->mutex.reset();
            }
        },
        [state](BackgroundSnapshot* const output, bool* const stop) {
            if (output == nullptr || stop == nullptr || !state->stop_event) {
                return Status{ErrorCode::INVALID_ARGUMENT, E_INVALIDARG,
                              ERROR_INVALID_PARAMETER};
            }
            const DWORD waited = WaitForSingleObject(state->stop_event.get(), 250);
            if (waited == WAIT_OBJECT_0) {
                *stop = true;
                *output = {};
                return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
            }
            if (waited != WAIT_TIMEOUT) {
                const DWORD error = GetLastError();
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              HRESULT_FROM_WIN32(error), error};
            }
            if (state->sequence ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return Status{ErrorCode::IPC_PROTOCOL_ERROR,
                              HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW),
                              ERROR_ARITHMETIC_OVERFLOW};
            }
            *stop = false;
            return state->observer.Capture(++state->sequence, output);
        },
        [state](const DWORD pid, std::unique_ptr<BackgroundSession>* const output) {
            if (pid == 0 || output == nullptr || *output) {
                return Status{ErrorCode::INVALID_ARGUMENT, E_INVALIDARG,
                              ERROR_INVALID_PARAMETER};
            }
            try {
                *output = std::make_unique<ProductionBackgroundSession>(
                    pid,
                    CreateProductionExplorerSessionOperations(
                        pid, state->hook_dll_path));
                return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
            } catch (const std::bad_alloc&) {
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              E_OUTOFMEMORY, ERROR_SUCCESS};
            }
        },
        [](std::unique_ptr<BackgroundSession> session) {
            if (session) RetainedProductionSessions().push_back(std::move(session));
        },
    };
    return RunBackgroundCoordinator(operations);
}

}  // namespace winexinfo
