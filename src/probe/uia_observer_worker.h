#pragma once

#include "probe/observer_runtime.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

namespace winexinfo {

struct ObserverUiaTarget final {
    HWND top_level;
    std::uint64_t generation;
    HWND active_view;
    HWND shell_tab;

    bool operator==(const ObserverUiaTarget&) const = default;
};

enum class ObserverUiaCommandKind {
    AddTarget,
    RemoveTarget,
    Reenumerate,
};

struct ObserverUiaCommand final {
    std::uint64_t id;
    ObserverUiaCommandKind kind;
    ObserverUiaTarget target;
};

struct ObserverUiaResponse final {
    std::uint64_t id;
    ObserverUiaCommandKind kind;
    ObserverUiaTarget target;
    ObserverOperationResult operation;
    std::size_t direct_child_count;
};

struct ObserverUiaHandlerRemovalState final {
    std::uintptr_t owner_identity;
    std::uintptr_t selection_handler_identity;
    std::uintptr_t structure_handler_identity;
    bool selection_registered;
    bool structure_registered;
};

struct ObserverUiaHandlerRemovalOperations final {
    std::function<ObserverOperationResult(std::uintptr_t, std::uintptr_t)>
        remove_selection;
    std::function<ObserverOperationResult(std::uintptr_t, std::uintptr_t)>
        remove_structure;
};

[[nodiscard]] ObserverCleanupOutcome RemoveObserverUiaHandlerRegistrations(
    const ObserverUiaHandlerRemovalOperations& operations,
    ObserverUiaHandlerRemovalState* state) noexcept;

struct ObserverUiaWorkerOperations final {
    std::function<ObserverOperationResult()> initialize;
    std::function<ObserverUiaResponse(const ObserverUiaCommand&)> process;
    std::function<ObserverCleanupOutcome()> cleanup;
    std::function<void()> uninitialize;
    std::function<BOOL(HANDLE)> set_event;
    std::function<BOOL(HANDLE)> reset_event;
    std::function<DWORD()> get_last_error;
    std::function<void(
        std::deque<ObserverUiaResponse>*,
        ObserverUiaResponse&&)> append_response;
};

class ObserverUiaMtaWorker final {
public:
    explicit ObserverUiaMtaWorker(ObserverUiaWorkerOperations operations);
    ~ObserverUiaMtaWorker();

    ObserverUiaMtaWorker(const ObserverUiaMtaWorker&) = delete;
    ObserverUiaMtaWorker& operator=(const ObserverUiaMtaWorker&) = delete;

    [[nodiscard]] ObserverOperationResult Start(HANDLE responseEvent);
    [[nodiscard]] ObserverOperationResult Submit(
        ObserverUiaCommandKind kind,
        const ObserverUiaTarget& target,
        std::uint64_t* commandId);
    [[nodiscard]] ObserverOperationResult Consume(
        std::uint64_t expectedCommandId,
        ObserverUiaResponse* output);
    [[nodiscard]] ObserverOperationResult BeginStopping();
    [[nodiscard]] ObserverCleanupOutcome Join();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ObserverUiaWorkerOperations
CreateProductionObserverUiaWorkerOperations(ObserverCallbackQueue* queue);

}  // namespace winexinfo
