#include "test_framework.h"

#include "ipc/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using winexinfo::ipc::DecodedFrame;
using winexinfo::ipc::MessageType;
using winexinfo::ipc::PaneTextResult;
using winexinfo::ipc::PaneTextUpdate;
using winexinfo::ipc::TabDescriptor;
using winexinfo::ipc::TabSetResult;
using winexinfo::ipc::TabSetUpdate;

void RequireProtocolFailure(const winexinfo::Status& status) {
    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::IPC_PROTOCOL_ERROR);
    WXI_REQUIRE(!status.ok());
}

DecodedFrame Decode(const std::vector<std::uint8_t>& bytes) {
    DecodedFrame frame{};
    WXI_REQUIRE(winexinfo::ipc::DecodeFrame(bytes, &frame).ok());
    return frame;
}

}  // namespace

WXI_TEST(ipc_production_tab_set_update_is_byte_exact, "ipc.production.tab_set_bytes") {
    const TabSetUpdate update{
        0x0102030405060708ULL,
        0x1112131415161718ULL,
        {
            {0x2122232425262728ULL, 0x3132333435363738ULL, 0x41424344},
            {0x5152535455565758ULL, 0x6162636465666768ULL, 0x71727374},
        },
    };
    std::vector<std::uint8_t> encoded;
    WXI_REQUIRE(winexinfo::ipc::EncodeTabSetUpdate(9, update, &encoded).ok());
    const std::vector<std::uint8_t> expected{
        'W', 'X', 'I', '1', 1, 0, 6, 0, 60, 0, 0, 0,
        9, 0, 0, 0, 0, 0, 0, 0,
        8, 7, 6, 5, 4, 3, 2, 1,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        2, 0, 0, 0,
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
        0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
        0x44, 0x43, 0x42, 0x41,
        0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51,
        0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61,
        0x74, 0x73, 0x72, 0x71,
    };
    WXI_REQUIRE_EQ(encoded, expected);

    TabSetUpdate decoded{};
    WXI_REQUIRE(winexinfo::ipc::DecodeTabSetUpdate(Decode(encoded), &decoded).ok());
    WXI_REQUIRE_EQ(decoded, update);
}

