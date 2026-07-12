#include "injection/thread_hook_injector.h"

#include <Windows.h>

#include <limits>
#include <new>
#include <utility>

namespace winexinfo::injection {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status Failure(const ErrorCode code) noexcept {
    return {code, S_FALSE, ERROR_SUCCESS};
}

bool ExactSuccess(const Status& status) noexcept {
    return status.code == ErrorCode::OK && status.hresult == S_OK &&
        status.win32 == ERROR_SUCCESS;
}

bool ReceiptMatches(
    const HookAttachReceipt& receipt,
    const std::uint64_t attachId,
    const HookTarget& target) {
    const bool resultShape =
        (receipt.result == 0) == receipt.error_code.empty();
    return receipt.available && receipt.request_id == attachId &&
        receipt.explorer_pid == target.explorer_pid &&
        receipt.ui_thread_id == target.ui_thread_id &&
        receipt.top_level_hwnd == target.top_level_hwnd && resultShape &&
        receipt.result == 0;
}

}  // namespace

ThreadHookInjector::ThreadHookInjector(
    HookPlatformOperations operations,
    const std::uint64_t lastAttachId)
    : operations_(std::move(operations)), last_attach_id_(lastAttachId) {}

ThreadHookInjector::~ThreadHookInjector() {
    if (!operations_.close_event) {
        return;
    }
    for (const auto& [pid, target] : retained_targets_) {
        static_cast<void>(pid);
        operations_.close_event(target.release_event);
    }
}

Status ThreadHookInjector::Attach(
    const HookTarget& target,
    HookAttachOutcome* const output) {
    if (output == nullptr || target.explorer_pid == 0 ||
        target.ui_thread_id == 0 || target.top_level_hwnd == nullptr ||
        !operations_.create_release_event || !operations_.register_message ||
        !operations_.resolve_hook_export || !operations_.set_hook ||
        !operations_.send_message_timeout || !operations_.wait_attach_result ||
        !operations_.unhook || !operations_.set_event ||
        !operations_.close_event) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    *output = {};
    if (retained_targets_.contains(target.explorer_pid) ||
        last_attach_id_ >= static_cast<std::uint64_t>(INT64_MAX)) {
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    const std::uint64_t attachId = ++last_attach_id_;
    output->attach_id = attachId;
    try {
        output->event_name =
            L"Local\\WinExInfo.HookReleased." +
            std::to_wstring(target.explorer_pid) + L"." +
            std::to_wstring(target.ui_thread_id) + L"." +
            std::to_wstring(attachId);
    } catch (const std::bad_alloc&) {
        return {ErrorCode::HOOK_INSTALL_FAILED, E_OUTOFMEMORY, ERROR_SUCCESS};
    }

    HookReleaseEvent releaseEvent{};
    const Status created = operations_.create_release_event(
        output->event_name, &releaseEvent);
    if (!ExactSuccess(created) || releaseEvent.handle == nullptr ||
        releaseEvent.handle == INVALID_HANDLE_VALUE ||
        releaseEvent.already_exists || !releaseEvent.manual_reset ||
        releaseEvent.initially_signaled || !releaseEvent.current_user_only ||
        releaseEvent.unexpected_principal_present) {
        if (releaseEvent.handle != nullptr &&
            releaseEvent.handle != INVALID_HANDLE_VALUE) {
            operations_.close_event(releaseEvent.handle);
        }
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    const auto closeReleaseEvent = [&]() {
        operations_.close_event(releaseEvent.handle);
        releaseEvent.handle = nullptr;
    };

    UINT attachMessage = 0;
    const Status registered = operations_.register_message(
        kAttachMessageName, &attachMessage);
    if (!ExactSuccess(registered) || attachMessage == 0) {
        closeReleaseEvent();
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    HMODULE module = nullptr;
    HOOKPROC procedure = nullptr;
    const Status resolved = operations_.resolve_hook_export(
        kHookExportName, &module, &procedure);
    if (!ExactSuccess(resolved) || module == nullptr || procedure == nullptr) {
        closeReleaseEvent();
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    if (operations_.before_set_hook) {
        const Status validated = operations_.before_set_hook();
        if (!ExactSuccess(validated)) {
            closeReleaseEvent();
            return validated;
        }
    }
    HHOOK hook = nullptr;
    const Status installed = operations_.set_hook(
        WH_CALLWNDPROC, procedure, module, target.ui_thread_id, &hook);
    if (!ExactSuccess(installed) || hook == nullptr) {
        closeReleaseEvent();
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }

    Status primary = Success();
    const Status triggered = operations_.send_message_timeout(
        target.top_level_hwnd,
        attachMessage,
        kAttachMagic,
        static_cast<LPARAM>(attachId),
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
        1000);
    if (!ExactSuccess(triggered)) {
        primary = Failure(ErrorCode::HOOK_TRIGGER_FAILED);
    } else {
        HookAttachReceipt receipt{};
        const Status waited = operations_.wait_attach_result(
            attachId, 5000, &receipt);
        if (!ExactSuccess(waited)) {
            primary = waited;
        } else if (!ReceiptMatches(receipt, attachId, target)) {
            primary = Failure(ErrorCode::WINDOW_ATTACH_FAILED);
        }
    }
    if (!primary.ok()) {
        output->original_status = primary;
    }

    bool unhooked = false;
    const Status released = operations_.unhook(hook, &unhooked);
    if (!ExactSuccess(released) || !unhooked) {
        retained_targets_.emplace(
            target.explorer_pid,
            RetainedTarget{releaseEvent.handle});
        releaseEvent.handle = nullptr;
        return Failure(ErrorCode::HOOK_RELEASE_FAILED);
    }
    output->hook_released = true;

    bool signaled = false;
    const Status eventSet = operations_.set_event(releaseEvent.handle, &signaled);
    if (!ExactSuccess(eventSet) || !signaled) {
        retained_targets_.emplace(
            target.explorer_pid,
            RetainedTarget{releaseEvent.handle});
        releaseEvent.handle = nullptr;
        return Failure(ErrorCode::HOOK_RELEASE_FAILED);
    }
    output->release_event_signaled = true;

    if (primary.code == ErrorCode::HOOK_TRIGGER_FAILED) {
        closeReleaseEvent();
        return primary;
    }
    retained_targets_.emplace(
        target.explorer_pid,
        RetainedTarget{releaseEvent.handle});
    releaseEvent.handle = nullptr;
    output->unload_authorized = primary.ok();
    return primary;
}

Status ThreadHookInjector::ConfirmTargetGone(const DWORD explorerPid) noexcept {
    const auto found = retained_targets_.find(explorerPid);
    if (explorerPid == 0 || found == retained_targets_.end() ||
        !operations_.close_event) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    operations_.close_event(found->second.release_event);
    retained_targets_.erase(found);
    return Success();
}

std::size_t ThreadHookInjector::retained_target_count() const noexcept {
    return retained_targets_.size();
}

}  // namespace winexinfo::injection
