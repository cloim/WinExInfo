#include "host/explorer_controller.h"

#include "test_framework.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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
    HostExitCode hook_release_result = HostExitCode::Pass;
    bool still_matches = true;
    DWORD pane_owner = 42;
    bool pane_absent = true;
    bool module_absent = true;
    HostExitCode pane_absent_result = HostExitCode::Pass;
    HostExitCode module_absent_result = HostExitCode::Pass;
    HostExitCode release_result = HostExitCode::Pass;
    bool attach_context_may_live = false;
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
        [recorder](const ExplorerControllerTarget&,
                   const std::wstring_view path,
                   const std::function<void()>& accepted,
                   bool* const contextMayLive) {
            recorder->calls.push_back(path == L"hook.dll" ? "attach_setup" : "attach_bad_path");
            recorder->calls.push_back("final_revalidate");
            *contextMayLive = recorder->attach_context_may_live;
            if (!recorder->still_matches) {
                return HostExitCode::ContractFailure;
            }
            accepted();
            recorder->calls.push_back("hook_install");
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
            recorder->calls.push_back("release_hook");
            return recorder->hook_release_result;
        },
        [recorder](const ExplorerControllerTarget&) {
            recorder->calls.push_back("detach");
            return recorder->detach_result;
        },
        [recorder](const ExplorerControllerTarget&, bool* const absent) {
            recorder->calls.push_back("pane_absent");
            *absent = recorder->pane_absent;
            return recorder->pane_absent_result;
        },
        [recorder](const DWORD pid, bool* const absent) {
            recorder->calls.push_back(pid == 42 ? "module_absent" : "module_bad_pid");
            *absent = recorder->module_absent;
            return recorder->module_absent_result;
        },
        [recorder](const DWORD) {
            recorder->calls.push_back("release");
            return recorder->release_result;
        },
        {},
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
        "inspect", "attach_setup", "final_revalidate", "accepted", "hook_install", "pane", "wait",
        "release_hook", "detach", "pane_absent", "module_absent", "release"}));
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
        "inspect", "attach_setup", "final_revalidate"}));
    WXI_REQUIRE_EQ(recorder.accepted_count, 0U);
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
    WXI_REQUIRE_EQ(recorder.calls.back(), std::string{"hook_install"});
}

WXI_TEST(explorer_controller_attach_transport_failure_is_infrastructure,
         "explorer_controller.attach_infrastructure") {
    Recorder recorder;
    recorder.attach_result = HostExitCode::Win32ComFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Win32ComFailure);
}

WXI_TEST(explorer_controller_rejects_pane_owner_pid_mismatch,
         "explorer_controller.pane_owner") {
    Recorder recorder;
    recorder.pane_owner = 43;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{
        "inspect", "attach_setup", "final_revalidate", "accepted", "hook_install", "pane", "release_hook", "detach",
        "pane_absent", "module_absent", "release"}));
}

WXI_TEST(explorer_controller_reports_layout_failure,
         "explorer_controller.layout_failure") {
    Recorder recorder;
    recorder.pane_result = HostExitCode::ContractFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::ContractFailure);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{
        "inspect", "attach_setup", "final_revalidate", "accepted", "hook_install", "pane", "release_hook", "detach",
        "pane_absent", "module_absent", "release"}));
}

WXI_TEST(explorer_controller_pane_enumeration_error_is_infrastructure,
         "explorer_controller.pane_enumeration_infrastructure") {
    Recorder recorder;
    recorder.pane_result = HostExitCode::Win32ComFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Win32ComFailure);
}

WXI_TEST(explorer_controller_waits_duration_then_detaches,
         "explorer_controller.duration_and_detach_order") {
    Recorder recorder;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Pass);
    WXI_REQUIRE_EQ(recorder.waited_ms, 7000U);
    const auto& calls = recorder.calls;
    WXI_REQUIRE(calls[6] == "wait" && calls[7] == "release_hook" &&
                calls[8] == "detach" && calls[9] == "pane_absent");
}

