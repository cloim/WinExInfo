#include "injection/hook_platform.h"

#include "common/win32_handle.h"

#include <Aclapi.h>

#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace winexinfo::injection {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status Failure(const ErrorCode code, const DWORD error) noexcept {
    return {code, HRESULT_FROM_WIN32(error), error};
}

Status CurrentUserSid(std::vector<std::uint8_t>* const output) {
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE) {
        return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
    }
    UniqueHandle token{rawToken};
    DWORD size = 0;
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size) != FALSE ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
    }
    try {
        std::vector<std::uint8_t> tokenBuffer(size);
        if (GetTokenInformation(
                token.get(), TokenUser, tokenBuffer.data(), size, &size) == FALSE) {
            return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
        }
        const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
        const DWORD sidLength = GetLengthSid(user->User.Sid);
        std::vector<std::uint8_t> sid(sidLength);
        if (sidLength == 0 || CopySid(sidLength, sid.data(), user->User.Sid) == FALSE) {
            return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
        }
        *output = std::move(sid);
        return Success();
    } catch (const std::bad_alloc&) {
        return {ErrorCode::HOOK_INSTALL_FAILED, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

struct ProductionState final {
    std::wstring dll_path;
    HMODULE module = nullptr;

    ~ProductionState() {
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
};

}  // namespace

Status InspectHookObjectSecurity(
    const HANDLE object,
    bool* const currentUserPresent,
    bool* const unexpectedPrincipalPresent) {
    if (object == nullptr || object == INVALID_HANDLE_VALUE ||
        currentUserPresent == nullptr || unexpectedPrincipalPresent == nullptr) {
        return {
            ErrorCode::INVALID_ARGUMENT,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }
    *currentUserPresent = false;
    *unexpectedPrincipalPresent = false;
    std::vector<std::uint8_t> currentSid;
    Status status = CurrentUserSid(&currentSid);
    if (!status.ok()) {
        return status;
    }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityStatus = GetSecurityInfo(
        object,
        SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (securityStatus != ERROR_SUCCESS || dacl == nullptr ||
        descriptor == nullptr) {
        LocalFree(descriptor);
        return Failure(ErrorCode::HOOK_INSTALL_FAILED, securityStatus);
    }
    ACL_SIZE_INFORMATION information{};
    if (GetAclInformation(
            dacl,
            &information,
            sizeof(information),
            AclSizeInformation) == FALSE) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        return Failure(ErrorCode::HOOK_INSTALL_FAILED, error);
    }
    for (DWORD index = 0; index < information.AceCount; ++index) {
        void* rawAce = nullptr;
        if (GetAce(dacl, index, &rawAce) == FALSE || rawAce == nullptr) {
            const DWORD error = GetLastError();
            LocalFree(descriptor);
            return Failure(ErrorCode::HOOK_INSTALL_FAILED, error);
        }
        const auto* header = static_cast<const ACE_HEADER*>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            *unexpectedPrincipalPresent = true;
            continue;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        PSID aceSid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(aceSid, currentSid.data()) != FALSE) {
            *currentUserPresent = true;
        } else {
            *unexpectedPrincipalPresent = true;
        }
    }
    LocalFree(descriptor);
    return Success();
}

HookPlatformOperations CreateProductionHookPlatformOperations(
    std::wstring hookDllPath,
    HookAttachWaitOperation waitAttachResult) {
    auto state = std::make_shared<ProductionState>();
    state->dll_path = std::move(hookDllPath);
    return {
        [](const std::wstring_view name, HookReleaseEvent* const output) {
            if (output == nullptr || name.empty() ||
                name.find(L'\0') != std::wstring_view::npos) {
                return Status{
                    ErrorCode::INVALID_ARGUMENT,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                };
            }
            std::vector<std::uint8_t> sid;
            Status status = CurrentUserSid(&sid);
            if (!status.ok()) {
                return status;
            }
            EXPLICIT_ACCESSW access{};
            access.grfAccessPermissions = EVENT_ALL_ACCESS;
            access.grfAccessMode = SET_ACCESS;
            access.grfInheritance = NO_INHERITANCE;
            access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            access.Trustee.TrusteeType = TRUSTEE_IS_USER;
            access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
            PACL acl = nullptr;
            const DWORD aclStatus = SetEntriesInAclW(1, &access, nullptr, &acl);
            if (aclStatus != ERROR_SUCCESS || acl == nullptr) {
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, aclStatus);
            }
            SECURITY_DESCRIPTOR descriptor{};
            if (InitializeSecurityDescriptor(
                    &descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
                SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) == FALSE) {
                const DWORD error = GetLastError();
                LocalFree(acl);
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, error);
            }
            SECURITY_ATTRIBUTES attributes{
                sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
            const std::wstring eventName{name};
            SetLastError(ERROR_SUCCESS);
            const HANDLE event = CreateEventW(
                &attributes, TRUE, FALSE, eventName.c_str());
            const DWORD error = GetLastError();
            LocalFree(acl);
            if (event == nullptr) {
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, error);
            }
            bool currentUserPresent = false;
            bool unexpectedPrincipalPresent = false;
            const Status inspected = InspectHookObjectSecurity(
                event,
                &currentUserPresent,
                &unexpectedPrincipalPresent);
            if (!inspected.ok()) {
                CloseHandle(event);
                return inspected;
            }
            *output = {
                event,
                error == ERROR_ALREADY_EXISTS,
                true,
                false,
                currentUserPresent,
                unexpectedPrincipalPresent,
            };
            return Success();
        },
        [](const std::wstring_view name, UINT* const output) {
            if (output == nullptr || name != L"WinExInfo.Attach.v1") {
                return Status{
                    ErrorCode::INVALID_ARGUMENT,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                };
            }
            const std::wstring messageName{name};
            const UINT message = RegisterWindowMessageW(messageName.c_str());
            if (message == 0) {
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
            }
            *output = message;
            return Success();
        },
        [state](
            const std::string_view name,
            HMODULE* const module,
            HOOKPROC* const procedure) {
            if (module == nullptr || procedure == nullptr ||
                name != "WinExInfoCallWndProc" || state->dll_path.empty()) {
                return Status{
                    ErrorCode::INVALID_ARGUMENT,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                };
            }
            if (state->module == nullptr) {
                state->module = LoadLibraryW(state->dll_path.c_str());
            }
            if (state->module == nullptr) {
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
            }
            const FARPROC raw = GetProcAddress(state->module, "WinExInfoCallWndProc");
            if (raw == nullptr) {
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
            }
            *module = state->module;
            *procedure = reinterpret_cast<HOOKPROC>(raw);
            return Success();
        },
        [](const int hookType,
           const HOOKPROC procedure,
           const HMODULE module,
           const DWORD threadId,
           HHOOK* const output) {
            if (hookType != WH_CALLWNDPROC || procedure == nullptr ||
                module == nullptr || threadId == 0 || output == nullptr) {
                return Status{
                    ErrorCode::INVALID_ARGUMENT,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                };
            }
            const HHOOK hook = SetWindowsHookExW(
                hookType, procedure, module, threadId);
            if (hook == nullptr) {
                return Failure(ErrorCode::HOOK_INSTALL_FAILED, GetLastError());
            }
            *output = hook;
            return Success();
        },
        [](const HWND window,
           const UINT message,
           const WPARAM wparam,
           const LPARAM lparam,
           const UINT flags,
           const UINT timeout) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(
                    window,
                    message,
                    wparam,
                    lparam,
                    flags,
                    timeout,
                    &result) == 0) {
                return Failure(ErrorCode::HOOK_TRIGGER_FAILED, GetLastError());
            }
            return Success();
        },
        std::move(waitAttachResult),
        [](const HHOOK hook, bool* const output) {
            if (hook == nullptr || output == nullptr) {
                return Status{
                    ErrorCode::INVALID_ARGUMENT,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                };
            }
            const BOOL unhooked = UnhookWindowsHookEx(hook);
            *output = unhooked != FALSE;
            return unhooked != FALSE
                ? Success()
                : Failure(ErrorCode::HOOK_RELEASE_FAILED, GetLastError());
        },
        [](const HANDLE event, bool* const output) {
            if (event == nullptr || event == INVALID_HANDLE_VALUE || output == nullptr) {
                return Status{
                    ErrorCode::INVALID_ARGUMENT,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                };
            }
            const BOOL signaled = SetEvent(event);
            *output = signaled != FALSE;
            return signaled != FALSE
                ? Success()
                : Failure(ErrorCode::HOOK_RELEASE_FAILED, GetLastError());
        },
        [](const HANDLE event) {
            if (event != nullptr && event != INVALID_HANDLE_VALUE) {
                CloseHandle(event);
            }
        },
    };
}

}  // namespace winexinfo::injection
