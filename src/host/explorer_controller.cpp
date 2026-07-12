#include "host/explorer_controller.h"

#include "common/win32_handle.h"
#include "hook/status_pane.h"
#include "injection/hook_platform.h"
#include "injection/thread_hook_injector.h"
#include "ipc/named_pipe.h"
#include "ipc/protocol.h"
#include "probe/target_validator.h"

#include <TlHelp32.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
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
        operations.attach && operations.query_pane_owner && operations.wait_duration &&
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

HostExitCode OpenFileIdentity(
    const std::wstring_view path,
    const DWORD desiredAccess,
    const DWORD shareMode,
    UniqueHandle* const file,
    ExplorerControllerFileIdentity* const output) {
    if (file == nullptr || *file || output == nullptr || path.empty() ||
        path.find(L'\0') != std::wstring_view::npos) {
        return HostExitCode::Win32ComFailure;
    }
    UniqueHandle candidateFile{CreateFileW(
        std::wstring{path}.c_str(), desiredAccess,
        shareMode,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!candidateFile) {
        return HostExitCode::Win32ComFailure;
    }
    FILE_ID_INFO information{};
    if (GetFileInformationByHandleEx(
            candidateFile.get(), FileIdInfo,
            &information, sizeof(information)) == FALSE) {
        return HostExitCode::Win32ComFailure;
    }
    ExplorerControllerFileIdentity candidate{};
    candidate.volume_serial = information.VolumeSerialNumber;
    std::copy(
        std::begin(information.FileId.Identifier),
        std::end(information.FileId.Identifier),
        candidate.file_id.begin());
    *file = std::move(candidateFile);
    *output = candidate;
    return HostExitCode::Pass;
}

HostExitCode DosPathFromMappedDevicePath(
    const std::wstring_view devicePath,
    std::wstring* const output) {
    if (output == nullptr || devicePath.empty()) {
        return HostExitCode::Win32ComFailure;
    }
    wchar_t drives[512]{};
    const DWORD length = GetLogicalDriveStringsW(512, drives);
    if (length == 0 || length >= 512) {
        return HostExitCode::Win32ComFailure;
    }
    std::vector<ExplorerControllerDeviceMapping> mappings;
    for (const wchar_t* drive = drives; *drive != L'\0';
         drive += std::wcslen(drive) + 1) {
        const std::wstring driveName{drive, 2};
        wchar_t device[32768]{};
        if (QueryDosDeviceW(driveName.c_str(), device, 32768) == 0) {
            return HostExitCode::Win32ComFailure;
        }
        mappings.push_back({device, driveName});
    }
    return SelectExplorerControllerDosPath(devicePath, mappings, output);
}

enum class ModulePresence { Present, Absent, Error };

HostExitCode QueryExactModulePresence(
    const DWORD processId,
    const ExplorerControllerFileIdentity& expected,
    ModulePresence* const output) {
    if (output == nullptr) {
        return HostExitCode::Win32ComFailure;
    }
    UniqueHandle process{OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId)};
    UniqueHandle snapshot{CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId)};
    if (!process || !snapshot) {
        return HostExitCode::Win32ComFailure;
    }
    MODULEENTRY32W entry{sizeof(entry)};
    if (Module32FirstW(snapshot.get(), &entry) == FALSE) {
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            return HostExitCode::Win32ComFailure;
        }
        *output = ModulePresence::Absent;
        return HostExitCode::Pass;
    }
    do {
        std::wstring mappedDevicePath(32768, L'\0');
        const DWORD mappedLength = K32GetMappedFileNameW(
            process.get(), entry.modBaseAddr,
            mappedDevicePath.data(),
            static_cast<DWORD>(mappedDevicePath.size()));
        if (mappedLength == 0 ||
            mappedLength == static_cast<DWORD>(mappedDevicePath.size())) {
            return HostExitCode::Win32ComFailure;
        }
        mappedDevicePath.resize(mappedLength);
        std::wstring mappedDosPath;
        const HostExitCode converted =
            DosPathFromMappedDevicePath(mappedDevicePath, &mappedDosPath);
        if (converted != HostExitCode::Pass) {
            return converted;
        }
        bool matches = false;
        const HostExitCode checked = CheckExplorerControllerFileIdentity(
            mappedDosPath, expected, &matches);
        if (checked != HostExitCode::Pass) {
            return checked;
        }
        if (matches) {
            *output = ModulePresence::Present;
            return HostExitCode::Pass;
        }
    } while (Module32NextW(snapshot.get(), &entry) != FALSE);
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        return HostExitCode::Win32ComFailure;
    }
    *output = ModulePresence::Absent;
    return HostExitCode::Pass;
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
    ExplorerControllerFileIdentityLock hook_identity_lock;
    std::unique_ptr<injection::ThreadHookInjector> injector;
    std::uint64_t detach_id = 0;
};

