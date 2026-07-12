#include "hook_test_modes.h"

#include "common/win32_handle.h"
#include "injection/thread_hook_injector.h"
#include "ipc/named_pipe.h"
#include "ipc/protocol.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace winexinfo::tests {
namespace {

constexpr wchar_t kTargetClass[] = L"WinExInfo.GateBTarget.v1";

struct WindowMatch final {
    DWORD pid = 0;
    DWORD tid = 0;
    HWND hwnd = nullptr;
    std::uint32_t count = 0;
};

BOOL CALLBACK FindTargetWindow(const HWND window, const LPARAM value) {
    auto* match = reinterpret_cast<WindowMatch*>(value);
    wchar_t className[64]{};
    if (GetClassNameW(window, className, 64) == 0 ||
        std::wstring_view{className} != kTargetClass) {
        return TRUE;
    }
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(window, &pid);
    if (pid == match->pid) {
        ++match->count;
        match->tid = tid;
        match->hwnd = window;
    }
    return TRUE;
}

bool SameExecutableFile(const HANDLE process) {
    std::wstring targetPath(32768, L'\0');
    DWORD size = static_cast<DWORD>(targetPath.size());
    if (QueryFullProcessImageNameW(process, 0, targetPath.data(), &size) == FALSE) {
        return false;
    }
    targetPath.resize(size);
    wchar_t currentPath[32768]{};
    const DWORD currentSize = GetModuleFileNameW(nullptr, currentPath, 32768);
    if (currentSize == 0 || currentSize == 32768) {
        return false;
    }
    UniqueHandle targetFile{CreateFileW(
        targetPath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    UniqueHandle currentFile{CreateFileW(
        currentPath, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    BY_HANDLE_FILE_INFORMATION targetInfo{};
    BY_HANDLE_FILE_INFORMATION currentInfo{};
    return targetFile && currentFile &&
        GetFileInformationByHandle(targetFile.get(), &targetInfo) != FALSE &&
        GetFileInformationByHandle(currentFile.get(), &currentInfo) != FALSE &&
        targetInfo.dwVolumeSerialNumber == currentInfo.dwVolumeSerialNumber &&
        targetInfo.nFileIndexHigh == currentInfo.nFileIndexHigh &&
        targetInfo.nFileIndexLow == currentInfo.nFileIndexLow;
}

bool QueryTokenValue(
    const HANDLE token,
    const TOKEN_INFORMATION_CLASS informationClass,
    std::vector<std::uint8_t>* const output) {
    DWORD size = 0;
    if (GetTokenInformation(token, informationClass, nullptr, 0, &size) != FALSE ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return false;
    }
    output->resize(size);
    return GetTokenInformation(
               token, informationClass, output->data(), size, &size) != FALSE;
}

bool SamePrincipalAndIntegrity(const HANDLE targetProcess) {
    HANDLE rawCurrentToken = nullptr;
    HANDLE rawTargetToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawCurrentToken) == FALSE ||
        OpenProcessToken(targetProcess, TOKEN_QUERY, &rawTargetToken) == FALSE) {
        if (rawCurrentToken != nullptr) {
            CloseHandle(rawCurrentToken);
        }
        return false;
    }
    UniqueHandle currentToken{rawCurrentToken};
    UniqueHandle targetToken{rawTargetToken};
    std::vector<std::uint8_t> currentUser;
    std::vector<std::uint8_t> targetUser;
    std::vector<std::uint8_t> currentIntegrity;
    std::vector<std::uint8_t> targetIntegrity;
    if (!QueryTokenValue(currentToken.get(), TokenUser, &currentUser) ||
        !QueryTokenValue(targetToken.get(), TokenUser, &targetUser) ||
        !QueryTokenValue(
            currentToken.get(), TokenIntegrityLevel, &currentIntegrity) ||
        !QueryTokenValue(
            targetToken.get(), TokenIntegrityLevel, &targetIntegrity)) {
        return false;
    }
    const auto* currentUserValue =
        reinterpret_cast<const TOKEN_USER*>(currentUser.data());
    const auto* targetUserValue =
        reinterpret_cast<const TOKEN_USER*>(targetUser.data());
    const auto* currentLevel =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(currentIntegrity.data());
    const auto* targetLevel =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(targetIntegrity.data());
    const DWORD currentCount = *GetSidSubAuthorityCount(currentLevel->Label.Sid);
    const DWORD targetCount = *GetSidSubAuthorityCount(targetLevel->Label.Sid);
    if (currentCount == 0 || targetCount == 0) {
        return false;
    }
    return EqualSid(
               currentUserValue->User.Sid,
               targetUserValue->User.Sid) != FALSE &&
        *GetSidSubAuthority(currentLevel->Label.Sid, currentCount - 1) ==
        *GetSidSubAuthority(targetLevel->Label.Sid, targetCount - 1);
}

bool IsNativeAmd64(const HANDLE process) {
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    return IsWow64Process2(process, &processMachine, &nativeMachine) != FALSE &&
        processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
        nativeMachine == IMAGE_FILE_MACHINE_AMD64;
}

enum class FileIdentityComparison {
    Same,
    Different,
    Error,
};

FileIdentityComparison CompareFileIdentity(
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
        return FileIdentityComparison::Error;
    }
    BY_HANDLE_FILE_INFORMATION firstInfo{};
    BY_HANDLE_FILE_INFORMATION secondInfo{};
    if (GetFileInformationByHandle(first.get(), &firstInfo) == FALSE ||
        GetFileInformationByHandle(second.get(), &secondInfo) == FALSE) {
        return FileIdentityComparison::Error;
    }
    return firstInfo.dwVolumeSerialNumber == secondInfo.dwVolumeSerialNumber &&
        firstInfo.nFileIndexHigh == secondInfo.nFileIndexHigh &&
        firstInfo.nFileIndexLow == secondInfo.nFileIndexLow
        ? FileIdentityComparison::Same
        : FileIdentityComparison::Different;
}

enum class ModulePresence {
    Present,
    Absent,
    Error,
};

ModulePresence QueryModulePresence(
    const DWORD pid,
    const std::wstring_view expectedDllPath) {
    UniqueHandle snapshot{CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
    if (!snapshot) {
        return ModulePresence::Error;
    }
    MODULEENTRY32W entry{sizeof(MODULEENTRY32W)};
    if (Module32FirstW(snapshot.get(), &entry) == FALSE) {
        return GetLastError() == ERROR_NO_MORE_FILES
            ? ModulePresence::Absent
            : ModulePresence::Error;
    }
    do {
        const FileIdentityComparison comparison =
            CompareFileIdentity(entry.szExePath, expectedDllPath);
        if (comparison == FileIdentityComparison::Error) {
            return ModulePresence::Error;
        }
        if (comparison == FileIdentityComparison::Same) {
            return ModulePresence::Present;
        }
    } while (Module32NextW(snapshot.get(), &entry) != FALSE);
    return GetLastError() == ERROR_NO_MORE_FILES
        ? ModulePresence::Absent
        : ModulePresence::Error;
}

bool WaitForModuleState(
    const DWORD pid,
    const std::wstring_view expectedDllPath,
    const bool present) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    do {
        const ModulePresence presence = QueryModulePresence(pid, expectedDllPath);
        if ((present && presence == ModulePresence::Present) ||
            (!present && presence == ModulePresence::Absent)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool CaptureProcessCounts(
    const HANDLE process,
    const DWORD pid,
    DWORD* const handles,
    DWORD* const threads) {
    if (handles == nullptr || threads == nullptr ||
        GetProcessHandleCount(process, handles) == FALSE) {
        return false;
    }
    UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)};
    if (!snapshot) {
        return false;
    }
    DWORD count = 0;
    THREADENTRY32 entry{sizeof(THREADENTRY32)};
    if (Thread32First(snapshot.get(), &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                ++count;
            }
        } while (Thread32Next(snapshot.get(), &entry) != FALSE);
    }
    *threads = count;
    return true;
}

bool WaitForStableProcessCounts(
    const HANDLE targetProcess,
    const DWORD targetPid,
    DWORD* const targetHandles,
    DWORD* const targetThreads,
    DWORD* const controllerHandles,
    DWORD* const controllerThreads) {
    DWORD previousTargetHandles = 0;
    DWORD previousTargetThreads = 0;
    DWORD previousControllerHandles = 0;
    DWORD previousControllerThreads = 0;
    if (!CaptureProcessCounts(
            targetProcess, targetPid,
            &previousTargetHandles, &previousTargetThreads) ||
        !CaptureProcessCounts(
            GetCurrentProcess(), GetCurrentProcessId(),
            &previousControllerHandles, &previousControllerThreads)) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    std::uint32_t stableSamples = 0;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        DWORD currentTargetHandles = 0;
        DWORD currentTargetThreads = 0;
        DWORD currentControllerHandles = 0;
        DWORD currentControllerThreads = 0;
        if (!CaptureProcessCounts(
                targetProcess, targetPid,
                &currentTargetHandles, &currentTargetThreads) ||
            !CaptureProcessCounts(
                GetCurrentProcess(), GetCurrentProcessId(),
                &currentControllerHandles, &currentControllerThreads)) {
            return false;
        }
        if (currentTargetHandles == previousTargetHandles &&
            currentTargetThreads == previousTargetThreads &&
            currentControllerHandles == previousControllerHandles &&
            currentControllerThreads == previousControllerThreads) {
            ++stableSamples;
            if (stableSamples >= 5) {
                *targetHandles = currentTargetHandles;
                *targetThreads = currentTargetThreads;
                *controllerHandles = currentControllerHandles;
                *controllerThreads = currentControllerThreads;
                return true;
            }
        } else {
            stableSamples = 0;
        }
        previousTargetHandles = currentTargetHandles;
        previousTargetThreads = currentTargetThreads;
        previousControllerHandles = currentControllerHandles;
        previousControllerThreads = currentControllerThreads;
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

std::wstring HookDllPath() {
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
    if (size == 0 || size == 32768) {
        return {};
    }
    return (std::filesystem::path{std::wstring{executable, size}}.parent_path() /
            L"WinExInfoHook.dll").wstring();
}

}  // namespace

int RunHookControllerMode(const HookTestCommand& command) {
    UniqueHandle targetProcess{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, command.target_pid)};
    if (!targetProcess || !SameExecutableFile(targetProcess.get()) ||
        !SamePrincipalAndIntegrity(targetProcess.get()) ||
        !IsNativeAmd64(targetProcess.get())) {
        std::cerr << "TARGET_VALIDATION_FAILED\n";
        return 1;
    }
    DWORD targetSession = 0;
    DWORD currentSession = 0;
    if (ProcessIdToSessionId(command.target_pid, &targetSession) == FALSE ||
        ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) == FALSE ||
        targetSession != currentSession) {
        std::cerr << "TARGET_VALIDATION_FAILED\n";
        return 1;
    }
    WindowMatch match{command.target_pid};
    EnumWindows(FindTargetWindow, reinterpret_cast<LPARAM>(&match));
    const std::wstring readyName =
        L"Local\\WinExInfo.GateBTarget.Ready.v1." +
        std::to_wstring(command.target_pid);
    UniqueHandle ready{OpenEventW(SYNCHRONIZE, FALSE, readyName.c_str())};
    if (match.count != 1 || match.hwnd == nullptr || match.tid == 0 ||
        !ready || WaitForSingleObject(ready.get(), 0) != WAIT_OBJECT_0) {
        std::cerr << "TARGET_VALIDATION_FAILED\n";
        return 1;
    }
    std::ostringstream accepted;
    accepted << "TARGET_ACCEPTED protocol=1 pid=" << command.target_pid
             << " tid=" << match.tid << " hwnd=0x" << std::uppercase
             << std::hex << std::setw(16) << std::setfill('0')
             << reinterpret_cast<std::uintptr_t>(match.hwnd);
    std::cout << accepted.str() << '\n' << std::flush;

