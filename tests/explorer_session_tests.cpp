#include "host/explorer_session.h"

#include "ipc/protocol.h"
#include "test_framework.h"

#include <Windows.h>

#include <cstdint>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <utility>
#include <vector>

namespace {

using winexinfo::ExplorerSession;
using winexinfo::ExplorerSessionOperations;
using winexinfo::SessionWindowSnapshot;
using winexinfo::Status;

Status Ok() { return {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS}; }
Status Fail(winexinfo::ErrorCode code = winexinfo::ErrorCode::PIPE_DISCONNECTED) {
    return {code, S_FALSE, ERROR_GEN_FAILURE};
}

SessionWindowSnapshot Window(
    const std::uintptr_t top,
    const std::uint64_t generation,
    const DWORD tid,
    const std::uintptr_t tab = 0) {
    return {
        reinterpret_cast<HWND>(top),
        generation,
        {{tab == 0 ? top + 0x100 : tab, generation + 100, tid}},
    };
}

struct Recorder final {
    DWORD pid = 41;
    std::vector<std::string> events;
    std::vector<winexinfo::ipc::MessageType> wire_types;
    std::vector<std::uint64_t> wire_request_ids;
    std::vector<std::uint64_t> wire_generations;
    bool fail_exchange = false;
    bool mismatch_request = false;
    bool mismatch_generation = false;
    bool fail_unhook = false;
    bool module_absent = true;
    unsigned drift_after_validation = 0;
    unsigned validation_count = 0;
    unsigned attach_count = 0;
    unsigned pipe_count = 0;
    DWORD exchange_delay_ms = 0;
    std::atomic<unsigned> active_exchanges{0};
    std::atomic<unsigned> maximum_active_exchanges{0};
    std::mutex record_mutex;

    void Event(std::string event) {
        std::scoped_lock lock{record_mutex};
        events.push_back(std::move(event));
    }

