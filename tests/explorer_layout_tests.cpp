#include "hook/explorer_layout.h"

#include "test_framework.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <functional>
#include <limits>
#include <atomic>

namespace {

bool IsEmpty(const RECT& rectangle) {
    return rectangle.left == 0 && rectangle.top == 0 &&
        rectangle.right == 0 && rectangle.bottom == 0;
}

bool RectEquals(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

winexinfo::ExplorerLayoutCaptureEvidence ExactCaptureEvidence() {
    return {
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        true,
        Handle(0x300),
        {100, 100, 1100, 800},
        96,
        {
            {winexinfo::ExplorerLayoutUiaScope::PaneSubtree,
             L"DirectUI", UIA_StatusBarControlTypeId, L"StatusBarModuleInner",
             L"StatusBarModuleInner", nullptr, {100, 760, 1100, 800}},
            {winexinfo::ExplorerLayoutUiaScope::StatusBarChildren,
             L"DirectUI", UIA_GroupControlTypeId, L"System.StatusBarViewItemCount",
             L"Element", nullptr, {110, 760, 250, 800}},
            {winexinfo::ExplorerLayoutUiaScope::StatusBarChildren,
             L"DirectUI", UIA_GroupControlTypeId, L"ViewButtonsGroup",
             L"SelectorNoDefault", nullptr, {900, 760, 1090, 800}},
        },
    };
}

}  // namespace

WXI_TEST(explorer_layout_gap, "explorer_layout.gap") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{100, 100, 1100, 800},
         {100, 760, 1100, 800},
         {110, 760, 250, 800},
         {900, 760, 1090, 800},
         96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{158, 660, 792, 700}));
}

WXI_TEST(explorer_layout_scales_dips, "explorer_layout.scales_dips") {
    RECT at144{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 1200, 800}, {0, 740, 1200, 800}, {0, 740, 300, 800},
         {900, 740, 1200, 800}, 144},
        &at144).ok());
    WXI_REQUIRE(RectEquals(at144, RECT{312, 740, 888, 800}));

    RECT at192{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 1200, 800}, {0, 740, 1200, 800}, {0, 740, 300, 800},
         {900, 740, 1200, 800}, 192},
        &at192).ok());
    WXI_REQUIRE(RectEquals(at192, RECT{316, 740, 884, 800}));
}

WXI_TEST(explorer_layout_clips_to_parent_and_status, "explorer_layout.clipping") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{100, 100, 500, 400}, {-100, 350, 700, 450}, {-200, 350, 120, 450},
         {450, 350, 700, 450}, 96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{28, 250, 342, 300}));
}

WXI_TEST(explorer_layout_converts_negative_screen_origin, "explorer_layout.negative_origin") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{-500, -300, 500, 400}, {-700, 360, 700, 450}, {-700, 360, -400, 450},
         {400, 360, 700, 450}, 96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{108, 660, 892, 700}));
}

WXI_TEST(explorer_layout_accepts_exact_minimum, "explorer_layout.minimum_width") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 400, 100}, {0, 60, 400, 100}, {0, 60, 100, 100},
         {212, 60, 400, 100}, 96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{108, 60, 204, 100}));
}

WXI_TEST(explorer_layout_hides_narrow_gap, "explorer_layout.hidden_narrow") {
    RECT output{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 400, 100}, {0, 60, 400, 100}, {0, 60, 100, 100},
         {211, 60, 400, 100}, 96},
        &output).ok());
    WXI_REQUIRE(IsEmpty(output));
}

WXI_TEST(explorer_layout_hides_overlap_and_reversed_groups, "explorer_layout.group_order") {
    RECT overlap{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 500, 100}, {0, 60, 500, 100}, {0, 60, 300, 100},
         {250, 60, 500, 100}, 96},
        &overlap).ok());
    WXI_REQUIRE(IsEmpty(overlap));

    RECT reversed{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 500, 100}, {0, 60, 500, 100}, {350, 60, 450, 100},
         {50, 60, 150, 100}, 96},
        &reversed).ok());
    WXI_REQUIRE(IsEmpty(reversed));
}