    std::uint64_t detachId = 0;
    const std::wstring hookDllPath = HookDllPath();
    if (hookDllPath.empty()) {
        return 3;
    }
    const std::uint32_t iterations =
        command.mode == HookTestMode::ControllerFault ? 1 : command.iterations;
    UniqueHandle* activePipe = nullptr;
    auto operations = injection::CreateProductionHookPlatformOperations(
        hookDllPath,
        [&activePipe, expectedTargetPid = command.target_pid](
            const std::uint64_t attachId,
            const DWORD,
            injection::HookAttachReceipt* const output) {
            if (activePipe == nullptr || !*activePipe) {
                return Status{ErrorCode::PIPE_DISCONNECTED, S_FALSE, ERROR_INVALID_HANDLE};
            }
            if (ConnectNamedPipe(activePipe->get(), nullptr) == FALSE &&
                GetLastError() != ERROR_PIPE_CONNECTED) {
                const DWORD error = GetLastError();
                return Status{ErrorCode::PIPE_DISCONNECTED,
                              HRESULT_FROM_WIN32(error), error};
            }
            ULONG clientPid = 0;
            if (GetNamedPipeClientProcessId(activePipe->get(), &clientPid) == FALSE ||
                clientPid != expectedTargetPid) {
                activePipe->reset();
                return Status{ErrorCode::TARGET_VALIDATION_FAILED,
                              S_FALSE, ERROR_ACCESS_DENIED};
            }
            ipc::DecodedFrame frame{};
            ipc::AttachResult result{};
            Status status = ipc::ReadFrame(activePipe, &frame);
            if (!status.ok() || frame.message_type != ipc::MessageType::AttachResult ||
                frame.request_id != attachId ||
                !ipc::DecodeAttachResult(frame, &result).ok()) {
                return Status{ErrorCode::IPC_PROTOCOL_ERROR, S_FALSE, ERROR_INVALID_DATA};
            }
            *output = {
                true, frame.request_id, result.explorer_pid,
                result.ui_thread_id,
                reinterpret_cast<HWND>(result.top_level_hwnd),
                result.result, result.error_code};
            return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
        });
    if (command.mode == HookTestMode::ControllerFault) {
        operations.unhook = [](HHOOK, bool* const output) {
            if (output != nullptr) {
                *output = false;
            }
            return Status{ErrorCode::HOOK_RELEASE_FAILED, S_FALSE, ERROR_INVALID_STATE};
        };
    }
    HMODULE controllerHookModule = nullptr;
    HOOKPROC controllerHookProcedure = nullptr;
    if (!operations.resolve_hook_export(
            injection::kHookExportName,
            &controllerHookModule,
            &controllerHookProcedure).ok() ||
        controllerHookModule == nullptr || controllerHookProcedure == nullptr) {
        return 3;
    }
    const HHOOK warmupHook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        controllerHookProcedure,
        controllerHookModule,
        GetCurrentThreadId());
    if (warmupHook == nullptr || UnhookWindowsHookEx(warmupHook) == FALSE) {
        return 3;
    }
    std::wstring warmupPipeName;
    UniqueHandle warmupPipe;
    if (!ipc::BuildCurrentUserPipeName(&warmupPipeName).ok() ||
        !ipc::CreateControllerPipeServer(warmupPipeName, &warmupPipe).ok()) {
        return 3;
    }
    warmupPipe.reset();
    injection::HookReleaseEvent warmupEvent{};
    const std::wstring warmupEventName =
        L"Local\\WinExInfo.ControllerWarmup." +
        std::to_wstring(GetCurrentProcessId());
    if (!operations.create_release_event(warmupEventName, &warmupEvent).ok() ||
        warmupEvent.handle == nullptr || warmupEvent.already_exists) {
        if (warmupEvent.handle != nullptr) {
            operations.close_event(warmupEvent.handle);
        }
        return 3;
    }
    operations.close_event(warmupEvent.handle);
    injection::ThreadHookInjector injector{std::move(operations)};
    DWORD targetHandlesStart = 0;
    DWORD targetThreadsStart = 0;
    DWORD controllerHandlesStart = 0;
    DWORD controllerThreadsStart = 0;
    DWORD targetHandlesEnd = 0;
    DWORD targetThreadsEnd = 0;
    DWORD controllerHandlesEnd = 0;
    DWORD controllerThreadsEnd = 0;
    if (!WaitForStableProcessCounts(
            targetProcess.get(), command.target_pid,
            &targetHandlesStart, &targetThreadsStart,
            &controllerHandlesStart, &controllerThreadsStart)) {
        return 3;
    }
    for (std::uint32_t cycle = 0; cycle < iterations; ++cycle) {
        if (FindWindowExW(
                match.hwnd, nullptr, L"WinExInfo.StatusPane", nullptr) != nullptr) {
            return 1;
        }
        DWORD targetHandlesBefore = 0;
        DWORD targetThreadsBefore = 0;
        DWORD controllerHandlesBefore = 0;
        DWORD controllerThreadsBefore = 0;
        if (!CaptureProcessCounts(
                targetProcess.get(), command.target_pid,
                &targetHandlesBefore, &targetThreadsBefore) ||
            !CaptureProcessCounts(
                GetCurrentProcess(), GetCurrentProcessId(),
                &controllerHandlesBefore, &controllerThreadsBefore)) {
            return 3;
        }
        if (cycle == 0 &&
            (targetHandlesBefore != targetHandlesStart ||
             targetThreadsBefore != targetThreadsStart ||
             controllerHandlesBefore != controllerHandlesStart ||
             controllerThreadsBefore != controllerThreadsStart)) {
            return 1;
        }
        std::wstring pipeName;
        UniqueHandle pipe;
        if (!ipc::BuildCurrentUserPipeNameForProcess(
                command.target_pid, &pipeName).ok() ||
            !ipc::CreateControllerPipeServer(pipeName, &pipe).ok()) {
            return 3;
        }
        activePipe = &pipe;
        injection::HookAttachOutcome outcome{};
        const Status attached = injector.Attach(
            {command.target_pid, match.tid, match.hwnd}, &outcome);
        if (command.mode == HookTestMode::ControllerFault) {
            const Status released = attached.ok()
                ? injector.ReleaseHookForDetach(command.target_pid)
                : attached;
            const bool retained = !released.ok() &&
                released.code == ErrorCode::HOOK_RELEASE_FAILED &&
                WaitForModuleState(command.target_pid, hookDllPath, true);
            UniqueHandle releaseEvent{
                OpenEventW(SYNCHRONIZE, FALSE, outcome.event_name.c_str())};
            const DWORD eventState = releaseEvent
                ? WaitForSingleObject(releaseEvent.get(), 0)
                : WAIT_FAILED;
            PostMessageW(match.hwnd, WM_CLOSE, 0, 0);
            const bool exited = WaitForSingleObject(targetProcess.get(), 5000) == WAIT_OBJECT_0;
            if (retained && eventState == WAIT_TIMEOUT && exited) {
                std::cout << "GATE_B_FAULT_PASS fault=unhook_failure module_retained=true event_signaled=false target_exit=0\n";
                return 0;
            }
            return 1;
        }
        if (!attached.ok() || !outcome.unload_authorized) {
            return 1;
        }
        if (!injector.ReleaseHookForDetach(command.target_pid).ok()) {
            return 1;
        }
        std::vector<std::uint8_t> detach;
        if (!ipc::EncodeDetachRequest(++detachId, &detach).ok() ||
            !ipc::WriteFrame(pipe.get(), detach).ok()) {
            return 3;
        }
        ipc::DecodedFrame response{};
        ipc::DetachResult result{};
        if (!ipc::ReadFrame(&pipe, &response).ok() ||
            !ipc::DecodeDetachResult(response, detachId, &result).ok() ||
            result.explorer_pid != command.target_pid || result.result != 0) {
            return 1;
        }
        pipe.reset();
        if (!WaitForModuleState(command.target_pid, hookDllPath, false) ||
            !injector.ConfirmTargetGone(command.target_pid).ok() ||
            WaitForSingleObject(targetProcess.get(), 0) != WAIT_TIMEOUT ||
            FindWindowExW(
                match.hwnd, nullptr, L"WinExInfo.StatusPane", nullptr) != nullptr) {
            return 1;
        }
        DWORD targetHandlesAfter = 0;
        DWORD targetThreadsAfter = 0;
        DWORD controllerHandlesAfter = 0;
        DWORD controllerThreadsAfter = 0;
        if (!CaptureProcessCounts(
                targetProcess.get(), command.target_pid,
                &targetHandlesAfter, &targetThreadsAfter) ||
            !CaptureProcessCounts(
                GetCurrentProcess(), GetCurrentProcessId(),
                &controllerHandlesAfter, &controllerThreadsAfter) ||
            targetHandlesAfter != targetHandlesBefore ||
            targetThreadsAfter != targetThreadsBefore ||
            controllerHandlesAfter != controllerHandlesBefore ||
            controllerThreadsAfter != controllerThreadsBefore) {
            std::cerr << "RESOURCE_DELTA cycle=" << (cycle + 1)
                      << " target_handles="
                      << static_cast<long long>(targetHandlesAfter) - targetHandlesBefore
                      << " target_threads="
                      << static_cast<long long>(targetThreadsAfter) - targetThreadsBefore
                      << " controller_handles="
                      << static_cast<long long>(controllerHandlesAfter) - controllerHandlesBefore
                      << " controller_threads="
                      << static_cast<long long>(controllerThreadsAfter) - controllerThreadsBefore
                      << '\n';
            return 1;
        }
        targetHandlesEnd = targetHandlesAfter;
        targetThreadsEnd = targetThreadsAfter;
        controllerHandlesEnd = controllerHandlesAfter;
        controllerThreadsEnd = controllerThreadsAfter;
    }
    if (targetHandlesEnd != targetHandlesStart ||
        targetThreadsEnd != targetThreadsStart ||
        controllerHandlesEnd != controllerHandlesStart ||
        controllerThreadsEnd != controllerThreadsStart) {
        return 1;
    }
    PostMessageW(match.hwnd, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(targetProcess.get(), 5000) != WAIT_OBJECT_0) {
        return 1;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(targetProcess.get(), &exitCode);
    if (exitCode != 0) {
        return 1;
    }
    std::cout << "RESOURCE_COUNTS target_handles_start=" << targetHandlesStart
              << " target_handles_end=" << targetHandlesEnd
              << " target_threads_start=" << targetThreadsStart
              << " target_threads_end=" << targetThreadsEnd
              << " controller_handles_start=" << controllerHandlesStart
              << " controller_handles_end=" << controllerHandlesEnd
              << " controller_threads_start=" << controllerThreadsStart
              << " controller_threads_end=" << controllerThreadsEnd << '\n';
    std::cout << "GATE_B_PASS iterations=" << iterations
              << " target_handles_delta=0 target_threads_delta=0"
              << " controller_handles_delta=0 controller_threads_delta=0 target_exit=0\n";
    return 0;
}

}  // namespace winexinfo::tests