WXI_TEST(ipc_production_results_are_byte_exact_and_correlated, "ipc.production.result_bytes") {
    const TabSetResult tabResult{0x0102030405060708ULL, 0, ""};
    std::vector<std::uint8_t> encoded;
    WXI_REQUIRE(winexinfo::ipc::EncodeTabSetResult(10, tabResult, &encoded).ok());
    const std::vector<std::uint8_t> expectedTab{
        'W', 'X', 'I', '1', 1, 0, 7, 0, 16, 0, 0, 0,
        10, 0, 0, 0, 0, 0, 0, 0,
        8, 7, 6, 5, 4, 3, 2, 1,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    WXI_REQUIRE_EQ(encoded, expectedTab);
    TabSetResult decodedTab{};
    WXI_REQUIRE(winexinfo::ipc::DecodeTabSetResult(
                    Decode(encoded), 10, 0x0102030405060708ULL, &decodedTab)
                    .ok());
    WXI_REQUIRE_EQ(decodedTab, tabResult);

    const PaneTextResult paneResult{0x1112131415161718ULL, 0x2122232425262728ULL, 7, "STALE"};
    WXI_REQUIRE(winexinfo::ipc::EncodePaneTextResult(11, paneResult, &encoded).ok());
    const std::vector<std::uint8_t> expectedPane{
        'W', 'X', 'I', '1', 1, 0, 9, 0, 29, 0, 0, 0,
        11, 0, 0, 0, 0, 0, 0, 0,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
        7, 0, 0, 0, 5, 0, 0, 0, 'S', 'T', 'A', 'L', 'E',
    };
    WXI_REQUIRE_EQ(encoded, expectedPane);
    PaneTextResult decodedPane{};
    WXI_REQUIRE(winexinfo::ipc::DecodePaneTextResult(
                    Decode(encoded), 11, 0x1112131415161718ULL,
                    0x2122232425262728ULL, &decodedPane)
                    .ok());
    WXI_REQUIRE_EQ(decodedPane, paneResult);
}

WXI_TEST(ipc_production_pane_text_is_byte_exact, "ipc.production.pane_text_bytes") {
    const std::string display{
        'm', 'a', 'i', 'n', ' ', ' ',
        static_cast<char>(0xE2),
        static_cast<char>(0x9C),
        static_cast<char>(0x93),
    };
    const PaneTextUpdate update{1, 2, 3, 4, display, "C:/repo"};
    std::vector<std::uint8_t> encoded;
    WXI_REQUIRE(winexinfo::ipc::EncodePaneTextUpdate(12, update, &encoded).ok());
    const std::vector<std::uint8_t> expected{
        'W', 'X', 'I', '1', 1, 0, 8, 0, 56, 0, 0, 0,
        12, 0, 0, 0, 0, 0, 0, 0,
        1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
        3, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
        9, 0, 0, 0, 'm', 'a', 'i', 'n', ' ', ' ', 0xE2, 0x9C, 0x93,
        7, 0, 0, 0, 'C', ':', '/', 'r', 'e', 'p', 'o',
    };
    WXI_REQUIRE_EQ(encoded, expected);
    PaneTextUpdate decoded{};
    WXI_REQUIRE(winexinfo::ipc::DecodePaneTextUpdate(Decode(encoded), &decoded).ok());
    WXI_REQUIRE_EQ(decoded, update);
}

WXI_TEST(ipc_production_rejects_invalid_tab_sets, "ipc.production.tab_set_rejections") {
    const TabSetUpdate valid{1, 2, {{3, 4, 5}, {6, 7, 8}}};
    std::vector<std::uint8_t> output{0xAA};
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {0, 2, {{3, 4, 5}}}, &output));
    WXI_REQUIRE_EQ(output, std::vector<std::uint8_t>{0xAA});
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 0, {{3, 4, 5}}}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 2, {}}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 2, {{0, 4, 5}}}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 2, {{3, 0, 5}}}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 2, {{3, 4, 0}}}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 2, {{3, 4, 5}, {3, 7, 8}}}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(1, {1, 2, {{3, 4, 5}, {6, 4, 8}}}, &output));

    TabSetUpdate maximum{1, 2, {}};
    maximum.tabs.resize(winexinfo::ipc::kMaximumTabDescriptors);
    for (std::size_t index = 0; index < maximum.tabs.size(); ++index) {
        maximum.tabs[index] = TabDescriptor{index + 1, index + 2, 1};
    }
    WXI_REQUIRE(winexinfo::ipc::EncodeTabSetUpdate(2, maximum, &output).ok());
    maximum.tabs.push_back({5000, 5001, 1});
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetUpdate(2, maximum, &output));

    std::vector<std::uint8_t> encoded;
    WXI_REQUIRE(winexinfo::ipc::EncodeTabSetUpdate(3, valid, &encoded).ok());
    auto frame = Decode(encoded);
    frame.payload.push_back(0);
    TabSetUpdate unchanged{9, 9, {{9, 9, 9}}};
    const auto original = unchanged;
    RequireProtocolFailure(winexinfo::ipc::DecodeTabSetUpdate(frame, &unchanged));
    WXI_REQUIRE_EQ(unchanged, original);

    frame = Decode(encoded);
    for (std::size_t index = 0; index < 8; ++index) {
        frame.payload[40 + index] = frame.payload[20 + index];
    }
    RequireProtocolFailure(winexinfo::ipc::DecodeTabSetUpdate(frame, &unchanged));
    WXI_REQUIRE_EQ(unchanged, original);
}

WXI_TEST(ipc_production_rejects_invalid_text_and_results, "ipc.production.text_result_rejections") {
    std::vector<std::uint8_t> output{0xAA};
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextUpdate(1, {1, 2, 3, 4, "shown", ""}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextUpdate(1, {1, 2, 3, 4, "", "tip"}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextUpdate(1, {1, 2, 3, 4, std::string("A\0B", 3), "tip"}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextUpdate(1, {1, 2, 3, 4, std::string("\xFF", 1), "tip"}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextUpdate(1, {0, 2, 3, 4, "", ""}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetResult(1, {2, 0, "ERROR"}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextResult(1, {2, 3, 7, ""}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodeTabSetResult(1, {0, 0, ""}, &output));
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextResult(1, {2, 0, 0, ""}, &output));

    const std::string maximumDisplay(
        winexinfo::ipc::kMaximumFrameSize - winexinfo::ipc::kFrameHeaderSize - 41,
        'X');
    WXI_REQUIRE(winexinfo::ipc::EncodePaneTextUpdate(
                    2, {1, 2, 3, 4, maximumDisplay, "T"}, &output)
                    .ok());
    WXI_REQUIRE_EQ(output.size(), winexinfo::ipc::kMaximumFrameSize);
    const auto maximumOutput = output;
    RequireProtocolFailure(winexinfo::ipc::EncodePaneTextUpdate(
        2, {1, 2, 3, 4, maximumDisplay + "X", "T"}, &output));
    WXI_REQUIRE_EQ(output, maximumOutput);

    WXI_REQUIRE(winexinfo::ipc::EncodePaneTextUpdate(
                    3, {1, 2, 3, 4, "shown", "tip"}, &output)
                    .ok());
    auto invalidText = Decode(output);
    invalidText.payload[36] = 0xFF;
    PaneTextUpdate textUnchanged{9, 9, 9, 9, "OLD", "OLD"};
    const auto originalText = textUnchanged;
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextUpdate(invalidText, &textUnchanged));
    invalidText.payload[36] = 0;
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextUpdate(invalidText, &textUnchanged));
    invalidText = Decode(output);
    invalidText.payload.push_back(0);
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextUpdate(invalidText, &textUnchanged));
    WXI_REQUIRE_EQ(textUnchanged, originalText);

    WXI_REQUIRE(winexinfo::ipc::EncodeTabSetResult(8, {9, 0, ""}, &output).ok());
    const auto tabFrame = Decode(output);
    TabSetResult tabUnchanged{1, 1, "OLD"};
    const auto originalTab = tabUnchanged;
    RequireProtocolFailure(winexinfo::ipc::DecodeTabSetResult(tabFrame, 7, 9, &tabUnchanged));
    RequireProtocolFailure(winexinfo::ipc::DecodeTabSetResult(tabFrame, 8, 10, &tabUnchanged));
    WXI_REQUIRE_EQ(tabUnchanged, originalTab);

    WXI_REQUIRE(winexinfo::ipc::EncodePaneTextResult(10, {11, 12, 0, ""}, &output).ok());
    const auto paneFrame = Decode(output);
    PaneTextResult paneUnchanged{1, 2, 1, "OLD"};
    const auto originalPane = paneUnchanged;
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextResult(paneFrame, 9, 11, 12, &paneUnchanged));
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextResult(paneFrame, 10, 13, 12, &paneUnchanged));
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextResult(paneFrame, 10, 11, 13, &paneUnchanged));
    WXI_REQUIRE_EQ(paneUnchanged, originalPane);

    auto trailingPaneResult = paneFrame;
    trailingPaneResult.payload.push_back(0);
    RequireProtocolFailure(winexinfo::ipc::DecodePaneTextResult(
        trailingPaneResult, 10, 11, 12, &paneUnchanged));
    WXI_REQUIRE_EQ(paneUnchanged, originalPane);
}

