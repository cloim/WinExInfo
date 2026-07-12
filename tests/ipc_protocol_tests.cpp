#include "test_framework.h"

#include "common/win32_handle.h"
#include "ipc/named_pipe.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using winexinfo::ipc::AttachResult;
using winexinfo::ipc::DecodedFrame;
using winexinfo::ipc::DetachResult;
using winexinfo::ipc::MessageType;

std::vector<std::uint8_t> Bytes(
    const std::initializer_list<std::uint8_t> values) {
    return {values};
}

void RequireProtocolFailure(const winexinfo::Status& status) {
    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::IPC_PROTOCOL_ERROR);
    WXI_REQUIRE(!status.ok());
}

std::wstring UniquePipeSuffix() {
    return L"ipc-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
}

}  // namespace

WXI_TEST(ipc_detach_request_is_byte_exact, "ipc.detach_request_bytes") {
    std::vector<std::uint8_t> frame;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachRequest(0x0102030405060708ULL, &frame).ok());
    const std::vector expected = Bytes({
        'W', 'X', 'I', '1',
        0x01, 0x00,
        0x03, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    });
    WXI_REQUIRE_EQ(frame, expected);

    DecodedFrame decoded{};
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(frame, &decoded).ok());
    WXI_REQUIRE_EQ(decoded.message_type, MessageType::DetachRequest);
    WXI_REQUIRE_EQ(decoded.request_id, 0x0102030405060708ULL);
    WXI_REQUIRE(decoded.payload.empty());
    WXI_REQUIRE(winexinfo::ipc::DecodeDetachRequest(decoded).ok());
}

WXI_TEST(ipc_attach_result_is_byte_exact, "ipc.attach_result_bytes") {
    const AttachResult result{0x11223344, 0x55667788, 0x0102030405060708ULL, 0, ""};
    std::vector<std::uint8_t> frame;
    WXI_REQUIRE(winexinfo::ipc::EncodeAttachResult(9, result, &frame).ok());
    const std::vector expected = Bytes({
        'W', 'X', 'I', '1', 0x01, 0x00, 0x04, 0x00,
        0x18, 0x00, 0x00, 0x00,
        0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x44, 0x33, 0x22, 0x11,
        0x88, 0x77, 0x66, 0x55,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    });
    WXI_REQUIRE_EQ(frame, expected);

    DecodedFrame decoded{};
    AttachResult roundTrip{};
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(frame, &decoded).ok());
    WXI_REQUIRE(winexinfo::ipc::DecodeAttachResult(decoded, &roundTrip).ok());
    WXI_REQUIRE_EQ(roundTrip, result);
}

WXI_TEST(ipc_detach_result_is_byte_exact, "ipc.detach_result_bytes") {
    const DetachResult result{
        0x11223344,
        7,
        "PIPE_DISCONNECTED",
        {1, 2, 3, 4, 5},
    };
    std::vector<std::uint8_t> frame;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachResult(11, result, &frame).ok());
    DecodedFrame decoded{};
    DetachResult roundTrip{};
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(frame, &decoded).ok());
    WXI_REQUIRE_EQ(decoded.message_type, MessageType::DetachResult);
    WXI_REQUIRE(winexinfo::ipc::DecodeDetachResult(decoded, 11, &roundTrip).ok());
    WXI_REQUIRE_EQ(roundTrip, result);

    const std::vector<std::uint8_t> expectedPayload{
        0x44, 0x33, 0x22, 0x11,
        0x07, 0x00, 0x00, 0x00,
        0x11, 0x00, 0x00, 0x00,
        'P','I','P','E','_','D','I','S','C','O','N','N','E','C','T','E','D',
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,
    };
    WXI_REQUIRE_EQ(decoded.payload, expectedPayload);
}

WXI_TEST(ipc_successful_detach_requires_authoritative_zero_cleanup,
         "ipc.detach_result_cleanup_proof") {
    for (std::size_t index = 0; index < 5; ++index) {
        DetachResult result{42, 0, {}, {0, 0, 0, 0, 0}};
        auto* counts = reinterpret_cast<std::uint32_t*>(&result.cleanup);
        counts[index] = 1;
        std::vector<std::uint8_t> frame;
        WXI_REQUIRE(!winexinfo::ipc::EncodeDetachResult(1, result, &frame).ok());
    }
}

