#include "host/explorer_controller.h"

#include "test_framework.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace winexinfo::tests {
namespace {

const HWND kTarget = reinterpret_cast<HWND>(std::uintptr_t{0x100});
const HWND kOther = reinterpret_cast<HWND>(std::uintptr_t{0x200});

struct Recorder final {
    ExplorerControllerTarget inspected{42, 84, kTarget};
    HostExitCode inspect_result = HostExitCode::Pass;
    HostExitCode attach_result = HostExitCode::Pass;
    HostExitCode pane_result = HostExitCode::Pass;
    HostExitCode detach_result = HostExitCode::Pass;
    bool still_matches = true;
    DWORD pane_owner = 42;
    bool pane_absent = true;
    bool module_absent = true;
    std::uint32_t accepted_count = 0;
    std::uint32_t waited_ms = 0;
    std::vector<std::string> calls;
};

ExplorerControllerOperations Operations(Recorder* const recorder) {
    return {
        [recorder](const HWND target, ExplorerControllerTarget* const output) {
            recorder->calls.push_back(target == kTarget ? "inspect" : "inspect_other");
            *output = recorder->inspected;
            return recorder->inspect_result;
        },
        [recorder](const ExplorerControllerTarget&) {
            ++recorder->accepted_count;
            recorder->calls.push_back("accepted");
        },
        [recorder](const ExplorerControllerTarget&) {
            recorder->calls.push_back("revalidate");
            return recorder->still_matches;
        },
        [recorder](const ExplorerControllerTarget&, const std::wstring_view path) {
            recorder->calls.push_back(path == L"hook.dll" ? "attach" : "attach_bad_path");
            return recorder->attach_result;
        },
        [recorder](const ExplorerControllerTarget&, DWORD* const owner) {
            recorder->calls.push_back("pane");
            *owner = recorder->pane_owner;
            return recorder->pane_result;
        },
        [recorder](const std::uint32_t duration) {
            recorder->calls.push_back("wait");
            recorder->waited_ms = duration;
        },
        [recorder](const ExplorerControllerTarget&) {
            recorder->calls.push_back("detach");
            return recorder->detach_result;
        },
        [recorder](const ExplorerControllerTarget&) {
            recorder->calls.push_back("pane_absent");
            return recorder->pane_absent;
        },
        [recorder](const DWORD pid, const std::wstring_view path) {
            recorder->calls.push_back(
                pid == 42 && path == L"hook.dll" ? "module_absent" : "module_bad_identity");
            return recorder->module_absent;
        },
        [recorder](const DWORD) {
            recorder->calls.push_back("release");
        },
    };
}

HostExitCode Run(Recorder* const recorder, const HWND target = kTarget) {
    return RunGateCPlacementWithOperations(
        target, 7000, L"hook.dll", Operations(recorder));
}

}  // namespace

WXI_TEST(explorer_controller_accepts_one_exact_top_level_target,
         "explorer_controller.exact_target") {
    Recorder recorder;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Pass);
    WXI_REQUIRE_EQ(recorder.accepted_count, 1U);
    WXI_REQUIRE_EQ(recorder.waited_ms, 7000U);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{
        "inspect", "accepted", "revalidate", "attach", "pane", "wait",
        "detach", "pane_absent", "module_absent", "release"}));
}

WXI_TEST(explorer_controller_rejects_wrong_or_non_explorer_hwnd,
         "explorer_controller.wrong_hwnd") {
    Recorder recorder;
    recorder.inspect_result = HostExitCode::ContractFailure;
    WXI_REQUIRE_EQ(Run(&recorder, kOther), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.accepted_count, 0U);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{"inspect_other"}));
}

WXI_TEST(explorer_controller_rejects_target_disappearing_before_attach,
         "explorer_controller.disappears_before_attach") {
    Recorder recorder;
    recorder.still_matches = false;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{
        "inspect", "accepted", "revalidate"}));
}

WXI_TEST(explorer_controller_rejects_process_thread_mismatch,
         "explorer_controller.process_thread_mismatch") {
    Recorder recorder;
    recorder.inspected.thread_id = 0;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.accepted_count, 0U);
}

WXI_TEST(explorer_controller_rejects_attach_result_mismatch,
         "explorer_controller.attach_mismatch") {
    Recorder recorder;
    recorder.attach_result = HostExitCode::ContractFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls.back(), std::string{"attach"});
}

WXI_TEST(explorer_controller_rejects_pane_owner_pid_mismatch,
         "explorer_controller.pane_owner") {
    Recorder recorder;
    recorder.pane_owner = 43;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{
        "inspect", "accepted", "revalidate", "attach", "pane", "detach",
        "pane_absent", "module_absent", "release"}));
}

WXI_TEST(explorer_controller_reports_layout_failure,
         "explorer_controller.layout_failure") {
    Recorder recorder;
    recorder.pane_result = HostExitCode::ContractFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{
        "inspect", "accepted", "revalidate", "attach", "pane", "detach",
        "pane_absent", "module_absent", "release"}));
}

WXI_TEST(explorer_controller_waits_duration_then_detaches,
         "explorer_controller.duration_and_detach_order") {
    Recorder recorder;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Pass);
    WXI_REQUIRE_EQ(recorder.waited_ms, 7000U);
    const auto& calls = recorder.calls;
    WXI_REQUIRE(calls[5] == "wait" && calls[6] == "detach" &&
                calls[7] == "pane_absent");
}

WXI_TEST(explorer_controller_requires_exact_module_disappearance,
         "explorer_controller.module_disappearance") {
    Recorder recorder;
    recorder.module_absent = false;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls.back(), std::string{"module_absent"});
}

WXI_TEST(explorer_controller_cleanup_timeout_retains_context,
         "explorer_controller.cleanup_timeout_retention") {
    Recorder recorder;
    recorder.pane_absent = false;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls.back(), std::string{"module_absent"});
    WXI_REQUIRE(std::find(recorder.calls.begin(), recorder.calls.end(), "release") ==
                recorder.calls.end());
}

}  // namespace winexinfo::tests