WXI_TEST(explorer_layout_extreme_coordinates_do_not_wrap, "explorer_layout.no_overflow") {
    RECT output{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{-100, -100, 100, 100}, {-100, 50, 100, 100},
         {-100, 50, (std::numeric_limits<LONG>::max)(), 100},
         {(std::numeric_limits<LONG>::min)(), 50, 100, 100}, 192},
        &output).ok());
    WXI_REQUIRE(IsEmpty(output));
}

WXI_TEST(explorer_layout_rejects_invalid_arguments, "explorer_layout.invalid_arguments") {
    RECT output{};
    WXI_REQUIRE_EQ(
        winexinfo::ComputeStatusPaneRect({}, nullptr).code,
        winexinfo::ErrorCode::INVALID_ARGUMENT);

    winexinfo::ExplorerLayoutMetrics zeroDpi{};
    zeroDpi.parent_screen = {0, 0, 100, 100};
    zeroDpi.status_screen = {0, 60, 100, 100};
    zeroDpi.left_group_screen = {0, 60, 10, 100};
    zeroDpi.right_group_screen = {90, 60, 100, 100};
    zeroDpi.dpi = 0;
    WXI_REQUIRE_EQ(
        winexinfo::ComputeStatusPaneRect(zeroDpi, &output).code,
        winexinfo::ErrorCode::INVALID_ARGUMENT);
}

WXI_TEST(
    explorer_layout_preserves_output_on_invalid_rectangles,
    "explorer_layout.transactional_invalid") {
    const RECT sentinel{11, 22, 33, 44};
    RECT output = sentinel;
    WXI_REQUIRE_EQ(
        winexinfo::ComputeStatusPaneRect(
            {{100, 0, 0, 100}, {0, 60, 100, 100}, {0, 60, 10, 100},
             {90, 60, 100, 100}, 96},
            &output).code,
        winexinfo::ErrorCode::INVALID_ARGUMENT);
    WXI_REQUIRE(RectEquals(output, sentinel));
}

WXI_TEST(explorer_layout_capture_exact_contract, "explorer_layout.capture_exact_contract") {
    winexinfo::ExplorerLayoutMetrics metrics{};
    HWND parent = nullptr;
    WXI_REQUIRE(winexinfo::ValidateExplorerLayoutCapture(
        ExactCaptureEvidence(), &metrics, &parent).ok());
    WXI_REQUIRE_EQ(parent, Handle(0x300));
    WXI_REQUIRE(RectEquals(metrics.parent_screen, RECT{100, 100, 1100, 800}));
    WXI_REQUIRE(RectEquals(metrics.status_screen, RECT{100, 760, 1100, 800}));
    WXI_REQUIRE(RectEquals(metrics.left_group_screen, RECT{110, 760, 250, 800}));
    WXI_REQUIRE(RectEquals(metrics.right_group_screen, RECT{900, 760, 1090, 800}));
    WXI_REQUIRE_EQ(metrics.dpi, UINT{96});
}

WXI_TEST(
    explorer_layout_capture_rejects_cardinality_and_scope,
    "explorer_layout.capture_selector_contract") {
    const winexinfo::ExplorerLayoutMetrics sentinel{
        {1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}, 144};
    const HWND sentinelParent = Handle(0x999);

    auto duplicate = ExactCaptureEvidence();
    duplicate.elements.push_back(duplicate.elements[0]);
    winexinfo::ExplorerLayoutMetrics metrics = sentinel;
    HWND parent = sentinelParent;
    WXI_REQUIRE_EQ(
        winexinfo::ValidateExplorerLayoutCapture(duplicate, &metrics, &parent).code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, sentinelParent);

    auto wrongScope = ExactCaptureEvidence();
    wrongScope.elements[1].scope = winexinfo::ExplorerLayoutUiaScope::PaneSubtree;
    metrics = sentinel;
    parent = sentinelParent;
    WXI_REQUIRE_EQ(
        winexinfo::ValidateExplorerLayoutCapture(wrongScope, &metrics, &parent).code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, sentinelParent);

    auto alternateSelector = ExactCaptureEvidence();
    alternateSelector.elements[0].automation_id = L"StatusBar";
    metrics = sentinel;
    parent = sentinelParent;
    WXI_REQUIRE_EQ(
        winexinfo::ValidateExplorerLayoutCapture(
            alternateSelector, &metrics, &parent).code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, sentinelParent);

    auto nativeGroup = ExactCaptureEvidence();
    nativeGroup.elements[2].native_window_handle = Handle(0x444);
    metrics = sentinel;
    parent = sentinelParent;
    WXI_REQUIRE_EQ(
        winexinfo::ValidateExplorerLayoutCapture(nativeGroup, &metrics, &parent).code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, sentinelParent);
}