WXI_TEST(explorer_controller_release_failure_prevents_detach_request,
         "explorer_controller.hook_release_order") {
    Recorder recorder;
    recorder.hook_release_result = HostExitCode::Win32ComFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Win32ComFailure);
    WXI_REQUIRE(std::find(recorder.calls.begin(), recorder.calls.end(), "release_hook") !=
                recorder.calls.end());
    WXI_REQUIRE(std::find(recorder.calls.begin(), recorder.calls.end(), "detach") ==
                recorder.calls.end());
}

WXI_TEST(explorer_controller_pipe_cleanup_error_is_infrastructure,
         "explorer_controller.cleanup_infrastructure") {
    Recorder recorder;
    recorder.pane_absent_result = HostExitCode::Win32ComFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Win32ComFailure);
    WXI_REQUIRE(std::find(recorder.calls.begin(), recorder.calls.end(), "release") ==
                recorder.calls.end());
}

WXI_TEST(explorer_controller_module_query_error_is_infrastructure,
         "explorer_controller.module_infrastructure") {
    Recorder recorder;
    recorder.module_absent_result = HostExitCode::Win32ComFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Win32ComFailure);
}

WXI_TEST(explorer_controller_release_failure_is_infrastructure,
         "explorer_controller.release_failure") {
    Recorder recorder;
    recorder.release_result = HostExitCode::Win32ComFailure;
    WXI_REQUIRE_EQ(Run(&recorder), HostExitCode::Win32ComFailure);
}

struct LifetimeProbe final {
    explicit LifetimeProbe(bool* const destroyedValue) : destroyed(destroyedValue) {}
    ~LifetimeProbe() { *destroyed = true; }
    bool* destroyed;
};

WXI_TEST(explorer_controller_cleanup_failure_retains_real_context_lifetime,
         "explorer_controller.real_lifetime_retention") {
    bool destroyed = false;
    std::weak_ptr<LifetimeProbe> weak;
    {
        Recorder recorder;
        recorder.pane_absent = false;
        ExplorerControllerOperations operations = Operations(&recorder);
        auto context = std::make_shared<LifetimeProbe>(&destroyed);
        weak = context;
        operations.retention_context = context;
        context.reset();
        WXI_REQUIRE_EQ(
            RunGateCPlacementWithOperations(kTarget, 7000, L"hook.dll", operations),
            HostExitCode::ContractFailure);
    }
    WXI_REQUIRE(!destroyed);
    WXI_REQUIRE(!weak.expired());
}