    ExplorerSessionOperations Operations();
};

ExplorerSessionOperations Recorder::Operations() {
    return {
        [this](const SessionWindowSnapshot& snapshot,
               winexinfo::injection::HookTarget* output) {
            Event("validate:" + std::to_string(
                reinterpret_cast<std::uintptr_t>(snapshot.top_level)));
            ++validation_count;
            if (output == nullptr || snapshot.tabs.empty()) return Fail();
            *output = {pid, snapshot.tabs.front().ui_thread_id, snapshot.top_level};
            if (drift_after_validation != 0 &&
                validation_count > drift_after_validation) {
                output->ui_thread_id += 1;
            }
            return Ok();
        },
        [this](const winexinfo::injection::HookTarget& target,
               const std::function<Status()>& final_validate,
               winexinfo::injection::HookAttachOutcome* output) {
            Event("attach:" + std::to_string(target.ui_thread_id));
            ++attach_count;
            ++pipe_count;
            if (!final_validate().ok()) return Fail();
            if (output != nullptr) output->unload_authorized = true;
            return Ok();
        },
        [this](const winexinfo::injection::HookTarget& target,
               const std::function<Status()>& final_validate) {
            Event("ensure:" + std::to_string(target.ui_thread_id));
            return final_validate();
        },
        [this](const std::vector<std::uint8_t>& bytes,
               winexinfo::ipc::DecodedFrame* response) {
            Event("exchange");
            const unsigned active = active_exchanges.fetch_add(1) + 1;
            unsigned prior = maximum_active_exchanges.load();
            while (prior < active &&
                   !maximum_active_exchanges.compare_exchange_weak(prior, active)) {}
            if (exchange_delay_ms != 0) Sleep(exchange_delay_ms);
            active_exchanges.fetch_sub(1);
            if (fail_exchange) return Fail();
            winexinfo::ipc::DecodedFrame request{};
            Status status = winexinfo::ipc::DecodeFrame(bytes, &request);
            if (!status.ok()) return status;
            {
                std::scoped_lock lock{record_mutex};
                wire_types.push_back(request.message_type);
                wire_request_ids.push_back(request.request_id);
            }
            std::vector<std::uint8_t> encoded;
            const std::uint64_t response_id = mismatch_request
                ? request.request_id + 1 : request.request_id;
            if (request.message_type == winexinfo::ipc::MessageType::TabSetUpdate) {
                winexinfo::ipc::TabSetUpdate update{};
                status = winexinfo::ipc::DecodeTabSetUpdate(request, &update);
                if (!status.ok()) return status;
                {
                    std::scoped_lock lock{record_mutex};
                    wire_generations.push_back(update.top_level_generation);
                }
                const std::uint64_t result_generation = mismatch_generation
                    ? update.top_level_generation + 1 : update.top_level_generation;
                status = winexinfo::ipc::EncodeTabSetResult(
                    response_id, {result_generation, 0, {}}, &encoded);
            } else if (request.message_type ==
                       winexinfo::ipc::MessageType::WindowRemoveRequest) {
                winexinfo::ipc::WindowRemoveRequest remove{};
                status = winexinfo::ipc::DecodeWindowRemoveRequest(request, &remove);
                if (!status.ok()) return status;
                {
                    std::scoped_lock lock{record_mutex};
                    wire_generations.push_back(remove.top_level_generation);
                }
                const std::uint64_t result_generation = mismatch_generation
                    ? remove.top_level_generation + 1 : remove.top_level_generation;
                status = winexinfo::ipc::EncodeWindowRemoveResult(
                    response_id, {result_generation, 0, {}}, &encoded);
            } else if (request.message_type ==
                       winexinfo::ipc::MessageType::DetachRequest) {
                status = winexinfo::ipc::EncodeDetachResult(
                    response_id, {pid, 0, {}}, &encoded);
            } else {
                return Fail(winexinfo::ErrorCode::IPC_PROTOCOL_ERROR);
            }
            if (!status.ok()) return status;
            return winexinfo::ipc::DecodeFrame(encoded, response);
        },
        [this](const DWORD process_id) {
            Event("unhook:" + std::to_string(process_id));
            return fail_unhook ? Fail(winexinfo::ErrorCode::HOOK_RELEASE_FAILED) : Ok();
        },
        [this](const DWORD process_id, const DWORD timeout, bool* absent) {
            Event("module_absent:" + std::to_string(process_id));
            WXI_REQUIRE_EQ(timeout, DWORD{5000});
            if (absent != nullptr) *absent = module_absent;
            return Ok();
        },
        [this](const DWORD process_id) {
            Event("confirm_gone:" + std::to_string(process_id));
            return Ok();
        },
    };
}

WXI_TEST(explorer_session_one_attach_one_pipe,
         "explorer_session.one_attach_one_pipe_for_multiple_windows") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    const std::vector windows{Window(0x1000, 1, 11), Window(0x2000, 2, 12)};
    WXI_REQUIRE(session.Reconcile(windows).ok());
    WXI_REQUIRE_EQ(recorder.attach_count, 1u);
    WXI_REQUIRE_EQ(recorder.pipe_count, 1u);
    WXI_REQUIRE_EQ(recorder.wire_types.size(), 2u);
    WXI_REQUIRE_EQ(recorder.wire_types[0], winexinfo::ipc::MessageType::TabSetUpdate);
    WXI_REQUIRE_EQ(recorder.wire_types[1], winexinfo::ipc::MessageType::TabSetUpdate);
}