WXI_TEST(explorer_layout_capture_requires_mta, "explorer_layout.capture_requires_mta") {
    auto evidence = ExactCaptureEvidence();
    evidence.executed_in_mta = false;
    winexinfo::ExplorerLayoutMetrics metrics{
        {1, 2, 3, 4}, {}, {}, {}, 144};
    HWND parent = Handle(0x999);
    WXI_REQUIRE_EQ(
        winexinfo::ValidateExplorerLayoutCapture(evidence, &metrics, &parent).code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE(RectEquals(metrics.parent_screen, RECT{1, 2, 3, 4}));
    WXI_REQUIRE_EQ(parent, Handle(0x999));
}

WXI_TEST(
    explorer_layout_capture_runs_on_bounded_worker,
    "explorer_layout.capture_bounded_worker") {
    DWORD observedTimeout = 0;
    winexinfo::ExplorerLayoutCaptureOperations operations{
        [&](const HWND, winexinfo::ExplorerLayoutCaptureEvidence* const evidence) {
            *evidence = ExactCaptureEvidence();
            return winexinfo::Status{winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
        },
        [&](winexinfo::ExplorerLayoutMtaTask task, const DWORD timeout) {
            observedTimeout = timeout;
            task({winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS});
            return winexinfo::Status{winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
        },
    };
    winexinfo::ExplorerLayoutMetrics metrics{};
    HWND parent = nullptr;
    WXI_REQUIRE(winexinfo::CaptureExplorerLayoutWithOperations(
        Handle(0x100), &metrics, &parent, operations).ok());
    WXI_REQUIRE_EQ(observedTimeout, winexinfo::kExplorerLayoutCaptureTimeoutMs);
    WXI_REQUIRE_EQ(parent, Handle(0x300));
}

WXI_TEST(
    explorer_layout_production_worker_is_mta,
    "explorer_layout.capture_production_mta") {
    const DWORD callerThread = GetCurrentThreadId();
    DWORD captureThread = callerThread;
    winexinfo::ExplorerLayoutCaptureOperations operations =
        winexinfo::GetProductionExplorerLayoutCaptureOperations();
    operations.capture_current_thread =
        [&](const HWND, winexinfo::ExplorerLayoutCaptureEvidence* const evidence) {
            captureThread = GetCurrentThreadId();
            *evidence = ExactCaptureEvidence();
            APTTYPE apartmentType = APTTYPE_CURRENT;
            APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
            evidence->executed_in_mta =
                CoGetApartmentType(&apartmentType, &qualifier) == S_OK &&
                apartmentType == APTTYPE_MTA;
            return winexinfo::Status{
                winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
        };

    winexinfo::ExplorerLayoutMetrics metrics{};
    HWND parent = nullptr;
    WXI_REQUIRE(winexinfo::CaptureExplorerLayoutWithOperations(
        Handle(0x100), &metrics, &parent, operations).ok());
    WXI_REQUIRE(captureThread != callerThread);
    WXI_REQUIRE_EQ(parent, Handle(0x300));
}

WXI_TEST(
    explorer_layout_capture_timeout_is_transactional_and_heap_safe,
    "explorer_layout.capture_timeout") {
    winexinfo::ExplorerLayoutMtaTask delayedTask;
    DWORD observedTimeout = 0;
    winexinfo::ExplorerLayoutCaptureOperations operations{
        [](const HWND, winexinfo::ExplorerLayoutCaptureEvidence* const evidence) {
            *evidence = ExactCaptureEvidence();
            return winexinfo::Status{winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
        },
        [&](winexinfo::ExplorerLayoutMtaTask task, const DWORD timeout) {
            observedTimeout = timeout;
            delayedTask = std::move(task);
            return winexinfo::Status{
                winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                ERROR_TIMEOUT};
        },
    };
    const winexinfo::ExplorerLayoutMetrics sentinel{
        {1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}, 144};
    winexinfo::ExplorerLayoutMetrics metrics = sentinel;
    HWND parent = Handle(0x999);
    const winexinfo::Status status = winexinfo::CaptureExplorerLayoutWithOperations(
        Handle(0x100), &metrics, &parent, operations);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_TIMEOUT});
    WXI_REQUIRE_EQ(observedTimeout, winexinfo::kExplorerLayoutCaptureTimeoutMs);
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, Handle(0x999));

    WXI_REQUIRE(static_cast<bool>(delayedTask));
    delayedTask({winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS});
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, Handle(0x999));
}

WXI_TEST(
    explorer_layout_capture_failure_is_transactional,
    "explorer_layout.capture_failure_transactional") {
    winexinfo::ExplorerLayoutCaptureOperations operations{
        [](const HWND, winexinfo::ExplorerLayoutCaptureEvidence*) {
            return winexinfo::Status{
                winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
                ERROR_SUCCESS};
        },
        [](winexinfo::ExplorerLayoutMtaTask task, const DWORD) {
            task({winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS});
            return winexinfo::Status{
                winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
        },
    };
    const winexinfo::ExplorerLayoutMetrics sentinel{
        {1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}, 144};
    winexinfo::ExplorerLayoutMetrics metrics = sentinel;
    HWND parent = Handle(0x999);
    const winexinfo::Status status = winexinfo::CaptureExplorerLayoutWithOperations(
        Handle(0x100), &metrics, &parent, operations);
    WXI_REQUIRE_EQ(status.hresult, static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
    WXI_REQUIRE(RectEquals(metrics.parent_screen, sentinel.parent_screen));
    WXI_REQUIRE_EQ(parent, Handle(0x999));
}

WXI_TEST(
    explorer_layout_worker_module_release_paths,
    "explorer_layout.worker_module_paths") {
    const HMODULE executable = reinterpret_cast<HMODULE>(0x1000);
    const HMODULE library = reinterpret_cast<HMODULE>(0x2000);
    WXI_REQUIRE_EQ(
        winexinfo::ClassifyExplorerLayoutWorkerModule(executable, executable),
        winexinfo::ExplorerLayoutWorkerModuleKind::Executable);
    WXI_REQUIRE_EQ(
        winexinfo::GetExplorerLayoutWorkerReleasePath(
            winexinfo::ExplorerLayoutWorkerModuleKind::Executable),
        winexinfo::ExplorerLayoutWorkerReleasePath::FreeLibraryThenExitThread);
    WXI_REQUIRE_EQ(
        winexinfo::ClassifyExplorerLayoutWorkerModule(library, executable),
        winexinfo::ExplorerLayoutWorkerModuleKind::DynamicLibrary);
    WXI_REQUIRE_EQ(
        winexinfo::GetExplorerLayoutWorkerReleasePath(
            winexinfo::ExplorerLayoutWorkerModuleKind::DynamicLibrary),
        winexinfo::ExplorerLayoutWorkerReleasePath::FreeLibraryAndExitThread);
}

WXI_TEST(
    explorer_layout_worker_balances_delayed_timeout_retention,
    "explorer_layout.worker_timeout_balances_module") {
    for (const winexinfo::ExplorerLayoutWorkerModuleKind kind : {
             winexinfo::ExplorerLayoutWorkerModuleKind::Executable,
             winexinfo::ExplorerLayoutWorkerModuleKind::DynamicLibrary}) {
        struct LifetimeState final {
            HANDLE release_gate;
            HANDLE released;
            std::atomic<int> retains{0};
            std::atomic<int> ordinary_releases{0};
            std::atomic<int> releases{0};
            std::atomic<bool> apartment_ok{false};
            winexinfo::ExplorerLayoutWorkerModuleKind requested_kind{};
            winexinfo::ExplorerLayoutWorkerModuleKind retained_kind{};
            winexinfo::ExplorerLayoutWorkerModuleKind released_kind{};
        } state{
            CreateEventW(nullptr, TRUE, FALSE, nullptr),
            CreateEventW(nullptr, TRUE, FALSE, nullptr),
        };
        WXI_REQUIRE(state.release_gate != nullptr);
        WXI_REQUIRE(state.released != nullptr);

        const winexinfo::ExplorerLayoutWorkerModuleOperations operations{
            &state,
            [](void* const context,
               winexinfo::ExplorerLayoutWorkerModuleRetention* const retention) {
                auto* const state = static_cast<LifetimeState*>(context);
                ++state->retains;
                state->retained_kind = state->requested_kind;
                *retention = {
                    reinterpret_cast<HMODULE>(0x1234),
                    state->retained_kind,
                };
                return winexinfo::Status{
                    winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
            },
            [](void* const context,
               const winexinfo::ExplorerLayoutWorkerModuleRetention) {
                auto* const state = static_cast<LifetimeState*>(context);
                ++state->ordinary_releases;
            },
            [](void* const context,
               const winexinfo::ExplorerLayoutWorkerModuleRetention retention) {
                auto* const state = static_cast<LifetimeState*>(context);
                ++state->releases;
                state->released_kind = retention.kind;
                static_cast<void>(SetEvent(state->released));
                ExitThread(0);
            },
        };
        state.requested_kind = kind;

        const winexinfo::Status timeout =
            winexinfo::RunExplorerLayoutMtaWorkerWithOperations(
                [&state](const winexinfo::Status apartmentStatus) {
                    state.apartment_ok = apartmentStatus.ok();
                    static_cast<void>(WaitForSingleObject(state.release_gate, 5000));
                },
                1,
                operations);
        WXI_REQUIRE_EQ(timeout.win32, DWORD{ERROR_TIMEOUT});
        WXI_REQUIRE(state.apartment_ok.load());
        WXI_REQUIRE_EQ(state.retains.load(), 1);
        WXI_REQUIRE_EQ(state.ordinary_releases.load(), 0);
        WXI_REQUIRE_EQ(state.releases.load(), 0);

        WXI_REQUIRE(SetEvent(state.release_gate) != FALSE);
        WXI_REQUIRE_EQ(WaitForSingleObject(state.released, 5000), DWORD{WAIT_OBJECT_0});
        WXI_REQUIRE_EQ(state.releases.load(), 1);
        WXI_REQUIRE_EQ(state.ordinary_releases.load(), 0);
        WXI_REQUIRE_EQ(state.retained_kind, kind);
        WXI_REQUIRE_EQ(state.released_kind, kind);
        CloseHandle(state.released);
        CloseHandle(state.release_gate);
    }
}

WXI_TEST(
    explorer_layout_preserves_output_on_client_overflow,
    "explorer_layout.transactional_overflow") {
    const RECT sentinel{11, 22, 33, 44};
    RECT output = sentinel;
    WXI_REQUIRE_EQ(
        winexinfo::ComputeStatusPaneRect(
            {{(std::numeric_limits<LONG>::min)(), 0,
              (std::numeric_limits<LONG>::max)(), 100},
             {(std::numeric_limits<LONG>::min)(), 60,
              (std::numeric_limits<LONG>::max)(), 100},
             {(std::numeric_limits<LONG>::min)(), 60,
              (std::numeric_limits<LONG>::min)(), 100},
             {(std::numeric_limits<LONG>::max)(), 60,
              (std::numeric_limits<LONG>::max)(), 100},
             96},
            &output).code,
        winexinfo::ErrorCode::INVALID_ARGUMENT);
    WXI_REQUIRE(RectEquals(output, sentinel));
}