WXI_TEST(ipc_production_rejects_unknown_types_and_stale_generations, "ipc.production.type_generation_rejections") {
    std::vector<std::uint8_t> encoded;
    WXI_REQUIRE(winexinfo::ipc::EncodeTabSetResult(1, {2, 0, ""}, &encoded).ok());
    encoded[6] = 12;
    DecodedFrame decoded{};
    RequireProtocolFailure(winexinfo::ipc::DecodeFrame(encoded, &decoded));

    std::uint64_t last = 0;
    RequireProtocolFailure(winexinfo::ipc::AcceptTopLevelGeneration(0, &last));
    WXI_REQUIRE(winexinfo::ipc::AcceptTopLevelGeneration(4, &last).ok());
    RequireProtocolFailure(winexinfo::ipc::AcceptTopLevelGeneration(4, &last));
    RequireProtocolFailure(winexinfo::ipc::AcceptTopLevelGeneration(3, &last));
    WXI_REQUIRE_EQ(last, std::uint64_t{4});
}

WXI_TEST(ipc_production_window_removal_is_not_tab_update_fallback,
         "ipc.production.authoritative_window_removal") {
    std::vector<std::uint8_t> encoded;
    WXI_REQUIRE(winexinfo::ipc::EncodeWindowRemoveRequest(
                    21, {0x1000, 44}, &encoded).ok());
    const auto requestFrame = Decode(encoded);
    WXI_REQUIRE_EQ(
        requestFrame.message_type,
        winexinfo::ipc::MessageType::WindowRemoveRequest);
    winexinfo::ipc::WindowRemoveRequest request{};
    WXI_REQUIRE(winexinfo::ipc::DecodeWindowRemoveRequest(
                    requestFrame, &request).ok());
    WXI_REQUIRE_EQ(request.top_level_hwnd, std::uint64_t{0x1000});
    WXI_REQUIRE_EQ(request.top_level_generation, std::uint64_t{44});

    WXI_REQUIRE(winexinfo::ipc::EncodeWindowRemoveResult(
                    21, {44, 0, {}}, &encoded).ok());
    const auto resultFrame = Decode(encoded);
    winexinfo::ipc::WindowRemoveResult result{};
    WXI_REQUIRE(winexinfo::ipc::DecodeWindowRemoveResult(
                    resultFrame, 21, 44, &result).ok());
    WXI_REQUIRE_EQ(result.top_level_generation, std::uint64_t{44});

    std::vector<std::uint8_t> tabUpdate;
    WXI_REQUIRE(!winexinfo::ipc::EncodeTabSetUpdate(
                     22, {0x1000, 44, {}}, &tabUpdate).ok());
    WXI_REQUIRE(!winexinfo::ipc::DecodeWindowRemoveResult(
                     resultFrame, 22, 44, &result).ok());
    WXI_REQUIRE(!winexinfo::ipc::DecodeWindowRemoveResult(
                     resultFrame, 21, 45, &result).ok());
}
