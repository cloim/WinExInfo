#include "host/explorer_session.h"

#include "common/win32_handle.h"
#include "host/explorer_controller.h"
#include "injection/hook_platform.h"
#include "ipc/named_pipe.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <ostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace winexinfo {
namespace {

Status Ok() noexcept { return {ErrorCode::OK, S_OK, ERROR_SUCCESS}; }

Status Invalid() noexcept {
    return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
}

Status Failed(
    const ErrorCode code = ErrorCode::WINDOW_ATTACH_FAILED,
    const DWORD error = ERROR_INVALID_STATE) noexcept {
    return {code, HRESULT_FROM_WIN32(error), error};
}

bool ExactSuccess(const Status& status) noexcept {
    return status.code == ErrorCode::OK && status.hresult == S_OK &&
        status.win32 == ERROR_SUCCESS;
}

bool ContainsThread(const std::vector<DWORD>& threads, const DWORD thread) {
    return std::find(threads.begin(), threads.end(), thread) != threads.end();
}

std::string HexHwnd(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(16)
           << std::setfill('0') << value;
    return output.str();
}

constexpr DWORD kSessionWaitMilliseconds = 5000;

DWORD RemainingWaitMilliseconds(
    const std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    return static_cast<DWORD>((std::max)(std::int64_t{1}, remaining));
}

bool WaitForPipeFrame(const HANDLE pipe, const DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    do {
        std::array<std::uint8_t, ipc::kFrameHeaderSize> header{};
        DWORD peeked = 0;
        DWORD available = 0;
        if (PeekNamedPipe(
                pipe,
                header.data(),
                static_cast<DWORD>(header.size()),
                &peeked,
                &available,
                nullptr) != FALSE &&
            peeked == static_cast<DWORD>(header.size())) {
            const std::uint32_t payload =
                static_cast<std::uint32_t>(header[8]) |
                static_cast<std::uint32_t>(header[9]) << 8 |
                static_cast<std::uint32_t>(header[10]) << 16 |
                static_cast<std::uint32_t>(header[11]) << 24;
            if (payload <= ipc::kMaximumFrameSize - ipc::kFrameHeaderSize &&
                available >= static_cast<DWORD>(ipc::kFrameHeaderSize) + payload) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool ConnectPipeBounded(const HANDLE pipe, const DWORD timeoutMs) {
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) == FALSE) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    bool connected = false;
    do {
        if (ConnectNamedPipe(pipe, nullptr) != FALSE ||
            GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = true;
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE &&
        connected;
}

Status ValidateProductionWindow(
    const DWORD processId,
    const SessionWindowSnapshot& snapshot,
    injection::HookTarget* const output) {
    if (output == nullptr || processId == 0 || snapshot.top_level == nullptr ||
        snapshot.top_level_generation == 0 || snapshot.tabs.empty() ||
        IsWindow(snapshot.top_level) == FALSE ||
        GetAncestor(snapshot.top_level, GA_ROOT) != snapshot.top_level) {
        return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
    }
    wchar_t className[64]{};
    if (GetClassNameW(snapshot.top_level, className, 64) == 0 ||
        std::wstring_view{className} != L"CabinetWClass") {
        return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
    }
    DWORD owner = 0;
    const DWORD thread = GetWindowThreadProcessId(snapshot.top_level, &owner);
    if (owner != processId || thread == 0 ||
        thread != snapshot.tabs.front().ui_thread_id) {
        return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
    }
    std::size_t tabIndex = 0;
    for (HWND child = GetWindow(snapshot.top_level, GW_CHILD);
         child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t childClass[64]{};
        if (GetClassNameW(child, childClass, 64) == 0) {
            return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
        }
        if (std::wstring_view{childClass} != L"ShellTabWindowClass") continue;
        DWORD childOwner = 0;
        const DWORD childThread = GetWindowThreadProcessId(child, &childOwner);
        if (tabIndex >= snapshot.tabs.size() || childOwner != processId ||
            childThread != thread ||
            snapshot.tabs[tabIndex].tab_hwnd !=
                reinterpret_cast<std::uint64_t>(child) ||
            snapshot.tabs[tabIndex].tab_generation == 0 ||
            snapshot.tabs[tabIndex].ui_thread_id != thread) {
            return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
        }
        ++tabIndex;
    }
    if (tabIndex != snapshot.tabs.size()) {
        return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
    }
    *output = {processId, thread, snapshot.top_level};
    return Ok();
}

struct ProductionSessionState final {
    explicit ProductionSessionState(std::wstring path)
        : hook_dll_path(std::move(path)) {}

    std::wstring hook_dll_path;
    UniqueHandle pipe;
    ExplorerControllerFileIdentityLock identity_lock;
    std::unique_ptr<injection::ThreadHookInjector> injector;
    std::function<Status()> pending_attach_validation;
    DWORD process_id = 0;
};

}  // namespace

class LifecycleDiagnosticJournal::Impl final {
public:
    explicit Impl(const std::size_t requestedCapacity)
        : capacity(requestedCapacity) {}
    const std::size_t capacity;
    mutable std::mutex mutex;
    std::vector<std::string> lines;
    std::unordered_map<std::string, std::string> last_by_key;
    std::atomic<bool> incomplete{false};
};

LifecycleDiagnosticJournal::LifecycleDiagnosticJournal(const std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

LifecycleDiagnosticJournal::~LifecycleDiagnosticJournal() = default;

bool LifecycleDiagnosticJournal::TryAppend(
    std::string dedupKey, std::string line) noexcept {
    if (!impl_) return false;
    if (impl_->capacity == 0) {
        impl_->incomplete.store(true, std::memory_order_release);
        return false;
    }
    std::unique_lock lock{impl_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        impl_->incomplete.store(true, std::memory_order_release);
        return false;
    }
    try {
        if (!dedupKey.empty()) {
            const auto prior = impl_->last_by_key.find(dedupKey);
            if (prior != impl_->last_by_key.end() && prior->second == line) {
                return true;
            }
        }
        if (impl_->lines.size() == impl_->capacity) {
            impl_->incomplete.store(true, std::memory_order_release);
            return false;
        }
        impl_->lines.push_back(line);
        if (!dedupKey.empty()) {
            impl_->last_by_key.insert_or_assign(
                std::move(dedupKey), std::move(line));
        }
        return true;
    } catch (...) {
        impl_->incomplete.store(true, std::memory_order_release);
        return false;
    }
}

bool LifecycleDiagnosticJournal::incomplete() const noexcept {
    return !impl_ || impl_->incomplete.load(std::memory_order_acquire);
}

std::vector<std::string> LifecycleDiagnosticJournal::Snapshot() const {
    std::scoped_lock lock{impl_->mutex};
    return impl_->lines;
}

void LifecycleDiagnosticJournal::Flush(std::ostream& output) const {
    std::scoped_lock lock{impl_->mutex};
    for (const std::string& line : impl_->lines) output << line << '\n';
    if (impl_->incomplete.load(std::memory_order_acquire)) {
        output << "LIFECYCLE_EVENT event=journal status=incomplete\n";
    }
    output.flush();
}

std::string FormatLifecycleAttachDiagnostic(
    const DWORD processId, const DWORD threadId, const HWND topLevel,
    const std::uint32_t result) {
    return "LIFECYCLE_EVENT event=attach pid=" + std::to_string(processId) +
        " tid=" + std::to_string(threadId) + " hwnd=" +
        HexHwnd(reinterpret_cast<std::uintptr_t>(topLevel)) +
        " result=" + std::to_string(result);
}

std::string FormatLifecycleUpdateDiagnostic(
    const DWORD processId, const std::uint64_t requestId,
    const ipc::TabSetUpdate& update, const ipc::TabSetResult& result) {
    std::string tabs;
    for (const ipc::TabDescriptor& tab : update.tabs) {
        if (!tabs.empty()) tabs.push_back(',');
        tabs += HexHwnd(tab.tab_hwnd) + ":" +
            std::to_string(tab.tab_generation) + ":" +
            std::to_string(tab.ui_thread_id);
    }
    return "LIFECYCLE_EVENT event=update pid=" + std::to_string(processId) +
        " request=" + std::to_string(requestId) + " hwnd=" +
        HexHwnd(update.top_level_hwnd) + " top_generation=" +
        std::to_string(update.top_level_generation) + " tabs=" + tabs +
        " result=" + std::to_string(result.result);
}

std::string FormatLifecycleRemoveDiagnostic(
    const DWORD processId, const std::uint64_t requestId,
    const ipc::WindowRemoveRequest& request,
    const ipc::WindowRemoveResult& result) {
    return "LIFECYCLE_EVENT event=remove pid=" + std::to_string(processId) +
        " request=" + std::to_string(requestId) + " hwnd=" +
        HexHwnd(request.top_level_hwnd) + " top_generation=" +
        std::to_string(request.top_level_generation) + " result=" +
        std::to_string(result.result);
}

std::string FormatLifecycleDetachDiagnostic(
    const DWORD processId, const std::uint64_t requestId,
    const ipc::DetachResult& result, const std::uint32_t finalResult) {
    return "LIFECYCLE_EVENT event=stop pid=" + std::to_string(processId) +
        " ack_pid=" + std::to_string(result.explorer_pid) +
        " request=" + std::to_string(requestId) + " result=" +
        std::to_string(finalResult) + " ack_result=" +
        std::to_string(result.result) + " pane=" +
        std::to_string(result.cleanup.pane_count) + " tab_subclass=" +
        std::to_string(result.cleanup.tab_subclass_count) +
        " parent_subclass=" +
        std::to_string(result.cleanup.parent_subclass_count) +
        " refresh_worker=" +
        std::to_string(result.cleanup.refresh_worker_count) + " callback=" +
        std::to_string(result.cleanup.callback_count);
}

ExplorerSession::ExplorerSession(
    const DWORD processId,
    ExplorerSessionOperations operations)
    : process_id_(processId), operations_(std::move(operations)) {}

void ExplorerSession::RecordDiagnostic(
    std::string key, std::string line) noexcept {
    if (operations_.diagnostic_journal) {
        static_cast<void>(operations_.diagnostic_journal->TryAppend(
            std::move(key), std::move(line)));
    }
}

Status ExplorerSession::NextRequestId(std::uint64_t* const output) noexcept {
    if (output == nullptr ||
        last_request_id_ == (std::numeric_limits<std::uint64_t>::max)()) {
        uncertain_ = true;
        return Failed(ErrorCode::IPC_PROTOCOL_ERROR, ERROR_ARITHMETIC_OVERFLOW);
    }
    *output = ++last_request_id_;
    return Ok();
}

Status ExplorerSession::SendUpdate(const SessionWindowSnapshot& snapshot) {
    std::uint64_t requestId = 0;
    Status status = NextRequestId(&requestId);
    std::vector<std::uint8_t> request;
    if (status.ok()) {
        status = ipc::EncodeTabSetUpdate(
            requestId,
            {reinterpret_cast<std::uint64_t>(snapshot.top_level),
             snapshot.top_level_generation,
             snapshot.tabs},
            &request);
    }
    ipc::DecodedFrame response{};
    if (status.ok()) status = operations_.exchange(request, &response);
    ipc::TabSetResult result{};
    if (status.ok()) {
        status = ipc::DecodeTabSetResult(
            response, requestId, snapshot.top_level_generation, &result);
    }
    if (!status.ok() || result.result != 0) {
        std::cerr << "BACKGROUND_UPDATE_FAILED pid=" << process_id_
                  << " hwnd="
                  << reinterpret_cast<std::uintptr_t>(snapshot.top_level)
                  << " request=" << requestId
                  << " transport_code=" << static_cast<int>(status.code)
                  << " result=" << result.result << '\n';
        uncertain_ = true;
        return status.ok()
            ? Failed(ErrorCode::WINDOW_ATTACH_FAILED, result.result)
            : status;
    }
    const auto prior = std::find_if(
        windows_.begin(), windows_.end(), [&snapshot](const WindowState& state) {
            return state.snapshot.top_level == snapshot.top_level;
        });
    if (prior == windows_.end() || prior->snapshot != snapshot) {
        RecordDiagnostic(
            "update:" + std::to_string(process_id_) + ":" +
                std::to_string(reinterpret_cast<std::uintptr_t>(snapshot.top_level)),
            FormatLifecycleUpdateDiagnostic(
                process_id_, requestId,
                {reinterpret_cast<std::uint64_t>(snapshot.top_level),
                 snapshot.top_level_generation, snapshot.tabs},
                result));
    }
    return Ok();
}

Status ExplorerSession::SendRemoval(const WindowState& window) {
    std::uint64_t requestId = 0;
    Status status = NextRequestId(&requestId);
    std::vector<std::uint8_t> request;
    if (status.ok()) {
        status = ipc::EncodeWindowRemoveRequest(
            requestId,
            {reinterpret_cast<std::uint64_t>(window.snapshot.top_level),
             window.snapshot.top_level_generation},
            &request);
    }
    ipc::DecodedFrame response{};
    if (status.ok()) status = operations_.exchange(request, &response);
    ipc::WindowRemoveResult result{};
    if (status.ok()) {
        status = ipc::DecodeWindowRemoveResult(
            response, requestId, window.snapshot.top_level_generation, &result);
    }
    if (!status.ok() || result.result != 0) {
        uncertain_ = true;
        return status.ok()
            ? Failed(ErrorCode::WINDOW_ATTACH_FAILED, result.result)
            : status;
    }
    RecordDiagnostic({}, FormatLifecycleRemoveDiagnostic(
        process_id_, requestId,
        {reinterpret_cast<std::uint64_t>(window.snapshot.top_level),
         window.snapshot.top_level_generation},
        result));
    return Ok();
}

Status ExplorerSession::SendPaneText(const SessionWindowSnapshot& snapshot) {
    if (snapshot.active_tab == nullptr || snapshot.active_tab_generation == 0 ||
        snapshot.display_text.empty() != snapshot.tooltip_text.empty()) {
        return Invalid();
    }
    std::uint64_t requestId = 0;
    Status status = NextRequestId(&requestId);
    std::vector<std::uint8_t> request;
    if (status.ok()) {
        status = ipc::EncodePaneTextUpdate(
            requestId,
            {reinterpret_cast<std::uint64_t>(snapshot.top_level),
             snapshot.top_level_generation,
             reinterpret_cast<std::uint64_t>(snapshot.active_tab),
             snapshot.active_tab_generation,
             snapshot.display_text,
             snapshot.tooltip_text},
            &request);
    }
    ipc::DecodedFrame response{};
    if (status.ok()) status = operations_.exchange(request, &response);
    ipc::PaneTextResult result{};
    if (status.ok()) {
        status = ipc::DecodePaneTextResult(
            response, requestId, snapshot.top_level_generation,
            snapshot.active_tab_generation, &result);
    }
    if (!status.ok() || result.result != 0) {
        std::cerr << "BACKGROUND_PANE_TEXT_FAILED pid=" << process_id_
                  << " hwnd="
                  << reinterpret_cast<std::uintptr_t>(snapshot.top_level)
                  << " request=" << requestId
                  << " code=" << static_cast<int>(status.code)
                  << " hresult=" << status.hresult
                  << " win32=" << status.win32
                  << " result=" << result.result << '\n';
        uncertain_ = true;
        return status.ok()
            ? Failed(ErrorCode::WINDOW_ATTACH_FAILED, result.result)
            : status;
    }
    return Ok();
}

Status ExplorerSession::Reconcile(
    const std::span<const SessionWindowSnapshot> windows) {
    std::scoped_lock lock{mutex_};
    if (process_id_ == 0 || stopped_ || uncertain_ ||
        (windows.empty() && !attached_) ||
        windows.size() > 64 || !operations_.validate_window ||
        !operations_.attach_initial || !operations_.ensure_thread_hook_lease ||
        !operations_.exchange) {
        return Invalid();
    }

    try {
        std::vector<WindowState> candidate;
        candidate.reserve(windows.size());
        for (const SessionWindowSnapshot& snapshot : windows) {
            if (snapshot.top_level == nullptr ||
                snapshot.top_level_generation == 0 || snapshot.tabs.empty() ||
                std::find_if(
                    candidate.begin(),
                    candidate.end(),
                    [&snapshot](const WindowState& existing) {
                        return existing.snapshot.top_level == snapshot.top_level;
                    }) != candidate.end()) {
                return Invalid();
            }
            injection::HookTarget target{};
            const Status validated = operations_.validate_window(snapshot, &target);
            if (!ExactSuccess(validated) || target.explorer_pid != process_id_ ||
                target.top_level_hwnd != snapshot.top_level ||
                target.ui_thread_id == 0 ||
                target.ui_thread_id != snapshot.tabs.front().ui_thread_id ||
                std::any_of(
                    snapshot.tabs.begin(),
                    snapshot.tabs.end(),
                    [&target](const ipc::TabDescriptor& tab) {
                        return tab.ui_thread_id != target.ui_thread_id;
                    })) {
                return validated.ok()
                    ? Failed(ErrorCode::TARGET_VALIDATION_FAILED)
                    : validated;
            }
            const auto prior = std::find_if(
                windows_.begin(),
                windows_.end(),
                [&snapshot](const WindowState& existing) {
                    return existing.snapshot.top_level == snapshot.top_level;
                });
            if (prior != windows_.end() &&
                snapshot.top_level_generation <
                    prior->snapshot.top_level_generation) {
                return Failed(ErrorCode::IPC_PROTOCOL_ERROR);
            }
            candidate.push_back({snapshot, target.ui_thread_id});
        }

        std::vector<DWORD> candidateThreads = installed_thread_ids_;
        if (!attached_) {
            const WindowState& first = candidate.front();
            const injection::HookTarget target{
                process_id_, first.ui_thread_id, first.snapshot.top_level};
            injection::HookAttachOutcome outcome{};
            const Status attached = operations_.attach_initial(
                target,
                [this, snapshot = first.snapshot, target]() {
                    injection::HookTarget checked{};
                    const Status status =
                        operations_.validate_window(snapshot, &checked);
                    return ExactSuccess(status) &&
                            checked.explorer_pid == target.explorer_pid &&
                            checked.ui_thread_id == target.ui_thread_id &&
                            checked.top_level_hwnd == target.top_level_hwnd
                        ? Ok()
                        : status.ok()
                            ? Failed(ErrorCode::TARGET_VALIDATION_FAILED)
                            : status;
                },
                &outcome);
            if (!ExactSuccess(attached) || !outcome.unload_authorized) {
                std::cerr << "BACKGROUND_ATTACH_FAILED pid=" << process_id_
                          << " tid=" << first.ui_thread_id
                          << " code=" << static_cast<int>(attached.code)
                          << " hresult=" << attached.hresult
                          << " win32=" << attached.win32
                          << " unload_authorized=" << outcome.unload_authorized
                          << '\n';
                uncertain_ = true;
                return attached.ok()
                    ? Failed(ErrorCode::WINDOW_ATTACH_FAILED)
                    : attached;
            }
            attached_ = true;
            candidateThreads.push_back(first.ui_thread_id);
            RecordDiagnostic({}, FormatLifecycleAttachDiagnostic(
                process_id_, first.ui_thread_id,
                first.snapshot.top_level, 0));
        }

        for (const WindowState& window : candidate) {
            const injection::HookTarget target{
                process_id_, window.ui_thread_id, window.snapshot.top_level};
            const Status ensured = operations_.ensure_thread_hook_lease(
                target,
                [this, snapshot = window.snapshot, target]() {
                    injection::HookTarget checked{};
                    const Status status =
                        operations_.validate_window(snapshot, &checked);
                    return ExactSuccess(status) &&
                            checked.explorer_pid == target.explorer_pid &&
                            checked.ui_thread_id == target.ui_thread_id &&
                            checked.top_level_hwnd == target.top_level_hwnd
                        ? Ok()
                        : status.ok()
                            ? Failed(ErrorCode::TARGET_VALIDATION_FAILED)
                            : status;
                });
            if (!ExactSuccess(ensured)) {
                std::cerr << "BACKGROUND_ENSURE_HOOK_FAILED pid="
                          << process_id_ << " tid=" << window.ui_thread_id
                          << " code=" << static_cast<int>(ensured.code)
                          << " hresult=" << ensured.hresult
                          << " win32=" << ensured.win32 << '\n';
                uncertain_ = true;
                return ensured;
            }
            if (!ContainsThread(candidateThreads, window.ui_thread_id)) {
                candidateThreads.push_back(window.ui_thread_id);
            }
            const Status updated = SendUpdate(window.snapshot);
            if (!updated.ok()) return updated;
            const auto priorWindow = std::find_if(
                windows_.begin(), windows_.end(),
                [&window](const WindowState& existing) {
                    return existing.snapshot.top_level == window.snapshot.top_level;
                });
            const bool paneChanged = priorWindow == windows_.end() ||
                priorWindow->snapshot.active_tab != window.snapshot.active_tab ||
                priorWindow->snapshot.active_tab_generation !=
                    window.snapshot.active_tab_generation ||
                priorWindow->snapshot.display_text != window.snapshot.display_text ||
                priorWindow->snapshot.tooltip_text != window.snapshot.tooltip_text;
            if (window.snapshot.active_tab != nullptr && paneChanged) {
                const Status text = SendPaneText(window.snapshot);
                if (!text.ok()) return text;
            }
        }

        for (auto prior = windows_.rbegin(); prior != windows_.rend(); ++prior) {
            const bool present = std::any_of(
                candidate.begin(),
                candidate.end(),
                [&prior](const WindowState& current) {
                    return current.snapshot.top_level == prior->snapshot.top_level;
                });
            if (!present) {
                const Status removed = SendRemoval(*prior);
                if (!removed.ok()) return removed;
            }
        }
        installed_thread_ids_ = std::move(candidateThreads);
        windows_ = std::move(candidate);
        return Ok();
    } catch (const std::bad_alloc&) {
        uncertain_ = attached_;
        return {ErrorCode::WINDOW_ATTACH_FAILED, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

Status ExplorerSession::Stop() {
    std::scoped_lock lock{mutex_};
    if (process_id_ == 0 || stopped_ || uncertain_ ||
        !operations_.release_hooks || !operations_.exchange ||
        !operations_.wait_exact_module_absent ||
        !operations_.confirm_target_gone) {
        return Invalid();
    }
    if (!attached_) {
        stopped_ = true;
        return Ok();
    }
    if (!release_complete_) {
        const Status released = operations_.release_hooks(process_id_);
        if (!ExactSuccess(released)) return released;
        release_complete_ = true;
    }
    if (!detach_complete_) {
        std::uint64_t requestId = 0;
        Status status = NextRequestId(&requestId);
        std::vector<std::uint8_t> request;
        if (status.ok()) status = ipc::EncodeDetachRequest(requestId, &request);
        ipc::DecodedFrame response{};
        if (status.ok()) status = operations_.exchange(request, &response);
        ipc::DetachResult result{};
        if (status.ok()) status = ipc::DecodeDetachResult(response, requestId, &result);
        if (status.ok()) {
            last_detach_result_ = result;
            last_detach_request_id_ = requestId;
            has_detach_result_ = true;
        }
        if (!status.ok() || result.explorer_pid != process_id_ || result.result != 0) {
            uncertain_ = true;
            if (status.ok()) {
                RecordDiagnostic({}, FormatLifecycleDetachDiagnostic(
                    process_id_, requestId, result,
                    static_cast<std::uint32_t>(ErrorCode::DLL_UNLOAD_TIMEOUT)));
            }
            return status.ok()
                ? Failed(ErrorCode::DLL_UNLOAD_TIMEOUT, result.result)
                : status;
        }
        detach_complete_ = true;
    }
    bool absent = false;
    Status status = operations_.wait_exact_module_absent(
        process_id_, kSessionWaitMilliseconds, &absent);
    if (!ExactSuccess(status) || !absent) {
        if (has_detach_result_) {
            const std::uint32_t finalResult = status.ok()
                ? static_cast<std::uint32_t>(ErrorCode::DLL_UNLOAD_TIMEOUT)
                : static_cast<std::uint32_t>(status.code);
            RecordDiagnostic({}, FormatLifecycleDetachDiagnostic(
                process_id_, last_detach_request_id_, last_detach_result_,
                finalResult));
        }
        return status.ok()
            ? Failed(ErrorCode::DLL_UNLOAD_TIMEOUT, ERROR_TIMEOUT)
            : status;
    }
    status = operations_.confirm_target_gone(process_id_);
    if (!ExactSuccess(status)) {
        if (has_detach_result_) {
            RecordDiagnostic({}, FormatLifecycleDetachDiagnostic(
                process_id_, last_detach_request_id_, last_detach_result_,
                static_cast<std::uint32_t>(status.code)));
        }
        return status;
    }
    windows_.clear();
    installed_thread_ids_.clear();
    attached_ = false;
    stopped_ = true;
    RecordDiagnostic({}, FormatLifecycleDetachDiagnostic(
        process_id_, last_detach_request_id_, last_detach_result_, 0));
    return Ok();
}

ExplorerSessionOperations CreateProductionExplorerSessionOperations(
    const DWORD processId,
    std::wstring hookDllPath,
    std::shared_ptr<LifecycleDiagnosticJournal> diagnosticJournal) {
    auto state = std::make_shared<ProductionSessionState>(std::move(hookDllPath));
    state->process_id = processId;
    return {
        [state](const SessionWindowSnapshot& snapshot,
                injection::HookTarget* const output) {
            return ValidateProductionWindow(state->process_id, snapshot, output);
        },
        [state](const injection::HookTarget& target,
                const std::function<Status()>& finalValidate,
                injection::HookAttachOutcome* const output) {
            if (state->injector || state->pipe || !finalValidate ||
                output == nullptr || target.explorer_pid == 0 ||
                state->hook_dll_path.empty()) {
                return Invalid();
            }
            if (state->process_id != target.explorer_pid) return Invalid();
            if (!ExactSuccess(finalValidate())) {
                return Failed(ErrorCode::TARGET_VALIDATION_FAILED);
            }
            if (AcquireExplorerControllerFileIdentityLock(
                    state->hook_dll_path, &state->identity_lock) !=
                HostExitCode::Pass) {
                return Failed(ErrorCode::HOOK_INSTALL_FAILED);
            }
            std::wstring pipeName;
            Status status = ipc::BuildCurrentUserPipeNameForProcess(
                target.explorer_pid, &pipeName);
            if (!status.ok()) {
                std::cerr << "BACKGROUND_PIPE_FAILED stage=name pid="
                          << target.explorer_pid << " win32=" << status.win32 << '\n';
            }
            if (status.ok()) {
                status = ipc::CreateControllerPipeServer(pipeName, &state->pipe);
                if (!status.ok()) {
                    std::cerr << "BACKGROUND_PIPE_FAILED stage=create pid="
                              << target.explorer_pid << " win32="
                              << status.win32 << '\n';
                }
            }
            if (!status.ok()) return status;
            auto platform = injection::CreateProductionHookPlatformOperations(
                state->hook_dll_path,
                [state](const std::uint64_t requestId,
                        const DWORD timeout,
                        injection::HookAttachReceipt* const receipt) {
                    const DWORD budget =
                        (std::min)(timeout, kSessionWaitMilliseconds);
                    const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(budget);
                    DWORD remaining = RemainingWaitMilliseconds(deadline);
                    if (receipt == nullptr || !state->pipe || remaining == 0 ||
                        !ConnectPipeBounded(state->pipe.get(), remaining)) {
                        return Failed(ErrorCode::WINDOW_ATTACH_FAILED, ERROR_TIMEOUT);
                    }
                    ULONG clientPid = 0;
                    remaining = RemainingWaitMilliseconds(deadline);
                    if (GetNamedPipeClientProcessId(state->pipe.get(), &clientPid) == FALSE ||
                        clientPid != state->process_id ||
                        remaining == 0 ||
                        !WaitForPipeFrame(state->pipe.get(), remaining)) {
                        return Failed(ErrorCode::WINDOW_ATTACH_FAILED, ERROR_TIMEOUT);
                    }
                    ipc::DecodedFrame frame{};
                    Status read = ipc::ReadFrame(&state->pipe, &frame);
                    ipc::AttachResult result{};
                    if (!read.ok() || frame.request_id != requestId ||
                        !ipc::DecodeAttachResult(frame, &result).ok()) {
                        return Failed(ErrorCode::IPC_PROTOCOL_ERROR);
                    }
                    *receipt = {
                        true,
                        frame.request_id,
                        result.explorer_pid,
                        result.ui_thread_id,
                        reinterpret_cast<HWND>(result.top_level_hwnd),
                        result.result,
                        result.error_code,
                    };
                    return Ok();
                });
            platform.before_set_hook = [state]() {
                return state->pending_attach_validation
                    ? state->pending_attach_validation()
                    : Failed(ErrorCode::TARGET_VALIDATION_FAILED);
            };
            state->pending_attach_validation = finalValidate;
            state->injector =
                std::make_unique<injection::ThreadHookInjector>(std::move(platform));
            status = state->injector->Attach(target, output);
            if (!status.ok()) {
                std::cerr << "BACKGROUND_INJECTOR_FAILED pid="
                          << target.explorer_pid << " tid="
                          << target.ui_thread_id << " code="
                          << static_cast<int>(status.code) << " win32="
                          << status.win32 << '\n';
            }
            state->pending_attach_validation = {};
            return status;
        },
        [state](const injection::HookTarget& target,
                const std::function<Status()>& finalValidate) {
            return state->injector
                ? state->injector->EnsureThreadHookLease(target, finalValidate)
                : Failed(ErrorCode::HOOK_INSTALL_FAILED);
        },
        [state](const std::vector<std::uint8_t>& request,
                ipc::DecodedFrame* const response) {
            if (!state->pipe || response == nullptr) return Invalid();
            Status status = ipc::WriteFrame(state->pipe.get(), request);
            if (!status.ok()) return status;
            if (!WaitForPipeFrame(state->pipe.get(), kSessionWaitMilliseconds)) {
                return Failed(ErrorCode::PIPE_DISCONNECTED, ERROR_TIMEOUT);
            }
            status = ipc::ReadFrame(&state->pipe, response);
            return status;
        },
        [state](const DWORD processId) {
            return state->injector
                ? state->injector->ReleaseHookForDetach(processId)
                : Failed(ErrorCode::HOOK_RELEASE_FAILED);
        },
        [state](const DWORD processId, const DWORD timeout, bool* const absent) {
            if (absent == nullptr || processId == 0 ||
                timeout == 0 || timeout > kSessionWaitMilliseconds ||
                !state->identity_lock.file) {
                return Invalid();
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeout);
            do {
                UniqueHandle snapshot{CreateToolhelp32Snapshot(
                    TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId)};
                if (!snapshot) {
                    return Failed(ErrorCode::DLL_UNLOAD_TIMEOUT, GetLastError());
                }
                bool present = false;
                MODULEENTRY32W entry{sizeof(entry)};
                if (Module32FirstW(snapshot.get(), &entry) == FALSE) {
                    if (GetLastError() != ERROR_NO_MORE_FILES) {
                        return Failed(
                            ErrorCode::DLL_UNLOAD_TIMEOUT, GetLastError());
                    }
                } else {
                    do {
                        bool matches = false;
                        if (CheckExplorerControllerFileIdentity(
                                entry.szExePath,
                                state->identity_lock.identity,
                                &matches) != HostExitCode::Pass) {
                            return Failed(ErrorCode::DLL_UNLOAD_TIMEOUT);
                        }
                        if (matches) {
                            present = true;
                            break;
                        }
                    } while (Module32NextW(snapshot.get(), &entry) != FALSE);
                    if (!present && GetLastError() != ERROR_NO_MORE_FILES) {
                        return Failed(
                            ErrorCode::DLL_UNLOAD_TIMEOUT, GetLastError());
                    }
                }
                if (!present) {
                    *absent = true;
                    return Ok();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < deadline);
            *absent = false;
            return Ok();
        },
        [state](const DWORD processId) {
            return state->injector
                ? state->injector->ConfirmTargetGone(processId)
                : Failed(ErrorCode::HOOK_RELEASE_FAILED);
        },
        std::move(diagnosticJournal),
    };
}

}  // namespace winexinfo