WXI_TEST(ipc_rejects_header_and_length_mismatches, "ipc.header_mismatches") {
    std::vector<std::uint8_t> valid;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachRequest(1, &valid).ok());
    DecodedFrame output{};

    auto wrongMagic = valid;
    wrongMagic[0] = 'Q';
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(wrongMagic, &output));
    auto wrongVersion = valid;
    wrongVersion[4] = 2;
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(wrongVersion, &output));
    auto wrongType = valid;
    wrongType[6] = 2;
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(wrongType, &output));
    auto truncated = valid;
    truncated.pop_back();
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(truncated, &output));
    auto trailing = valid;
    trailing.push_back(0);
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(trailing, &output));
    auto oversized = valid;
    oversized[8] = 0xED;
    oversized[9] = 0xFF;
    oversized[10] = 0x03;
    oversized[11] = 0x00;
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(oversized, &output));
}

WXI_TEST(ipc_rejects_payload_shape_and_result_mismatches, "ipc.payload_mismatches") {
    std::vector<std::uint8_t> frame;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachRequest(1, &frame).ok());
    frame[8] = 1;
    frame.push_back(0);
    DecodedFrame decoded{};
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(frame, &decoded).ok());
    RequireProtocolFailure(winexinfo::ipc::DecodeDetachRequest(decoded));

    AttachResult attach{};
    std::vector<std::uint8_t> invalidUtf8;
    WXI_REQUIRE(winexinfo::ipc::EncodeAttachResult(
                    3,
                    AttachResult{1, 2, 3, 7, "X"},
                    &invalidUtf8)
                    .ok());
    invalidUtf8.back() = 0xFF;
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(invalidUtf8, &decoded).ok());
    RequireProtocolFailure(winexinfo::ipc::DecodeAttachResult(decoded, &attach));

    auto embeddedNul = invalidUtf8;
    embeddedNul.back() = 0;
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(embeddedNul, &decoded).ok());
    RequireProtocolFailure(winexinfo::ipc::DecodeAttachResult(decoded, &attach));

    std::vector<std::uint8_t> successWithError;
    RequireProtocolFailure(winexinfo::ipc::EncodeAttachResult(
        1, AttachResult{1, 2, 3, 0, "ERROR"}, &successWithError));
    std::vector<std::uint8_t> failureWithoutError;
    RequireProtocolFailure(winexinfo::ipc::EncodeDetachResult(
        1, DetachResult{1, 5, ""}, &failureWithoutError));
}

WXI_TEST(ipc_enforces_detach_id_monotonicity, "ipc.detach_id_sequence") {
    std::uint64_t last = 0;
    RequireProtocolFailure(winexinfo::ipc::AcceptDetachId(0, &last));
    WXI_REQUIRE(winexinfo::ipc::AcceptDetachId(4, &last).ok());
    WXI_REQUIRE_EQ(last, std::uint64_t{4});
    RequireProtocolFailure(winexinfo::ipc::AcceptDetachId(4, &last));
    RequireProtocolFailure(winexinfo::ipc::AcceptDetachId(3, &last));
    WXI_REQUIRE_EQ(last, std::uint64_t{4});

    std::vector<std::uint8_t> frame;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachResult(
                    8, DetachResult{1, 0, ""}, &frame)
                    .ok());
    DecodedFrame decoded{};
    DetachResult result{};
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(frame, &decoded).ok());
    RequireProtocolFailure(winexinfo::ipc::DecodeDetachResult(decoded, 9, &result));
}