WXI_TEST(explorer_session_formats_machine_parseable_authoritative_diagnostics,
         "explorer_session.lifecycle_diagnostics") {
    const winexinfo::ipc::TabSetUpdate update{
        0x1000, 7, {{0x1100, 9, 11}, {0x1200, 10, 11}}};
    WXI_REQUIRE_EQ(
        winexinfo::FormatLifecycleAttachDiagnostic(
            41, 11, reinterpret_cast<HWND>(0x1000), 0),
        std::string{"LIFECYCLE_EVENT event=attach pid=41 tid=11 hwnd=0x0000000000001000 result=0"});
    WXI_REQUIRE_EQ(
        winexinfo::FormatLifecycleUpdateDiagnostic(
            41, 3, update, {7, 0, {}}),
        std::string{"LIFECYCLE_EVENT event=update pid=41 request=3 hwnd=0x0000000000001000 top_generation=7 tabs=0x0000000000001100:9:11,0x0000000000001200:10:11 result=0"});
    WXI_REQUIRE_EQ(
        winexinfo::FormatLifecycleRemoveDiagnostic(
            41, 4, {0x1000, 7}, {7, 0, {}}),
        std::string{"LIFECYCLE_EVENT event=remove pid=41 request=4 hwnd=0x0000000000001000 top_generation=7 result=0"});
    WXI_REQUIRE_EQ(
        winexinfo::FormatLifecycleDetachDiagnostic(
            41, 5, {41, 0, {}, {0, 0, 0, 0, 0}}),
        std::string{"LIFECYCLE_EVENT event=stop pid=41 request=5 result=0 pane=0 tab_subclass=0 parent_subclass=0 refresh_worker=0 callback=0"});
}

WXI_TEST(explorer_session_shared_tid_deduplicates_lease,
         "explorer_session.shared_tid_uses_no_duplicate_hook_lease") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(
        std::vector{Window(0x1000, 1, 11), Window(0x2000, 2, 11)}).ok());
    unsigned ensures = 0;
    for (const auto& event : recorder.events) {
        if (event == "ensure:11") ++ensures;
    }
    WXI_REQUIRE_EQ(ensures, 2u);
}

WXI_TEST(explorer_session_removes_absent_in_reverse_prior_order,
         "explorer_session.absent_windows_use_authoritative_reverse_removal") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{
        Window(0x1000, 1, 11), Window(0x2000, 2, 12), Window(0x3000, 3, 13)}).ok());
    recorder.wire_types.clear();
    recorder.wire_generations.clear();
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    WXI_REQUIRE_EQ(recorder.wire_types.size(), 3u);
    WXI_REQUIRE_EQ(recorder.wire_types[0], winexinfo::ipc::MessageType::TabSetUpdate);
    WXI_REQUIRE_EQ(recorder.wire_types[1], winexinfo::ipc::MessageType::WindowRemoveRequest);
    WXI_REQUIRE_EQ(recorder.wire_types[2], winexinfo::ipc::MessageType::WindowRemoveRequest);
    WXI_REQUIRE_EQ(recorder.wire_generations[1], 3u);
    WXI_REQUIRE_EQ(recorder.wire_generations[2], 2u);
}

WXI_TEST(explorer_session_rejects_mismatched_update_ack,
         "explorer_session.requires_exact_type7_request_and_generation") {
    Recorder recorder;
    recorder.mismatch_generation = true;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(!session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    const std::size_t event_count = recorder.events.size();
    WXI_REQUIRE(!session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    WXI_REQUIRE_EQ(recorder.events.size(), event_count);
}

WXI_TEST(explorer_session_stop_unhooks_before_detach_and_confirms_absence,
         "explorer_session.stop_reverse_protocol_retains_until_module_absent") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    recorder.events.clear();
    WXI_REQUIRE(session.Stop().ok());
    WXI_REQUIRE_EQ(recorder.events.front(), "unhook:41");
    WXI_REQUIRE_EQ(recorder.events[1], "exchange");
    WXI_REQUIRE_EQ(recorder.events[2], "module_absent:41");
    WXI_REQUIRE_EQ(recorder.events[3], "confirm_gone:41");
}

WXI_TEST(explorer_session_unhook_failure_forbids_detach,
         "explorer_session.unhook_failure_retains_session_without_detach") {
    Recorder recorder;
    recorder.fail_unhook = true;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    recorder.events.clear();
    WXI_REQUIRE(!session.Stop().ok());
    WXI_REQUIRE_EQ(recorder.events.size(), 1u);
    WXI_REQUIRE_EQ(recorder.events[0], "unhook:41");
}

WXI_TEST(explorer_session_removes_last_window,
         "explorer_session.empty_snapshot_removes_all_attached_windows") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    recorder.wire_types.clear();
    const std::vector<SessionWindowSnapshot> none;
    WXI_REQUIRE(session.Reconcile(none).ok());
    WXI_REQUIRE_EQ(recorder.wire_types.size(), 1u);
    WXI_REQUIRE_EQ(
        recorder.wire_types[0],
        winexinfo::ipc::MessageType::WindowRemoveRequest);
}

