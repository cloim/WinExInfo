#include "hook/tab_subclass_set.h"

#include "hook/runtime.h"

#include <CommCtrl.h>

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace winexinfo::hook {

struct InstalledTab final {
    HWND window = nullptr;
    std::uint64_t generation = 0;

    bool operator==(const InstalledTab&) const = default;
};

class TabSubclassSet::Impl final {
public:
    HWND top_level = nullptr;
    std::uint64_t top_level_generation = 0;
    std::vector<InstalledTab> authoritative;
    std::vector<InstalledTab> installed;
    std::vector<InstalledTab> known_generations;
    bool lifetime_failed = false;
    bool cleanup_blocked = false;
};

namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status Failure(const DWORD error = ERROR_INVALID_STATE) noexcept {
    return {
        ErrorCode::WINDOW_ATTACH_FAILED,
        HRESULT_FROM_WIN32(error),
        error,
    };
}

void SetResult(
    ipc::TabSetResult* const result,
    const std::uint64_t generation,
    const Status status) {
    if (result == nullptr) {
        return;
    }
    if (status.ok()) {
        *result = {generation, 0, {}};
        return;
    }
    *result = {
        generation,
        static_cast<std::uint32_t>(status.code),
        std::string{ToString(status.code)},
    };
}

bool HasOperations(const TabSubclassOperations& operations) {
    return operations.get_current_thread_id &&
        operations.get_window_thread_process_id && operations.get_class_name &&
        operations.get_parent && operations.get_first_child &&
        operations.get_next_sibling && operations.install_subclass &&
        operations.remove_subclass && operations.post_refresh;
}

bool ExactWindow(
    const HWND window,
    const DWORD expectedThread,
    const std::wstring_view expectedClass,
    const TabSubclassOperations& operations) {
    DWORD process = 0;
    if (window == nullptr ||
        operations.get_window_thread_process_id(window, &process) != expectedThread ||
        process == 0) {
        return false;
    }
    std::wstring className;
    return operations.get_class_name(window, &className).ok() &&
        className == expectedClass;
}

const InstalledTab* Find(
    const std::vector<InstalledTab>& tabs,
    const HWND window) {
    const auto found = std::find_if(
        tabs.begin(), tabs.end(),
        [window](const InstalledTab& tab) { return tab.window == window; });
    return found == tabs.end() ? nullptr : &*found;
}

void TrackPossibleCallback(auto& state, const InstalledTab& tab) {
    const auto found = std::find_if(
        state.installed.begin(), state.installed.end(),
        [&](const InstalledTab& candidate) { return candidate.window == tab.window; });
    if (found == state.installed.end()) {
        state.installed.push_back(tab);
    } else {
        *found = tab;
    }
}

void ForgetCallback(auto& state, const HWND window) {
    const auto found = std::find_if(
        state.installed.begin(), state.installed.end(),
        [&](const InstalledTab& candidate) { return candidate.window == window; });
    if (found != state.installed.end()) {
        state.installed.erase(found);
    }
}

Status ValidateUpdate(
    const HWND topLevel,
    const ipc::TabSetUpdate& update,
    const TabSubclassOperations& operations,
    std::vector<InstalledTab>* const desired) {
    if (topLevel == nullptr || desired == nullptr || !HasOperations(operations) ||
        update.top_level_hwnd != reinterpret_cast<std::uint64_t>(topLevel) ||
        update.top_level_generation == 0 || update.tabs.empty() ||
        update.tabs.size() > ipc::kMaximumTabDescriptors) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    const DWORD thread = operations.get_current_thread_id();
    if (thread == 0 || !ExactWindow(topLevel, thread, L"CabinetWClass", operations)) {
        return Failure(ERROR_INVALID_WINDOW_HANDLE);
    }

    std::vector<HWND> liveTabs;
    std::unordered_set<HWND> visited;
    std::size_t directChildCount = 0;
    for (HWND child = operations.get_first_child(topLevel);
         child != nullptr;
         child = operations.get_next_sibling(child)) {
        if (++directChildCount > kMaximumTabDirectChildren ||
            !visited.insert(child).second) {
            return Failure(ERROR_INVALID_DATA);
        }
        std::wstring childClass;
        if (!operations.get_class_name(child, &childClass).ok()) {
            return Failure(ERROR_INVALID_WINDOW_HANDLE);
        }
        if (childClass == L"ShellTabWindowClass") {
            if (operations.get_parent(child) != topLevel ||
                !ExactWindow(child, thread, L"ShellTabWindowClass", operations)) {
                return Failure(ERROR_INVALID_WINDOW_HANDLE);
            }
            liveTabs.push_back(child);
            if (liveTabs.size() > ipc::kMaximumTabDescriptors) {
                return Failure(ERROR_INVALID_DATA);
            }
        }
    }
    if (liveTabs.size() != update.tabs.size()) {
        return Failure(ERROR_INVALID_WINDOW_HANDLE);
    }
    desired->clear();
    desired->reserve(update.tabs.size());
    for (std::size_t index = 0; index < update.tabs.size(); ++index) {
        const ipc::TabDescriptor& descriptor = update.tabs[index];
        const HWND tab = reinterpret_cast<HWND>(descriptor.tab_hwnd);
        if (tab == nullptr || descriptor.tab_generation == 0 ||
            descriptor.ui_thread_id != thread || liveTabs[index] != tab ||
            operations.get_parent(tab) != topLevel ||
            !ExactWindow(tab, thread, L"ShellTabWindowClass", operations) ||
            Find(*desired, tab) != nullptr ||
            std::any_of(
                desired->begin(), desired->end(),
                [&](const InstalledTab& prior) {
                    return prior.generation == descriptor.tab_generation;
                })) {
            return Failure(ERROR_INVALID_WINDOW_HANDLE);
        }
        desired->push_back({tab, descriptor.tab_generation});
    }
    return Success();
}

Status ValidateGenerations(
    const auto& state,
    const HWND topLevel,
    const ipc::TabSetUpdate& update,
    const std::vector<InstalledTab>& desired) {
    if (state.top_level == nullptr) {
        return Success();
    }
    if (state.lifetime_failed || state.cleanup_blocked) {
        return Failure(ERROR_INVALID_STATE);
    }
    if (state.top_level != topLevel ||
        update.top_level_generation < state.top_level_generation) {
        return Failure(ERROR_INVALID_STATE);
    }
    if (update.top_level_generation == state.top_level_generation) {
        return desired == state.authoritative
            ? Success()
            : Failure(ERROR_INVALID_STATE);
    }
    for (const InstalledTab& tab : desired) {
        const InstalledTab* const prior = Find(state.known_generations, tab.window);
        const InstalledTab* const active = Find(state.authoritative, tab.window);
        if (prior != nullptr &&
            (tab.generation < prior->generation ||
             (active == nullptr && tab.generation == prior->generation))) {
            return Failure(ERROR_INVALID_STATE);
        }
    }
    return Success();
}

enum class MutationKind { Added, Removed };
struct Mutation final {
    MutationKind kind;
    InstalledTab tab;
};

Status Rollback(
    auto& state,
    const std::vector<Mutation>& mutations,
    const TabSubclassOperations& operations) {
    Status firstFailure = Success();
    for (auto mutation = mutations.rbegin(); mutation != mutations.rend(); ++mutation) {
        Status status{};
        if (mutation->kind == MutationKind::Added) {
            status = operations.remove_subclass(
                mutation->tab.window, kTabSubclassId);
            if (status.ok()) {
                ForgetCallback(state, mutation->tab.window);
            } else {
                TrackPossibleCallback(state, mutation->tab);
            }
        } else {
            status = operations.install_subclass(
                mutation->tab.window, kTabSubclassId, mutation->tab.generation);
            TrackPossibleCallback(state, mutation->tab);
        }
        if (!status.ok() && firstFailure.ok()) {
            firstFailure = status;
        }
    }
    if (!firstFailure.ok()) {
        state.lifetime_failed = true;
        state.cleanup_blocked = true;
    }
    return firstFailure;
}

Status Reconcile(
    auto& state,
    const std::vector<InstalledTab>& desired,
    const TabSubclassOperations& operations) {
    const std::vector<InstalledTab> previousInstalled = state.installed;
    std::vector<Mutation> mutations;
    const auto fail = [&](const Status original) {
        const Status rollback = Rollback(state, mutations, operations);
        if (rollback.ok()) {
            state.installed = previousInstalled;
            return original;
        }
        return rollback;
    };

    // Reused HWND values must lose the old generation before the new reference is set.
    for (const InstalledTab& tab : desired) {
        const InstalledTab* const prior = Find(previousInstalled, tab.window);
        if (prior == nullptr || prior->generation == tab.generation) {
            continue;
        }
        const InstalledTab old = *prior;
        Status status = operations.remove_subclass(old.window, kTabSubclassId);
        if (!status.ok()) {
            return fail(status);
        }
        ForgetCallback(state, old.window);
        mutations.push_back({MutationKind::Removed, old});
        status = operations.install_subclass(
            tab.window, kTabSubclassId, tab.generation);
        if (!status.ok()) {
            return fail(status);
        }
        TrackPossibleCallback(state, tab);
        mutations.push_back({MutationKind::Added, tab});
    }

    // Install new HWNDs in authoritative z-order before retiring old HWNDs.
    for (const InstalledTab& tab : desired) {
        if (Find(state.installed, tab.window) != nullptr) {
            continue;
        }
        const Status status = operations.install_subclass(
            tab.window, kTabSubclassId, tab.generation);
        if (!status.ok()) {
            return fail(status);
        }
        TrackPossibleCallback(state, tab);
        mutations.push_back({MutationKind::Added, tab});
    }
    for (auto tab = previousInstalled.rbegin();
         tab != previousInstalled.rend(); ++tab) {
        if (Find(desired, tab->window) != nullptr) {
            continue;
        }
        const Status status = operations.remove_subclass(tab->window, kTabSubclassId);
        if (!status.ok()) {
            return fail(status);
        }
        ForgetCallback(state, tab->window);
        mutations.push_back({MutationKind::Removed, *tab});
    }

    if (mutations.empty()) {
        return Success();
    }
    const Status refresh = operations.post_refresh();
    if (!refresh.ok()) {
        return fail(refresh);
    }
    return Success();
}

Status GetClassNameStatus(const HWND window, std::wstring* const output) {
    if (output == nullptr) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    wchar_t name[256]{};
    const int length = GetClassNameW(window, name, 256);
    if (length <= 0) {
        return Failure(GetLastError());
    }
    output->assign(name, static_cast<std::size_t>(length));
    return Success();
}

LRESULT CALLBACK TabSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR id,
    const DWORD_PTR reference) {
    auto* const owner = reinterpret_cast<TabSubclassSet*>(reference);
    if (id == kTabSubclassId && owner != nullptr) {
        owner->Notify(window, message);
        if (message == WM_NCDESTROY || message == WM_SIZE ||
            message == WM_DPICHANGED || message == WM_THEMECHANGED ||
            message == WM_SHOWWINDOW || message == WM_WINDOWPOSCHANGED) {
            static_cast<void>(SignalHookRuntimeRefresh(owner));
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace

TabSubclassSet::TabSubclassSet() : impl_(std::make_unique<Impl>()) {}

TabSubclassSet::~TabSubclassSet() = default;

Status TabSubclassSet::Apply(
    const HWND topLevel,
    const ipc::TabSetUpdate& update,
    const TabSubclassOperations& operations,
    ipc::TabSetResult* const result) {
    if (result == nullptr) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    SetResult(result, update.top_level_generation, Failure());
    std::vector<InstalledTab> desired;
    Status status = ValidateUpdate(topLevel, update, operations, &desired);
    if (status.ok()) {
        status = ValidateGenerations(*impl_, topLevel, update, desired);
    }
    if (status.ok()) {
        status = Reconcile(*impl_, desired, operations);
    }
    if (status.ok()) {
        impl_->top_level = topLevel;
        impl_->top_level_generation = update.top_level_generation;
        impl_->authoritative = desired;
        impl_->installed = desired;
        for (const InstalledTab& tab : desired) {
            const auto known = std::find_if(
                impl_->known_generations.begin(),
                impl_->known_generations.end(),
                [&](const InstalledTab& candidate) {
                    return candidate.window == tab.window;
                });
            if (known == impl_->known_generations.end()) {
                impl_->known_generations.push_back(tab);
            } else if (tab.generation > known->generation) {
                known->generation = tab.generation;
            }
        }
    }
    SetResult(result, update.top_level_generation, status);
    return status;
}

Status TabSubclassSet::RemoveAll(const TabSubclassOperations& operations) {
    if (!operations.remove_subclass) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    Status firstFailure = Success();
    const std::vector<InstalledTab> tracked = impl_->installed;
    for (auto tab = tracked.rbegin(); tab != tracked.rend(); ++tab) {
        const Status status = operations.remove_subclass(
            tab->window, kTabSubclassId);
        if (status.ok()) {
            ForgetCallback(*impl_, tab->window);
        } else if (firstFailure.ok()) {
            firstFailure = status;
        }
    }
    if (!impl_->installed.empty()) {
        impl_->lifetime_failed = true;
        impl_->cleanup_blocked = true;
        return firstFailure.ok() ? Failure() : firstFailure;
    }
    *impl_ = {};
    return firstFailure;
}

bool TabSubclassSet::cleanup_safe() const noexcept {
    return !impl_->lifetime_failed && !impl_->cleanup_blocked;
}

std::size_t TabSubclassSet::active_count() const noexcept {
    return impl_->installed.size();
}

TabSubclassOperations CreateProductionTabSubclassOperations(
    TabSubclassSet& owner) {
    return {
        &GetCurrentThreadId,
        &GetWindowThreadProcessId,
        &GetClassNameStatus,
        &GetParent,
        [](const HWND window) { return GetWindow(window, GW_CHILD); },
        [](const HWND window) { return GetWindow(window, GW_HWNDNEXT); },
        [&owner](const HWND window, const UINT_PTR id, const std::uint64_t) {
            return SetWindowSubclass(
                       window,
                       TabSubclassProc,
                       id,
                       reinterpret_cast<DWORD_PTR>(&owner)) != FALSE
                ? Success()
                : Failure(GetLastError());
        },
        [](const HWND window, const UINT_PTR id) {
            return RemoveWindowSubclass(window, TabSubclassProc, id) != FALSE
                ? Success()
                : Failure(GetLastError());
        },
        [&owner] { return SignalHookRuntimeRefresh(&owner); },
    };
}

void TabSubclassSet::Notify(const HWND window, const UINT message) noexcept {
    if (window == nullptr || message != WM_NCDESTROY) {
        return;
    }
    ForgetCallback(*impl_, window);
    if (impl_->installed.empty()) {
        impl_->lifetime_failed = false;
        impl_->cleanup_blocked = false;
    }
}

}  // namespace winexinfo::hook