WXI_TEST(explorer_controller_file_identity_survives_path_replacement,
         "explorer_controller.immutable_file_identity") {
    const auto directory = std::filesystem::temp_directory_path() /
        L"WinExInfo.ControllerIdentity";
    std::filesystem::create_directories(directory);
    const auto expectedPath = directory / L"WinExInfoHook.dll";
    const auto mappedPath = directory / L"MappedOriginal.dll";
    std::filesystem::remove(expectedPath);
    std::filesystem::remove(mappedPath);
    { std::ofstream file(expectedPath, std::ios::binary); file << "original"; }
    ExplorerControllerFileIdentity captured{};
    WXI_REQUIRE_EQ(
        CaptureExplorerControllerFileIdentity(expectedPath.wstring(), &captured),
        HostExitCode::Pass);
    std::filesystem::rename(expectedPath, mappedPath);
    { std::ofstream file(expectedPath, std::ios::binary); file << "replacement"; }
    ExplorerControllerFileIdentity mapped{};
    ExplorerControllerFileIdentity replacement{};
    WXI_REQUIRE_EQ(
        CaptureExplorerControllerFileIdentity(mappedPath.wstring(), &mapped),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(
        CaptureExplorerControllerFileIdentity(expectedPath.wstring(), &replacement),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(mapped, captured);
    WXI_REQUIRE(!(replacement == captured));
    bool mappedPresent = false;
    bool replacementPresent = true;
    WXI_REQUIRE_EQ(
        CheckExplorerControllerFileIdentity(
            mappedPath.wstring(), captured, &mappedPresent),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(
        CheckExplorerControllerFileIdentity(
            expectedPath.wstring(), captured, &replacementPresent),
        HostExitCode::Pass);
    WXI_REQUIRE(mappedPresent);
    WXI_REQUIRE(!replacementPresent);
    std::filesystem::remove_all(directory);
}

WXI_TEST(explorer_controller_identity_lock_blocks_path_mutation_until_release,
         "explorer_controller.identity_lock") {
    const auto directory = std::filesystem::temp_directory_path() /
        L"WinExInfo.ControllerIdentityLock";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto target = directory / L"WinExInfoHook.dll";
    const auto replacement = directory / L"Replacement.dll";
    const auto renamed = directory / L"Renamed.dll";
    { std::ofstream file(target, std::ios::binary); file << "original"; }
    { std::ofstream file(replacement, std::ios::binary); file << "replacement"; }

    ExplorerControllerFileIdentityLock lock{};
    WXI_REQUIRE_EQ(
        AcquireExplorerControllerFileIdentityLock(target.wstring(), &lock),
        HostExitCode::Pass);
    WXI_REQUIRE(MoveFileExW(target.c_str(), renamed.c_str(), 0) == FALSE);
    WXI_REQUIRE_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    WXI_REQUIRE(DeleteFileW(target.c_str()) == FALSE);
    WXI_REQUIRE_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    WXI_REQUIRE(ReplaceFileW(
        target.c_str(), replacement.c_str(), nullptr, 0, nullptr, nullptr) == FALSE);
    WXI_REQUIRE_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    UniqueHandle writer{CreateFileW(
        target.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    WXI_REQUIRE(!writer);
    WXI_REQUIRE_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));

    lock.file.reset();
    WXI_REQUIRE(ReplaceFileW(
        target.c_str(), replacement.c_str(), nullptr, 0, nullptr, nullptr) != FALSE);
    WXI_REQUIRE(MoveFileExW(target.c_str(), renamed.c_str(), 0) != FALSE);
    WXI_REQUIRE(DeleteFileW(renamed.c_str()) != FALSE);
    std::filesystem::remove_all(directory);
}

WXI_TEST(explorer_controller_device_path_uses_longest_component_prefix,
         "explorer_controller.device_path_prefix") {
    const std::vector<ExplorerControllerDeviceMapping> mappings{
        {L"\\Device\\HarddiskVolume1", L"C:"},
        {L"\\Device\\HarddiskVolume10", L"D:"},
        {L"\\Device\\HarddiskVolume", L"Z:"},
    };
    std::wstring converted;
    WXI_REQUIRE_EQ(
        SelectExplorerControllerDosPath(
            L"\\Device\\HarddiskVolume10\\apps\\WinExInfoHook.dll",
            mappings,
            &converted),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(converted, std::wstring{L"D:\\apps\\WinExInfoHook.dll"});
    WXI_REQUIRE_EQ(
        SelectExplorerControllerDosPath(
            L"\\Device\\HarddiskVolume100\\apps\\WinExInfoHook.dll",
            mappings,
            &converted),
        HostExitCode::Win32ComFailure);
    const std::vector<ExplorerControllerDeviceMapping> ambiguous{
        {L"\\Device\\HarddiskVolume10", L"D:"},
        {L"\\Device\\HarddiskVolume10", L"E:"},
    };
    WXI_REQUIRE_EQ(
        SelectExplorerControllerDosPath(
            L"\\Device\\HarddiskVolume10\\apps\\WinExInfoHook.dll",
            ambiguous,
            &converted),
        HostExitCode::Win32ComFailure);
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