WXI_TEST(explorer_session_final_validation_requires_exact_target,
         "explorer_session.final_validation_rejects_pid_tid_or_hwnd_drift") {
    Recorder recorder;
    recorder.drift_after_validation = 1;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(!session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    WXI_REQUIRE(recorder.wire_types.empty());
}

WXI_TEST(explorer_session_shared_tid_revalidates_each_window_before_send,
         "explorer_session.shared_tid_drift_is_rejected_before_that_update") {
    Recorder recorder;
    recorder.drift_after_validation = 4;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(!session.Reconcile(
        std::vector{Window(0x1000, 1, 11), Window(0x2000, 2, 11)}).ok());
    WXI_REQUIRE_EQ(recorder.wire_types.size(), 1u);
}

WXI_TEST(explorer_session_different_tid_installs_one_additional_lease,
         "explorer_session.different_tid_installs_one_observation_hook") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(
        std::vector{Window(0x1000, 1, 11), Window(0x2000, 2, 12)}).ok());
    unsigned ensure12 = 0;
    for (const auto& event : recorder.events) {
        if (event == "ensure:12") ++ensure12;
    }
    WXI_REQUIRE_EQ(ensure12, 1u);
    WXI_REQUIRE_EQ(recorder.attach_count, 1u);
}

WXI_TEST(explorer_session_enforces_64_window_bound,
         "explorer_session.accepts_64_and_rejects_65_windows") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    std::vector<SessionWindowSnapshot> windows;
    for (std::uintptr_t index = 0; index < 64; ++index) {
        windows.push_back(Window(0x1000 + index * 0x100, index + 1, 11));
    }
    WXI_REQUIRE(session.Reconcile(windows).ok());
    windows.push_back(Window(0x9000, 65, 11));
    const std::size_t exchanges = recorder.wire_types.size();
    WXI_REQUIRE(!session.Reconcile(windows).ok());
    WXI_REQUIRE_EQ(recorder.wire_types.size(), exchanges);
}

WXI_TEST(explorer_session_rejects_stale_generation_before_transport,
         "explorer_session.stale_generation_does_not_touch_pipe") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 7, 11)}).ok());
    const std::size_t exchanges = recorder.wire_types.size();
    WXI_REQUIRE(!session.Reconcile(std::vector{Window(0x1000, 6, 11)}).ok());
    WXI_REQUIRE_EQ(recorder.wire_types.size(), exchanges);
}

WXI_TEST(explorer_session_remove_ack_mismatch_poison_retains,
         "explorer_session.removal_ack_requires_exact_request_and_generation") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 7, 11)}).ok());
    recorder.mismatch_request = true;
    const std::vector<SessionWindowSnapshot> none;
    WXI_REQUIRE(!session.Reconcile(none).ok());
    recorder.events.clear();
    WXI_REQUIRE(!session.Stop().ok());
    WXI_REQUIRE(recorder.events.empty());
}