struct PaneMatch final {
    HWND pane = nullptr;
    std::uint32_t count = 0;
    bool failed = false;
};

HostExitCode EnumerateExactPanes(
    const HWND target,
    PaneMatch* const output) {
    if (target == nullptr || output == nullptr) {
        return HostExitCode::Win32ComFailure;
    }
    *output = {};
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = EnumChildWindows(
        target,
        [](const HWND child, const LPARAM value) {
            auto* const match = reinterpret_cast<PaneMatch*>(value);
            wchar_t name[64]{};
            if (GetClassNameW(child, name, 64) == 0) {
                match->failed = true;
                return FALSE;
            }
            if (std::wstring_view{name} == hook::kStatusPaneClassName) {
                match->pane = child;
                ++match->count;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(output));
    if (output->failed ||
        (enumerated == FALSE && GetLastError() != ERROR_SUCCESS)) {
        return HostExitCode::Win32ComFailure;
    }
    return HostExitCode::Pass;
}

void RetainContextForHostLifetime(const std::shared_ptr<void>& context) {
    if (!context) {
        return;
    }
    static auto* const retained =
        new std::vector<std::shared_ptr<void>>();
    retained->push_back(context);
}

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
        [state](const ExplorerControllerTarget& target,
                const std::wstring_view hookDllPath,
                const std::function<void()>& accepted,
                bool* const contextMayLive) {
            if (contextMayLive == nullptr || !accepted) {
                return HostExitCode::Win32ComFailure;
            }
            *contextMayLive = false;
            HostExitCode identityResult = AcquireExplorerControllerFileIdentityLock(
                hookDllPath,
                &state->hook_identity_lock);
            if (identityResult != HostExitCode::Pass) {
                return identityResult;
            }
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
                            state->pipe.get(), &clientPid) == FALSE) {
                        const DWORD error = GetLastError();
                        return Status{ErrorCode::PIPE_DISCONNECTED,
                                      HRESULT_FROM_WIN32(error), error};
                    }
                    if (clientPid != expectedPid) {
                        return Status{ErrorCode::TARGET_VALIDATION_FAILED,
                                      S_FALSE, ERROR_ACCESS_DENIED};
                    }
                    if (!WaitForPipeFrame(state->pipe.get(), timeoutMs)) {
                        return Status{ErrorCode::PIPE_DISCONNECTED,
                                      HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                                      ERROR_TIMEOUT};
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
            const auto resolveHookExport = platform.resolve_hook_export;
            platform.resolve_hook_export =
                [state, resolveHookExport](
                    const std::string_view name,
                    HMODULE* const module,
                    HOOKPROC* const procedure) {
                    const Status resolved =
                        resolveHookExport(name, module, procedure);
                    if (!resolved.ok() || module == nullptr || *module == nullptr) {
                        return resolved;
                    }
                    std::wstring modulePath(32768, L'\0');
                    const DWORD length = GetModuleFileNameW(
                        *module, modulePath.data(),
                        static_cast<DWORD>(modulePath.size()));
                    if (length == 0 ||
                        length == static_cast<DWORD>(modulePath.size())) {
                        const DWORD error = GetLastError();
                        return Status{ErrorCode::HOOK_INSTALL_FAILED,
                                      HRESULT_FROM_WIN32(error), error};
                    }
                    modulePath.resize(length);
                    ExplorerControllerFileIdentity loadedIdentity{};
                    if (CaptureExplorerControllerFileIdentity(
                            modulePath, &loadedIdentity) != HostExitCode::Pass ||
                        !(loadedIdentity == state->hook_identity_lock.identity)) {
                        return Status{ErrorCode::TARGET_VALIDATION_FAILED,
                                      S_FALSE, ERROR_FILE_INVALID};
                    }
                    return resolved;
                };
            platform.before_set_hook = [target, accepted] {
                if (!IsExactTargetWindow(target.top_level, target)) {
                    return Status{ErrorCode::TARGET_VALIDATION_FAILED,
                                  S_FALSE, ERROR_INVALID_WINDOW_HANDLE};
                }
                accepted();
                return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
            };
            state->injector =
                std::make_unique<injection::ThreadHookInjector>(std::move(platform));
            injection::HookAttachOutcome outcome{};
            status = state->injector->Attach(
                {target.process_id, target.thread_id, target.top_level}, &outcome);
            *contextMayLive = state->injector->retained_target_count() != 0;
            if (!status.ok() || !outcome.unload_authorized) {
                return StatusExit(status);
            }
            *contextMayLive = true;
            return HostExitCode::Pass;
        },
        [](const ExplorerControllerTarget& target, DWORD* const ownerPid) {
            PaneMatch match{};
            if (ownerPid == nullptr) {
                return HostExitCode::Win32ComFailure;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kBoundedWaitMs);
            do {
                const HostExitCode enumerated =
                    EnumerateExactPanes(target.top_level, &match);
                if (enumerated != HostExitCode::Pass) {
                    return enumerated;
                }
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
                : HostExitCode::Win32ComFailure;
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
        [](const ExplorerControllerTarget& target, bool* const absent) {
            if (absent == nullptr) {
                return HostExitCode::Win32ComFailure;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kBoundedWaitMs);
            do {
                PaneMatch match{};
                const HostExitCode enumerated =
                    EnumerateExactPanes(target.top_level, &match);
                if (enumerated != HostExitCode::Pass) {
                    return enumerated;
                }
                if (match.count == 0) {
                    *absent = true;
                    return HostExitCode::Pass;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < deadline);
            *absent = false;
            return HostExitCode::Pass;
        },
        [state](const DWORD processId, bool* const absent) {
            if (absent == nullptr || !state->hook_identity_lock.file) {
                return HostExitCode::Win32ComFailure;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kBoundedWaitMs);
            do {
                ModulePresence presence = ModulePresence::Error;
                const HostExitCode queried = QueryExactModulePresence(
                    processId, state->hook_identity_lock.identity, &presence);
                if (queried != HostExitCode::Pass) {
                    return queried;
                }
                if (presence == ModulePresence::Absent) {
                    *absent = true;
                    return HostExitCode::Pass;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < deadline);
            *absent = false;
            return HostExitCode::Pass;
        },
        [state](const DWORD processId) {
            if (!state->injector) {
                return HostExitCode::Win32ComFailure;
            }
            return state->injector->ConfirmTargetGone(processId).ok()
                ? HostExitCode::Pass
                : HostExitCode::Win32ComFailure;
        },
        state,
    };
}

}  // namespace

HostExitCode CaptureExplorerControllerFileIdentity(
    const std::wstring_view path,
    ExplorerControllerFileIdentity* const output) {
    UniqueHandle file;
    return OpenFileIdentity(
        path,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &file,
        output);
}

HostExitCode CheckExplorerControllerFileIdentity(
    const std::wstring_view path,
    const ExplorerControllerFileIdentity& expected,
    bool* const matches) {
    if (matches == nullptr) {
        return HostExitCode::Win32ComFailure;
    }
    ExplorerControllerFileIdentity candidate{};
    const HostExitCode captured =
        CaptureExplorerControllerFileIdentity(path, &candidate);
    if (captured != HostExitCode::Pass) {
        return captured;
    }
    *matches = candidate == expected;
    return HostExitCode::Pass;
}

HostExitCode AcquireExplorerControllerFileIdentityLock(
    const std::wstring_view path,
    ExplorerControllerFileIdentityLock* const output) {
    if (output == nullptr || output->file) {
        return HostExitCode::Win32ComFailure;
    }
    ExplorerControllerFileIdentityLock candidate{};
    const HostExitCode opened = OpenFileIdentity(
        path, GENERIC_READ, FILE_SHARE_READ,
        &candidate.file, &candidate.identity);
    if (opened != HostExitCode::Pass) {
        return opened;
    }
    *output = std::move(candidate);
    return HostExitCode::Pass;
}

HostExitCode SelectExplorerControllerDosPath(
    const std::wstring_view devicePath,
    const std::span<const ExplorerControllerDeviceMapping> mappings,
    std::wstring* const output) {
    if (devicePath.empty() || mappings.empty() || output == nullptr) {
        return HostExitCode::Win32ComFailure;
    }
    const ExplorerControllerDeviceMapping* selected = nullptr;
    bool ambiguous = false;
    for (const auto& mapping : mappings) {
        const std::wstring_view prefix{mapping.device_prefix};
        if (prefix.empty() || mapping.drive.empty() ||
            devicePath.size() < prefix.size() ||
            _wcsnicmp(devicePath.data(), prefix.data(), prefix.size()) != 0 ||
            (devicePath.size() != prefix.size() &&
             devicePath[prefix.size()] != L'\\')) {
            continue;
        }
        if (selected == nullptr ||
            mapping.device_prefix.size() > selected->device_prefix.size()) {
            selected = &mapping;
            ambiguous = false;
        } else if (
            mapping.device_prefix.size() == selected->device_prefix.size() &&
            _wcsicmp(mapping.drive.c_str(), selected->drive.c_str()) != 0) {
            ambiguous = true;
        }
    }
    if (selected == nullptr || ambiguous) {
        return HostExitCode::Win32ComFailure;
    }
    *output = selected->drive;
    output->append(devicePath.substr(selected->device_prefix.size()));
    return HostExitCode::Pass;
}

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
    bool contextMayLive = false;
    result = operations.attach(
        inspected,
        hookDllPath,
        [&operations, &inspected] {
            operations.emit_target_accepted(inspected);
        },
        &contextMayLive);
    if (result != HostExitCode::Pass) {
        if (contextMayLive) {
            RetainContextForHostLifetime(operations.retention_context);
        }
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
    bool paneAbsent = false;
    const HostExitCode paneCleanup =
        operations.pane_absent(inspected, &paneAbsent);
    bool moduleAbsent = false;
    const HostExitCode moduleCleanup =
        operations.exact_module_absent(inspected.process_id, &moduleAbsent);
    if (detached != HostExitCode::Pass ||
        paneCleanup != HostExitCode::Pass ||
        moduleCleanup != HostExitCode::Pass ||
        !paneAbsent || !moduleAbsent) {
        RetainContextForHostLifetime(operations.retention_context);
        return detached == HostExitCode::Win32ComFailure ||
            paneCleanup == HostExitCode::Win32ComFailure ||
            moduleCleanup == HostExitCode::Win32ComFailure
            ? HostExitCode::Win32ComFailure
            : HostExitCode::ContractFailure;
    }
    const HostExitCode released =
        operations.release_retained_target(inspected.process_id);
    if (released != HostExitCode::Pass) {
        RetainContextForHostLifetime(operations.retention_context);
        return released;
    }
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
