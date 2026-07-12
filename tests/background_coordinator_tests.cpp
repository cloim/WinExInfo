#include "test_framework.h"

#include "host/background_coordinator.h"
#include "host/com_apartment.h"
#include "probe/tab_identity.h"
#include "probe/win32_probe.h"

#include <algorithm>
#include <Aclapi.h>
#include <objbase.h>
#include <deque>
#include <filesystem>
#include <fstream>
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

WXI_TEST(background_stop_event_is_exact_per_host_pid,
         "background_coordinator.stop_event_name") {
    std::wstring name;
    WXI_REQUIRE(winexinfo::BuildBackgroundStopEventName(4321, &name).ok());
    WXI_REQUIRE_EQ(name, std::wstring{L"Local\\WinExInfo.Host.Stop.v1.4321"});
    WXI_REQUIRE(!winexinfo::BuildBackgroundStopEventName(0, &name).ok());
    winexinfo::UniqueHandle first;
    winexinfo::UniqueHandle collision;
    WXI_REQUIRE(winexinfo::CreateBackgroundStopEvent(GetCurrentProcessId(), &first).ok());
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    WXI_REQUIRE_EQ(GetSecurityInfo(
        first.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor), DWORD{ERROR_SUCCESS});
    WXI_REQUIRE(dacl != nullptr);
    WXI_REQUIRE_EQ(dacl->AceCount, WORD{1});
    void* rawAce = nullptr;
    WXI_REQUIRE(GetAce(dacl, 0, &rawAce) != FALSE);
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
    WXI_REQUIRE_EQ(ace->Header.AceType, BYTE{ACCESS_ALLOWED_ACE_TYPE});
    WXI_REQUIRE_EQ(ace->Mask, DWORD{EVENT_MODIFY_STATE | SYNCHRONIZE});
    HANDLE rawToken = nullptr;
    WXI_REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) != FALSE);
    winexinfo::UniqueHandle token{rawToken};
    DWORD tokenBytes = 0;
    WXI_REQUIRE(GetTokenInformation(
        token.get(), TokenUser, nullptr, 0, &tokenBytes) == FALSE);
    WXI_REQUIRE_EQ(GetLastError(), DWORD{ERROR_INSUFFICIENT_BUFFER});
    std::vector<std::uint8_t> tokenBuffer(tokenBytes);
    WXI_REQUIRE(GetTokenInformation(
        token.get(), TokenUser, tokenBuffer.data(), tokenBytes,
        &tokenBytes) != FALSE);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
    const auto* aceSid = reinterpret_cast<const SID*>(&ace->SidStart);
    WXI_REQUIRE(EqualSid(user->User.Sid, const_cast<SID*>(aceSid)) != FALSE);
    LocalFree(descriptor);
    WXI_REQUIRE(!winexinfo::CreateBackgroundStopEvent(
        GetCurrentProcessId(), &collision).ok());
    first.reset();
    WXI_REQUIRE(winexinfo::CreateBackgroundStopEvent(
        GetCurrentProcessId(), &collision).ok());
}

WXI_TEST(background_flushes_lifecycle_journal_only_after_all_sessions_stop,
         "background_coordinator.lifecycle_flush_order") {
    const std::filesystem::path source =
        std::filesystem::path{__FILE__}.parent_path().parent_path() /
        "src" / "host" / "background_coordinator.cpp";
    std::ifstream stream{source, std::ios::binary};
    WXI_REQUIRE(stream.good());
    const std::string text{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
    const std::size_t run = text.find(
        "const HostExitCode result = RunBackgroundCoordinator(operations)");
    const std::size_t flush = text.find("diagnostic_journal->Flush", run);
    WXI_REQUIRE(run != std::string::npos && flush != std::string::npos);
    WXI_REQUIRE(run < flush);
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

WXI_TEST(background_transient_unsupported_keeps_existing_session,
         "background_coordinator.transient_unsupported_process_keeps_session") {
    Recorder recorder;
    recorder.inputs.push_back({
        {1, {Process(41, true, {Window(0x1000, 1)})}}, false});
    recorder.inputs.push_back({{2, {Process(41, false, {})}}, false});
    recorder.inputs.push_back({
        {3, {Process(41, true, {Window(0x1000, 2)})}}, false});
    StopInput(&recorder);

    WXI_REQUIRE_EQ(
        winexinfo::RunBackgroundCoordinator(recorder.Operations()),
        HostExitCode::Pass);
    WXI_REQUIRE_EQ(
        std::count(recorder.events.begin(), recorder.events.end(), "create:41"), 1);
    WXI_REQUIRE_EQ(
        std::count(recorder.events.begin(), recorder.events.end(), "stop:41"), 1);
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

WXI_TEST(background_production_empty_then_later_window,
         "background_coordinator.production_empty_observation_allows_later_window") {
    winexinfo::BackgroundObserverTracker tracker;
    BackgroundSnapshot empty{};
    const std::vector<winexinfo::ExplorerWindowRecord> none;
    WXI_REQUIRE(winexinfo::ReconcileEmptyProductionBackgroundObservation(
        1, none, &tracker, &empty).ok());
    WXI_REQUIRE(empty.processes.empty());

    const HWND top = reinterpret_cast<HWND>(0x3000);
    const HWND tab = reinterpret_cast<HWND>(0x3100);
    const std::vector windows{winexinfo::ExplorerWindowRecord{top, 51, 21}};
    const std::vector metadata{
        winexinfo::ObserverShellEntryMetadata{3, true, top, tab}};
    const std::vector orders{
        winexinfo::ObserverTopLevelTabOrder{top, {{tab, true}}}};
    const std::vector<DWORD> validated{51};
    BackgroundSnapshot later{};
    WXI_REQUIRE(tracker.Reconcile(
        2, windows, metadata, orders, validated, &later).ok());
    WXI_REQUIRE(later.processes[0].validated);
    WXI_REQUIRE_EQ(later.processes[0].windows[0].top_level_generation, 1u);
}

WXI_TEST(background_production_unsupported_then_supported,
         "background_coordinator.production_all_unsupported_allows_later_supported") {
    winexinfo::BackgroundObserverTracker tracker;
    const HWND top = reinterpret_cast<HWND>(0x4000);
    const HWND tab = reinterpret_cast<HWND>(0x4100);
    const std::vector enumerated{
        winexinfo::ExplorerWindowRecord{top, 61, 31}};
    BackgroundSnapshot unsupported{};
    WXI_REQUIRE(winexinfo::ReconcileEmptyProductionBackgroundObservation(
        1, enumerated, &tracker, &unsupported).ok());
    WXI_REQUIRE_EQ(unsupported.processes.size(), 1u);
    WXI_REQUIRE(!unsupported.processes[0].validated);
    WXI_REQUIRE(unsupported.processes[0].windows.empty());

    const std::vector metadata{
        winexinfo::ObserverShellEntryMetadata{4, true, top, tab}};
    const std::vector orders{
        winexinfo::ObserverTopLevelTabOrder{top, {{tab, true}}}};
    const std::vector<DWORD> validated{61};
    BackgroundSnapshot supported{};
    WXI_REQUIRE(tracker.Reconcile(
        2, enumerated, metadata, orders, validated, &supported).ok());
    WXI_REQUIRE(supported.processes[0].validated);
    WXI_REQUIRE_EQ(supported.processes[0].windows.size(), 1u);
}

}  // namespace
