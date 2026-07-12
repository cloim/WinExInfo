#include "host/explorer_controller.h"

#include "common/win32_handle.h"
#include "hook/status_pane.h"
#include "injection/hook_platform.h"
#include "injection/thread_hook_injector.h"
#include "ipc/named_pipe.h"
#include "ipc/protocol.h"
#include "probe/target_validator.h"

#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace winexinfo {
namespace {

constexpr std::wstring_view kExplorerWindowClass = L"CabinetWClass";
constexpr DWORD kBoundedWaitMs = 5000;

bool ValidOperations(const ExplorerControllerOperations& operations) {
    return operations.inspect_target && operations.emit_target_accepted &&
        operations.target_still_matches && operations.attach &&
        operations.query_pane_owner && operations.wait_duration &&
        operations.detach && operations.pane_absent &&
        operations.exact_module_absent && operations.release_retained_target;
}

bool IsExactTargetWindow(
    const HWND window,
    const ExplorerControllerTarget& expected) {
    if (window == nullptr || IsWindow(window) == FALSE ||
        GetAncestor(window, GA_ROOT) != window) {
        return false;
    }
    wchar_t className[64]{};
    if (GetClassNameW(window, className, 64) == 0 ||
        std::wstring_view{className} != kExplorerWindowClass) {
        return false;
    }
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    return processId == expected.process_id && threadId == expected.thread_id &&
        expected.top_level == window;
}

enum class FileComparison { Same, Different, Error };

FileComparison CompareFileIdentity(
    const std::wstring_view firstPath,
    const std::wstring_view secondPath) {
    UniqueHandle first{CreateFileW(
        std::wstring{firstPath}.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    UniqueHandle second{CreateFileW(
        std::wstring{secondPath}.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!first || !second) {
        return FileComparison::Error;
    }
    FILE_ID_INFO firstInfo{};
    FILE_ID_INFO secondInfo{};
    if (GetFileInformationByHandleEx(
            first.get(), FileIdInfo, &firstInfo, sizeof(firstInfo)) == FALSE ||
        GetFileInformationByHandleEx(
            second.get(), FileIdInfo, &secondInfo, sizeof(secondInfo)) == FALSE) {
        return FileComparison::Error;
    }
    return firstInfo.VolumeSerialNumber == secondInfo.VolumeSerialNumber &&
        std::equal(
            std::begin(firstInfo.FileId.Identifier),
            std::end(firstInfo.FileId.Identifier),
            std::begin(secondInfo.FileId.Identifier))
        ? FileComparison::Same
        : FileComparison::Different;
}

enum class ModulePresence { Present, Absent, Error };

ModulePresence QueryExactModulePresence(
    const DWORD processId,
    const std::wstring_view hookDllPath) {
    UniqueHandle snapshot{CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId)};
    if (!snapshot) {
        return ModulePresence::Error;
    }
    MODULEENTRY32W entry{sizeof(entry)};
    if (Module32FirstW(snapshot.get(), &entry) == FALSE) {
        return GetLastError() == ERROR_NO_MORE_FILES
            ? ModulePresence::Absent
            : ModulePresence::Error;
    }
    do {
        const FileComparison comparison =
            CompareFileIdentity(entry.szExePath, hookDllPath);
        if (comparison == FileComparison::Error) {
            return ModulePresence::Error;
        }
        if (comparison == FileComparison::Same) {
            return ModulePresence::Present;
        }
    } while (Module32NextW(snapshot.get(), &entry) != FALSE);
    return GetLastError() == ERROR_NO_MORE_FILES
        ? ModulePresence::Absent
        : ModulePresence::Error;
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
        if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE &&
        connected;
}

bool WaitForPipeFrame(const HANDLE pipe, const DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    do {
        std::array<std::uint8_t, ipc::kFrameHeaderSize> header{};
        DWORD peeked = 0;
        DWORD available = 0;
        if (PeekNamedPipe(
                pipe, header.data(), static_cast<DWORD>(header.size()),
                &peeked, &available, nullptr) != FALSE &&
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

struct ProductionControllerState final {
    UniqueHandle pipe;
    std::unique_ptr<injection::ThreadHookInjector> injector;
    std::uint64_t detach_id = 0;
};

HostExitCode StatusExit(const Status& status) {
    return status.code == ErrorCode::WINDOW_ATTACH_FAILED ||
        status.code == ErrorCode::TARGET_VALIDATION_FAILED ||
        status.code == ErrorCode::HOOK_RELEASE_FAILED
        ? HostExitCode::ContractFailure
        : HostExitCode::Win32ComFailure;
}

ExplorerControllerOperations ProductionOperations(
    const std::shared_ptr<ProductionControllerState>& state) {
    return {
        [](const HWND window, ExplorerControllerTarget* const output) {
            if (output == nullptr || window == nullptr || IsWindow(window) == FALSE ||
                GetAncestor(window, GA_ROOT) != window) {
                return HostExitCode::ContractFailure;
            }
            wchar_t className[64]{};
            if (GetClassNameW(window, className, 64) == 0 ||
                std::wstring_view{className} != kExplorerWindowClass) {
                return HostExitCode::ContractFailure;
            }
            DWORD processId = 0;
            const DWORD threadId = GetWindowThreadProcessId(window, &processId);
            if (processId == 0 || threadId == 0) {
                return HostExitCode::ContractFailure;
            }
            TargetValidationEvidence evidence{};
            const Status captured =
                CaptureTargetValidationEvidence(processId, &evidence);
            if (!captured.ok()) {
                return HostExitCode::Win32ComFailure;
            }
            if (!ValidateTargetEvidence(evidence).status.ok()) {
                return HostExitCode::ContractFailure;
            }
            *output = {processId, threadId, window};
            return HostExitCode::Pass;
        },
        [](const ExplorerControllerTarget& target) {
            std::ostringstream accepted;
            accepted << "TARGET_ACCEPTED protocol=1 pid=" << target.process_id
                     << " tid=" << target.thread_id << " hwnd=0x"
                     << std::uppercase << std::hex << std::setw(16)
                     << std::setfill('0')
                     << reinterpret_cast<std::uintptr_t>(target.top_level);
            std::cout << accepted.str() << '\n' << std::flush;
        },
        [](const ExplorerControllerTarget& target) {
            return IsExactTargetWindow(target.top_level, target);
        },
        [state](const ExplorerControllerTarget& target,
                const std::wstring_view hookDllPath) {
            std::wstring pipeName;
            Status status = ipc::BuildCurrentUserPipeName(&pipeName);
            if (status.ok()) {
                status = ipc::CreateControllerPipeServer(pipeName, &state->pipe);
            }
            if (!status.ok()) {
                return HostExitCode::Win32ComFailure;
            }
            auto platform = injection::CreateProductionHookPlatformOperations(
                std::wstring{hookDllPath},
                [state, expectedPid = target.process_id](
                    const std::uint64_t attachId,
                    const DWORD timeoutMs,
                    injection::HookAttachReceipt* const output) {
                    if (!state->pipe || timeoutMs > kBoundedWaitMs) {
                        return Status{ErrorCode::PIPE_DISCONNECTED,
                                      S_FALSE, ERROR_INVALID_HANDLE};
                    }
                    if (!ConnectPipeBounded(state->pipe.get(), timeoutMs)) {
                        return Status{ErrorCode::PIPE_DISCONNECTED,
                                      HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                                      ERROR_TIMEOUT};
                    }
                    ULONG clientPid = 0;
                    if (GetNamedPipeClientProcessId(
                            state->pipe.get(), &clientPid) == FALSE ||
                        clientPid != expectedPid ||
                        !WaitForPipeFrame(state->pipe.get(), timeoutMs)) {
                        return Status{ErrorCode::TARGET_VALIDATION_FAILED,
                                      S_FALSE, ERROR_ACCESS_DENIED};
                    }
                    ipc::DecodedFrame frame{};
                    ipc::AttachResult result{};
                    Status read = ipc::ReadFrame(&state->pipe, &frame);
                    if (!read.ok() ||
                        frame.message_type != ipc::MessageType::AttachResult ||
                        frame.request_id != attachId ||
                        !ipc::DecodeAttachResult(frame, &result).ok()) {
                        return Status{ErrorCode::IPC_PROTOCOL_ERROR,
                                      S_FALSE, ERROR_INVALID_DATA};
                    }
                    *output = {
                        true, frame.request_id, result.explorer_pid,
                        result.ui_thread_id,
                        reinterpret_cast<HWND>(result.top_level_hwnd),
                        result.result, result.error_code};
                    return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
                });
            state->injector =
                std::make_unique<injection::ThreadHookInjector>(std::move(platform));
            injection::HookAttachOutcome outcome{};
            status = state->injector->Attach(
                {target.process_id, target.thread_id, target.top_level}, &outcome);
            if (!status.ok() || !outcome.unload_authorized) {
                return StatusExit(status);
            }
            return HostExitCode::Pass;
        },
        [](const ExplorerControllerTarget& target, DWORD* const ownerPid) {
            struct PaneMatch final {
                HWND pane = nullptr;
                std::uint32_t count = 0;
            } match;
            if (ownerPid == nullptr) {
                return HostExitCode::ContractFailure;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kBoundedWaitMs);
            do {
                match = {};
                EnumChildWindows(
                    target.top_level,
                    [](const HWND child, const LPARAM value) {
                        wchar_t name[64]{};
                        if (GetClassNameW(child, name, 64) != 0 &&
                            std::wstring_view{name} == hook::kStatusPaneClassName) {
                            auto* found = reinterpret_cast<PaneMatch*>(value);
                            found->pane = child;
                            ++found->count;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&match));
                if (match.count == 1 && match.pane != nullptr) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < deadline);
            if (match.count != 1 || match.pane == nullptr) {
                return HostExitCode::ContractFailure;
            }
            GetWindowThreadProcessId(match.pane, ownerPid);
            return *ownerPid != 0
                ? HostExitCode::Pass
                : HostExitCode::ContractFailure;
        },
        [](const std::uint32_t durationMs) {
            std::uint32_t remaining = durationMs;
            while (remaining > 0) {
                const std::uint32_t slice =
                    (std::min)(
                        remaining, static_cast<std::uint32_t>(kBoundedWaitMs));
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                remaining -= slice;
            }
        },
        [state](const ExplorerControllerTarget& target) {
            if (!state->pipe || !state->injector) {
                return HostExitCode::Win32ComFailure;
            }
            std::vector<std::uint8_t> frame;
            Status status = ipc::EncodeDetachRequest(++state->detach_id, &frame);
            if (status.ok()) {
                status = ipc::WriteFrame(state->pipe.get(), frame);
            }
            if (!status.ok() ||
                !WaitForPipeFrame(state->pipe.get(), kBoundedWaitMs)) {
                return HostExitCode::Win32ComFailure;
            }
            ipc::DecodedFrame response{};
            ipc::DetachResult result{};
            status = ipc::ReadFrame(&state->pipe, &response);
            if (!status.ok()) {
                return HostExitCode::Win32ComFailure;
            }
            if (!ipc::DecodeDetachResult(
                    response, state->detach_id, &result).ok() ||
                result.explorer_pid != target.process_id || result.result != 0) {
                return HostExitCode::ContractFailure;
            }
            state->pipe.reset();
            return HostExitCode::Pass;
        },
        [](const ExplorerControllerTarget& target) {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kBoundedWaitMs);
            do {
                bool found = false;
                EnumChildWindows(
                    target.top_level,
                    [](const HWND child, const LPARAM value) {
                        wchar_t name[64]{};
                        if (GetClassNameW(child, name, 64) != 0 &&
                            std::wstring_view{name} == hook::kStatusPaneClassName) {
                            *reinterpret_cast<bool*>(value) = true;
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&found));
                if (!found) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        },
        [](const DWORD processId, const std::wstring_view hookDllPath) {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kBoundedWaitMs);
            do {
                if (QueryExactModulePresence(processId, hookDllPath) ==
                    ModulePresence::Absent) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        },
        [state](const DWORD processId) {
            if (state->injector) {
                static_cast<void>(state->injector->ConfirmTargetGone(processId));
            }
        },
    };
}

}  // namespace

HostExitCode RunGateCPlacementWithOperations(
    const HWND target,
    const std::uint32_t durationMs,
    const std::wstring_view hookDllPath,
    const ExplorerControllerOperations& operations) {
    if (target == nullptr || durationMs == 0 || hookDllPath.empty() ||
        !ValidOperations(operations)) {
        return HostExitCode::Win32ComFailure;
    }
    ExplorerControllerTarget inspected{};
    HostExitCode result = operations.inspect_target(target, &inspected);
    if (result != HostExitCode::Pass) {
        return result;
    }
    if (inspected.process_id == 0 || inspected.thread_id == 0 ||
        inspected.top_level != target) {
        return HostExitCode::ContractFailure;
    }
    operations.emit_target_accepted(inspected);
    if (!operations.target_still_matches(inspected)) {
        return HostExitCode::ContractFailure;
    }
    result = operations.attach(inspected, hookDllPath);
    if (result != HostExitCode::Pass) {
        return result;
    }

    DWORD paneOwner = 0;
    HostExitCode primary = operations.query_pane_owner(inspected, &paneOwner);
    if (primary == HostExitCode::Pass && paneOwner != inspected.process_id) {
        primary = HostExitCode::ContractFailure;
    }
    if (primary == HostExitCode::Pass) {
        operations.wait_duration(durationMs);
    }
    const HostExitCode detached = operations.detach(inspected);
    const bool paneAbsent = operations.pane_absent(inspected);
    const bool moduleAbsent =
        operations.exact_module_absent(inspected.process_id, hookDllPath);
    if (detached != HostExitCode::Pass || !paneAbsent || !moduleAbsent) {
        return detached == HostExitCode::Win32ComFailure
            ? HostExitCode::Win32ComFailure
            : HostExitCode::ContractFailure;
    }
    operations.release_retained_target(inspected.process_id);
    return primary;
}

HostExitCode RunGateCPlacement(
    const HWND target,
    const std::uint32_t durationMs,
    const std::wstring_view hookDllPath) {
    auto state = std::make_shared<ProductionControllerState>();
    const HostExitCode result = RunGateCPlacementWithOperations(
        target, durationMs, hookDllPath, ProductionOperations(state));
    if (result != HostExitCode::Pass) {
        std::cerr << "GATE_C_FAILED\n";
    }
    return result;
}

}  // namespace winexinfo
