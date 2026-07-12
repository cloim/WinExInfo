#include "test_framework.h"

#include "host/background_coordinator.h"
#include "host/com_apartment.h"
#include "probe/tab_identity.h"
#include "probe/win32_probe.h"

#include <algorithm>
#include <objbase.h>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

using winexinfo::BackgroundOperations;
using winexinfo::BackgroundSession;
using winexinfo::BackgroundSnapshot;
using winexinfo::ExplorerProcessSnapshot;
using winexinfo::HostExitCode;
using winexinfo::SessionWindowSnapshot;
using winexinfo::Status;

Status Ok() { return {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS}; }
Status Fail() {
    return {winexinfo::ErrorCode::PIPE_DISCONNECTED, E_FAIL, ERROR_BROKEN_PIPE};
}

SessionWindowSnapshot Window(std::uintptr_t hwnd, std::uint64_t generation) {
    return {
        reinterpret_cast<HWND>(hwnd),
        generation,
        {{hwnd + 1, generation, static_cast<DWORD>(hwnd & 0xFFu) + 1}},
    };
}

struct Recorder;

class FakeSession final : public BackgroundSession {
public:
    FakeSession(DWORD pid, Recorder* recorder) : pid_(pid), recorder_(recorder) {}
    Status Reconcile(std::span<const SessionWindowSnapshot> windows) override;
    Status Stop() override;

private:
    DWORD pid_;
    Recorder* recorder_;
};

struct Recorder final {
    std::deque<std::pair<BackgroundSnapshot, bool>> inputs;
    std::vector<std::string> events;
    std::vector<std::vector<SessionWindowSnapshot>> reconciled;
    std::vector<std::unique_ptr<BackgroundSession>> retained;
    DWORD fail_reconcile_pid = 0;
    DWORD fail_stop_pid = 0;
    bool mutex_ok = true;

    BackgroundOperations Operations() {
        return {
            [this]() {
                events.push_back("mutex.acquire");
                return mutex_ok ? Ok() : Fail();
            },
            [this]() { events.push_back("mutex.release"); },
            [this](BackgroundSnapshot* snapshot, bool* stop) {
                if (inputs.empty() || snapshot == nullptr || stop == nullptr) {
                    return Fail();
                }
                auto item = std::move(inputs.front());
                inputs.pop_front();
                *snapshot = std::move(item.first);
                *stop = item.second;
                return Ok();
            },
            [this](const DWORD pid, std::unique_ptr<BackgroundSession>* output) {
                events.push_back("create:" + std::to_string(pid));
                *output = std::make_unique<FakeSession>(pid, this);
                return Ok();
            },
            [this](std::unique_ptr<BackgroundSession> session) {
                events.push_back("retain");
                retained.push_back(std::move(session));
            },
        };
    }
};

Status FakeSession::Reconcile(std::span<const SessionWindowSnapshot> windows) {
    recorder_->events.push_back("reconcile:" + std::to_string(pid_));
    recorder_->reconciled.emplace_back(windows.begin(), windows.end());
    if (pid_ == recorder_->fail_reconcile_pid) return Fail();
    return Ok();
}

Status FakeSession::Stop() {
    recorder_->events.push_back("stop:" + std::to_string(pid_));
    return pid_ == recorder_->fail_stop_pid ? Fail() : Ok();
}

ExplorerProcessSnapshot Process(
    DWORD pid, bool validated, std::vector<SessionWindowSnapshot> windows) {
    return {pid, validated, std::move(windows)};
}

void StopInput(Recorder* recorder) {
    recorder->inputs.push_back({{}, true});
}

WXI_TEST(background_single_instance_mutex,
         "background_coordinator.single_instance_mutex_exact_name") {
    WXI_REQUIRE_EQ(
        winexinfo::kBackgroundMutexName,
        std::wstring_view{L"Local\\WinExInfo.Host.v1"});
    Recorder recorder;
    recorder.mutex_ok = false;
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Win32ComFailure);
    WXI_REQUIRE_EQ(recorder.events.size(), 1u);
}

WXI_TEST(background_existing_multi_window_one_session,
         "background_coordinator.existing_windows_one_session_per_validated_pid") {
    Recorder recorder;
    recorder.inputs.push_back({
        {1, {Process(41, true, {Window(0x1000, 1), Window(0x2000, 1)})}}, false});
    StopInput(&recorder);
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(
        std::count(recorder.events.begin(), recorder.events.end(), "create:41"), 1);
    WXI_REQUIRE_EQ(recorder.reconciled.size(), 1u);
    WXI_REQUIRE_EQ(recorder.reconciled[0].size(), 2u);
}

WXI_TEST(background_process_start_exit_reverse_stop,
         "background_coordinator.process_start_exit_stops_absent_deterministically") {
    Recorder recorder;
    recorder.inputs.push_back({
        {1, {Process(41, true, {Window(0x1000, 1)}),
             Process(42, true, {Window(0x2000, 1)})}}, false});
    recorder.inputs.push_back({{2, {}}, false});
    StopInput(&recorder);
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Pass);
    const auto stop42 = std::find(recorder.events.begin(), recorder.events.end(), "stop:42");
    const auto stop41 = std::find(recorder.events.begin(), recorder.events.end(), "stop:41");
    WXI_REQUIRE(stop42 < stop41);
}

