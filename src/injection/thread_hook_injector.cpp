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

Status ExactValidationFailure(const Status& status) noexcept {
    return status.ok()
        ? Failure(ErrorCode::TARGET_VALIDATION_FAILED)
        : status;
}

bool AllHooksGone(const auto& target) noexcept {
    for (std::size_t index = 0; index < target.lease_count; ++index) {
        if (target.leases[index].hook != nullptr) {
            return false;
        }
    }
    return true;
}

}  // namespace

ThreadHookInjector::ThreadHookInjector(
    HookPlatformOperations operations,
    const std::uint64_t lastAttachId,
    std::function<void()> beforeRetainedTargetReservation)
    : operations_(std::move(operations)),
      last_attach_id_(lastAttachId),
      before_retained_target_reservation_(
          std::move(beforeRetainedTargetReservation)) {}

ThreadHookInjector::~ThreadHookInjector() {
    if (!operations_.close_event) {
        return;
    }
    for (const auto& [pid, target] : retained_targets_) {
        static_cast<void>(pid);
        if (target.release_event != nullptr) {
            operations_.close_event(target.release_event);
        }
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

    std::map<DWORD, RetainedTarget>::iterator retainedPosition;
    try {
        if (before_retained_target_reservation_) {
            before_retained_target_reservation_();
        }
        const auto [position, inserted] =
            retained_targets_.try_emplace(target.explorer_pid);
        if (!inserted) {
            return Failure(ErrorCode::HOOK_INSTALL_FAILED);
        }
        retainedPosition = position;
    } catch (const std::bad_alloc&) {
        return {ErrorCode::HOOK_INSTALL_FAILED, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
    RetainedTarget& retained = retainedPosition->second;
    const auto eraseCleanReservation = [&]() {
        if (retained.release_event != nullptr) {
            operations_.close_event(retained.release_event);
            retained.release_event = nullptr;
        }
        retained_targets_.erase(retainedPosition);
    };

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
        retained_targets_.erase(retainedPosition);
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    retained.release_event = releaseEvent.handle;
    releaseEvent.handle = nullptr;

    UINT attachMessage = 0;
    const Status registered = operations_.register_message(
        kAttachMessageName, &attachMessage);
    if (!ExactSuccess(registered) || attachMessage == 0) {
        eraseCleanReservation();
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    HMODULE module = nullptr;
    HOOKPROC procedure = nullptr;
    const Status resolved = operations_.resolve_hook_export(
        kHookExportName, &module, &procedure);
    if (!ExactSuccess(resolved) || module == nullptr || procedure == nullptr) {
        eraseCleanReservation();
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    if (operations_.before_set_hook) {
        const Status validated = operations_.before_set_hook();
        if (!ExactSuccess(validated)) {
            eraseCleanReservation();
            return ExactValidationFailure(validated);
        }
    }
    HHOOK hook = nullptr;
    const Status installed = operations_.set_hook(
        WH_CALLWNDPROC, procedure, module, target.ui_thread_id, &hook);
    if (!ExactSuccess(installed) || hook == nullptr) {
        if (hook != nullptr) {
            retained.leases[0] = {target.ui_thread_id, hook};
            retained.lease_count = 1;
            retained.release_started = true;
            output->original_status = installed;
            return installed.ok()
                ? Failure(ErrorCode::HOOK_INSTALL_FAILED)
                : installed;
        }
        eraseCleanReservation();
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    retained.leases[0] = {target.ui_thread_id, hook};
    retained.lease_count = 1;

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
            output->original_status = waited;
            primary = waited.ok()
                ? Failure(ErrorCode::WINDOW_ATTACH_FAILED)
                : waited;
        } else if (!ReceiptMatches(receipt, attachId, target)) {
            primary = Failure(ErrorCode::WINDOW_ATTACH_FAILED);
        }
    }
    if (!ExactSuccess(primary) && !output->original_status.has_value()) {
        output->original_status = primary;
    }

    if (ExactSuccess(primary)) {
        output->unload_authorized = true;
        return primary;
    }

    bool unhooked = false;
    const Status released = operations_.unhook(hook, &unhooked);
    if (!ExactSuccess(released) || !unhooked) {
        retained.release_started = true;
        return Failure(ErrorCode::HOOK_RELEASE_FAILED);
    }
    retained.leases[0].hook = nullptr;
    output->hook_released = true;

    bool signaled = false;
    const Status eventSet = operations_.set_event(retained.release_event, &signaled);
    if (!ExactSuccess(eventSet) || !signaled) {
        retained.release_started = true;
        return eventSet.ok()
            ? Failure(ErrorCode::HOOK_RELEASE_FAILED)
            : eventSet;
    }
    retained.release_event_signaled = true;
    retained.release_started = true;
    output->release_event_signaled = true;

    if (primary.code == ErrorCode::HOOK_TRIGGER_FAILED) {
        eraseCleanReservation();
        return primary;
    }
    output->unload_authorized = ExactSuccess(primary);
    return primary;
}

Status ThreadHookInjector::EnsureThreadHookLease(
    const HookTarget& target,
    const std::function<Status()>& finalValidate) {
    if (target.explorer_pid == 0 || target.ui_thread_id == 0 ||
        target.top_level_hwnd == nullptr || !finalValidate ||
        !operations_.resolve_hook_export || !operations_.set_hook) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    const auto found = retained_targets_.find(target.explorer_pid);
    if (found == retained_targets_.end() || found->second.release_started) {
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    RetainedTarget& retained = found->second;
    for (std::size_t index = 0; index < retained.lease_count; ++index) {
        if (retained.leases[index].ui_thread_id == target.ui_thread_id) {
            const Status validated = finalValidate();
            return ExactSuccess(validated)
                ? Success()
                : ExactValidationFailure(validated);
        }
    }
    if (retained.lease_count >= kMaximumThreadHookLeases) {
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }

    HMODULE module = nullptr;
    HOOKPROC procedure = nullptr;
    const Status resolved = operations_.resolve_hook_export(
        kHookExportName, &module, &procedure);
    if (!ExactSuccess(resolved) || module == nullptr || procedure == nullptr) {
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    const Status validated = finalValidate();
    if (!ExactSuccess(validated)) {
        return ExactValidationFailure(validated);
    }
    HHOOK hook = nullptr;
    const Status installed = operations_.set_hook(
        WH_CALLWNDPROC, procedure, module, target.ui_thread_id, &hook);
    if (!ExactSuccess(installed) || hook == nullptr) {
        if (hook != nullptr) {
            retained.leases[retained.lease_count++] = {
                target.ui_thread_id, hook};
            retained.release_started = true;
            return installed.ok()
                ? Failure(ErrorCode::HOOK_INSTALL_FAILED)
                : installed;
        }
        return Failure(ErrorCode::HOOK_INSTALL_FAILED);
    }
    retained.leases[retained.lease_count++] = {target.ui_thread_id, hook};
    return Success();
}

Status ThreadHookInjector::ReleaseHookForDetach(
    const DWORD explorerPid) noexcept {
    const auto found = retained_targets_.find(explorerPid);
    if (explorerPid == 0 || found == retained_targets_.end() ||
        !operations_.unhook || !operations_.set_event) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    RetainedTarget& target = found->second;
    target.release_started = true;
    Status firstError = Success();
    bool hasFirstError = false;
    for (std::size_t index = target.lease_count; index > 0; --index) {
        ThreadHookLease& lease = target.leases[index - 1];
        if (lease.hook == nullptr) {
            continue;
        }
        bool unhooked = false;
        const Status released = operations_.unhook(lease.hook, &unhooked);
        if (!ExactSuccess(released) || !unhooked) {
            if (!hasFirstError) {
                firstError = released.ok()
                    ? Failure(ErrorCode::HOOK_RELEASE_FAILED)
                    : released;
                hasFirstError = true;
            }
            continue;
        }
        lease.hook = nullptr;
    }
    if (hasFirstError || !AllHooksGone(target)) {
        return hasFirstError
            ? firstError
            : Failure(ErrorCode::HOOK_RELEASE_FAILED);
    }
    if (!target.release_event_signaled) {
        bool signaled = false;
        const Status eventSet = operations_.set_event(target.release_event, &signaled);
        if (!ExactSuccess(eventSet) || !signaled) {
            return eventSet.ok()
                ? Failure(ErrorCode::HOOK_RELEASE_FAILED)
                : eventSet;
        }
        target.release_event_signaled = true;
    }
    return Success();
}

Status ThreadHookInjector::ConfirmTargetGone(const DWORD explorerPid) noexcept {
    const auto found = retained_targets_.find(explorerPid);
    if (explorerPid == 0 || found == retained_targets_.end() ||
        !operations_.close_event) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    if (!AllHooksGone(found->second) ||
        !found->second.release_event_signaled) {
        return Failure(ErrorCode::HOOK_RELEASE_FAILED);
    }
    operations_.close_event(found->second.release_event);
    retained_targets_.erase(found);
    return Success();
}

std::size_t ThreadHookInjector::retained_target_count() const noexcept {
    return retained_targets_.size();
}

}  // namespace winexinfo::injection