WXI_TEST(explorer_session_module_absence_can_be_rechecked_without_redetach,
         "explorer_session.module_presence_retains_then_rechecks_only_absence") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    recorder.module_absent = false;
    WXI_REQUIRE(!session.Stop().ok());
    recorder.module_absent = true;
    recorder.events.clear();
    WXI_REQUIRE(session.Stop().ok());
    WXI_REQUIRE_EQ(recorder.events.size(), 2u);
    WXI_REQUIRE_EQ(recorder.events[0], "module_absent:41");
    WXI_REQUIRE_EQ(recorder.events[1], "confirm_gone:41");
}

WXI_TEST(explorer_session_serializes_reconcile_calls,
         "explorer_session.concurrent_reconcile_has_one_pipe_exchange") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    const std::vector windows{Window(0x1000, 1, 11)};
    WXI_REQUIRE(session.Reconcile(windows).ok());
    recorder.maximum_active_exchanges.store(0);
    recorder.exchange_delay_ms = 25;
    Status first = Fail();
    Status second = Fail();
    std::thread one([&] { first = session.Reconcile(windows); });
    std::thread two([&] { second = session.Reconcile(windows); });
    one.join();
    two.join();
    WXI_REQUIRE(first.ok());
    WXI_REQUIRE(second.ok());
    WXI_REQUIRE_EQ(recorder.maximum_active_exchanges.load(), 1u);
}

WXI_TEST(explorer_session_restart_reuses_one_attach_and_exact_new_generation,
         "explorer_session.add_remove_restart_keeps_one_process_session") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    const std::vector<SessionWindowSnapshot> none;
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    WXI_REQUIRE(session.Reconcile(none).ok());
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 2, 11)}).ok());
    WXI_REQUIRE_EQ(recorder.attach_count, 1u);
    WXI_REQUIRE_EQ(recorder.pipe_count, 1u);
    WXI_REQUIRE_EQ(recorder.wire_types.size(), 3u);
    WXI_REQUIRE_EQ(
        recorder.wire_types[1],
        winexinfo::ipc::MessageType::WindowRemoveRequest);
    WXI_REQUIRE_EQ(recorder.wire_generations[2], std::uint64_t{2});
}

WXI_TEST(explorer_session_transport_failure_poison_retains,
         "explorer_session.transport_failure_retains_without_stop_cleanup") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    WXI_REQUIRE(session.Reconcile(std::vector{Window(0x1000, 1, 11)}).ok());
    recorder.fail_exchange = true;
    WXI_REQUIRE(!session.Reconcile(std::vector{Window(0x1000, 2, 11)}).ok());
    recorder.events.clear();
    WXI_REQUIRE(!session.Stop().ok());
    WXI_REQUIRE(recorder.events.empty());
}

WXI_TEST(explorer_session_serializes_reconcile_against_stop,
         "explorer_session.stop_waits_for_inflight_reconcile") {
    Recorder recorder;
    ExplorerSession session{recorder.pid, recorder.Operations()};
    const std::vector windows{Window(0x1000, 1, 11)};
    WXI_REQUIRE(session.Reconcile(windows).ok());
    recorder.events.clear();
    recorder.exchange_delay_ms = 25;
    Status reconciled = Fail();
    Status stopped = Fail();
    std::thread reconcile([&] { reconciled = session.Reconcile(windows); });
    while (recorder.active_exchanges.load() == 0) Sleep(1);
    std::thread stop([&] { stopped = session.Stop(); });
    reconcile.join();
    stop.join();
    WXI_REQUIRE(reconciled.ok());
    WXI_REQUIRE(stopped.ok());
    const auto update = std::find(
        recorder.events.begin(), recorder.events.end(), "exchange");
    const auto unhook = std::find(
        recorder.events.begin(), recorder.events.end(), "unhook:41");
    WXI_REQUIRE(update != recorder.events.end());
    WXI_REQUIRE(unhook != recorder.events.end());
    WXI_REQUIRE(update < unhook);
}

}  // namespace