WXI_TEST(background_unsupported_skip,
         "background_coordinator.unsupported_process_is_skipped") {
    Recorder recorder;
    recorder.inputs.push_back({
        {1, {Process(41, false, {Window(0x1000, 1)})}}, false});
    StopInput(&recorder);
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(
        std::count(recorder.events.begin(), recorder.events.end(), "create:41"), 0);
}

WXI_TEST(background_strict_snapshot_sequence,
         "background_coordinator.rejects_duplicate_or_stale_snapshot_sequence") {
    for (const std::uint64_t second : {std::uint64_t{1}, std::uint64_t{0}}) {
        Recorder recorder;
        recorder.inputs.push_back({{1, {}}, false});
        recorder.inputs.push_back({{second, {}}, false});
        WXI_REQUIRE_EQ(
            winexinfo::RunBackgroundCoordinator(recorder.Operations()),
            HostExitCode::ContractFailure);
    }
}

WXI_TEST(background_snapshot_is_immutable_copy,
         "background_coordinator.session_receives_immutable_snapshot_copy") {
    Recorder recorder;
    auto original = Window(0x1000, 1);
    recorder.inputs.push_back({{1, {Process(41, true, {original})}}, false});
    StopInput(&recorder);
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(recorder.reconciled[0][0], original);
}

WXI_TEST(background_transport_failure_retains_failed_session,
         "background_coordinator.transport_failure_retains_uncertain_session") {
    Recorder recorder;
    recorder.fail_reconcile_pid = 42;
    recorder.inputs.push_back({
        {1, {Process(41, true, {Window(0x1000, 1)}),
             Process(42, true, {Window(0x2000, 1)})}}, false});
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Win32ComFailure);
    WXI_REQUIRE_EQ(recorder.retained.size(), 1u);
    WXI_REQUIRE(
        std::find(recorder.events.begin(), recorder.events.end(), "stop:41") !=
        recorder.events.end());
}

WXI_TEST(background_stop_failure_retains_session,
         "background_coordinator.stop_failure_is_retained_and_reported") {
    Recorder recorder;
    recorder.fail_stop_pid = 41;
    recorder.inputs.push_back({
        {1, {Process(41, true, {Window(0x1000, 1)})}}, false});
    StopInput(&recorder);
    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Win32ComFailure);
    WXI_REQUIRE_EQ(recorder.retained.size(), 1u);
}

WXI_TEST(background_production_observer_non_injected,
         "background_coordinator.production_observer_builds_real_immutable_snapshot") {
    const HRESULT initialized = CoInitializeEx(
        nullptr, winexinfo::kShellComApartmentFlags);
    WXI_REQUIRE(SUCCEEDED(initialized));
    BackgroundSnapshot snapshot{};
    const Status captured =
        winexinfo::CaptureProductionBackgroundSnapshotOnce(7, &snapshot);
    if (SUCCEEDED(initialized)) CoUninitialize();
    WXI_REQUIRE(captured.ok());
    WXI_REQUIRE_EQ(snapshot.sequence, std::uint64_t{7});
    for (const auto& process : snapshot.processes) {
        WXI_REQUIRE(process.process_id != 0);
        for (const auto& window : process.windows) {
            WXI_REQUIRE(window.top_level != nullptr);
            WXI_REQUIRE(window.top_level_generation != 0);
            WXI_REQUIRE(!window.tabs.empty());
        }
    }
}

WXI_TEST(background_persistent_observer_generations,
         "background_coordinator.persistent_observer_increments_hwnd_reuse_generations") {
    winexinfo::BackgroundObserverTracker tracker;
    const HWND top = reinterpret_cast<HWND>(0x1000);
    const HWND tab = reinterpret_cast<HWND>(0x1100);
    const std::vector windows{winexinfo::ExplorerWindowRecord{top, 41, 11}};
    const std::vector firstMetadata{
        winexinfo::ObserverShellEntryMetadata{1, true, top, tab}};
    const std::vector orders{
        winexinfo::ObserverTopLevelTabOrder{top, {{tab, true}}}};
    const std::vector<DWORD> validated{41};
    BackgroundSnapshot first{};
    WXI_REQUIRE(tracker.Reconcile(
        1, windows, firstMetadata, orders, validated, &first).ok());
    WXI_REQUIRE_EQ(first.processes[0].windows[0].top_level_generation, 1u);
    WXI_REQUIRE_EQ(first.processes[0].windows[0].tabs[0].tab_generation, 1u);

    BackgroundSnapshot absent{};
    const std::vector<winexinfo::ExplorerWindowRecord> noWindows;
    const std::vector<winexinfo::ObserverShellEntryMetadata> noMetadata;
    const std::vector<winexinfo::ObserverTopLevelTabOrder> noOrders;
    WXI_REQUIRE(tracker.Reconcile(
        2, noWindows, noMetadata, noOrders, validated, &absent).ok());

    const std::vector secondMetadata{
        winexinfo::ObserverShellEntryMetadata{2, true, top, tab}};
    BackgroundSnapshot reused{};
    WXI_REQUIRE(tracker.Reconcile(
        3, windows, secondMetadata, orders, validated, &reused).ok());
    WXI_REQUIRE_EQ(reused.processes[0].windows[0].top_level_generation, 2u);
    WXI_REQUIRE_EQ(reused.processes[0].windows[0].tabs[0].tab_generation, 2u);
}

}  // namespace