WXI_TEST(ipc_pipe_is_local_byte_mode_and_current_user_only, "ipc.pipe_security") {
    WXI_REQUIRE((winexinfo::ipc::kControllerPipeMode & PIPE_REJECT_REMOTE_CLIENTS) != 0);
    WXI_REQUIRE((winexinfo::ipc::kControllerPipeMode & PIPE_TYPE_MESSAGE) == 0);
    WXI_REQUIRE((winexinfo::ipc::kControllerPipeMode & PIPE_READMODE_MESSAGE) == 0);

    std::wstring pipeName;
    WXI_REQUIRE(winexinfo::ipc::BuildCurrentUserPipeName(&pipeName).ok());
    WXI_REQUIRE(pipeName.starts_with(L"\\\\.\\pipe\\WinExInfo.v1.S-1-"));
    pipeName += L"." + UniquePipeSuffix();

    winexinfo::UniqueHandle server;
    WXI_REQUIRE(winexinfo::ipc::CreateControllerPipeServer(pipeName, &server).ok());
    winexinfo::ipc::PipeSecurityInspection inspection{};
    WXI_REQUIRE(winexinfo::ipc::InspectPipeSecurity(server.get(), &inspection).ok());
    WXI_REQUIRE(inspection.current_user_present);
    WXI_REQUIRE(!inspection.unexpected_principal_present);
    WXI_REQUIRE(inspection.byte_type);
    WXI_REQUIRE(inspection.byte_read_mode);
}

WXI_TEST(ipc_pipe_security_rejects_missing_or_broad_acl, "ipc.pipe_security_rejections") {
    const winexinfo::ipc::PipeSecurityInspection exact{true, false, true, true};
    WXI_REQUIRE(winexinfo::ipc::ValidatePipeSecurityInspection(exact).ok());
    auto missingCurrentUser = exact;
    missingCurrentUser.current_user_present = false;
    RequireProtocolFailure(
        winexinfo::ipc::ValidatePipeSecurityInspection(missingCurrentUser));
    auto broadPrincipal = exact;
    broadPrincipal.unexpected_principal_present = true;
    RequireProtocolFailure(
        winexinfo::ipc::ValidatePipeSecurityInspection(broadPrincipal));
    auto messageMode = exact;
    messageMode.byte_type = false;
    RequireProtocolFailure(
        winexinfo::ipc::ValidatePipeSecurityInspection(messageMode));
}

WXI_TEST(ipc_protocol_error_closes_without_resynchronizing, "ipc.no_resynchronization") {
    std::wstring pipeName;
    WXI_REQUIRE(winexinfo::ipc::BuildCurrentUserPipeName(&pipeName).ok());
    pipeName += L"." + UniquePipeSuffix();
    winexinfo::UniqueHandle server;
    winexinfo::UniqueHandle client;
    WXI_REQUIRE(winexinfo::ipc::CreateLocalPipePair(pipeName, &server, &client).ok());

    std::vector<std::uint8_t> malformed;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachRequest(1, &malformed).ok());
    malformed[0] = 'Q';
    std::vector<std::uint8_t> valid;
    WXI_REQUIRE(winexinfo::ipc::EncodeDetachRequest(2, &valid).ok());
    std::vector combined = malformed;
    combined.insert(combined.end(), valid.begin(), valid.end());
    DWORD written = 0;
    WXI_REQUIRE(WriteFile(
                    client.get(),
                    combined.data(),
                    static_cast<DWORD>(combined.size()),
                    &written,
                    nullptr) != FALSE);
    WXI_REQUIRE_EQ(written, static_cast<DWORD>(combined.size()));

    DecodedFrame decoded{};
    RequireProtocolFailure(winexinfo::ipc::ReadFrame(&server, &decoded));
    WXI_REQUIRE(!server);
    WXI_REQUIRE_EQ(decoded.request_id, std::uint64_t{0});
}

WXI_TEST(ipc_pipe_names_are_isolated_per_explorer_process,
         "ipc.pipe_name.per_process_isolation") {
    std::wstring first;
    std::wstring second;
    WXI_REQUIRE(winexinfo::ipc::BuildCurrentUserPipeNameForProcess(41, &first).ok());
    WXI_REQUIRE(winexinfo::ipc::BuildCurrentUserPipeNameForProcess(42, &second).ok());
    WXI_REQUIRE(first != second);
    WXI_REQUIRE(first.ends_with(L".41"));
    WXI_REQUIRE(second.ends_with(L".42"));
}
