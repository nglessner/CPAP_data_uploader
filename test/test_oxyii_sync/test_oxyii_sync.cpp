// Native unit tests for O2RingOxyIISync — orchestrator state machine.
//
// Drives a MockBleClient with pre-built OxyII frames matching every opcode
// the orchestrator expects. Validates the result enum on every error branch
// and the happy-path file-pull pipeline end-to-end (sans real BLE).

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H
#include "../mocks/MockPreferences.h"
#include "../mocks/MockTime.h"
#include "../mocks/MockBleClient.h"

#include "OxyIIProtocol.h"
#include "OxyIIAes.h"
#include "O2RingOxyIISync.h"

#include "../../src/OxyIIAes.cpp"
#include "../../src/O2RingState.cpp"
#include "../../src/O2RingOxyIISync.cpp"

using OxyIIProtocol::OP_AUTH;
using OxyIIProtocol::OP_KEEPALIVE;
using OxyIIProtocol::OP_SET_UTC_TIME;
using OxyIIProtocol::OP_HANDSHAKE;
using OxyIIProtocol::OP_GET_INFO;
using OxyIIProtocol::OP_GET_FILE_LIST;
using OxyIIProtocol::OP_READ_FILE_START;
using OxyIIProtocol::OP_READ_FILE_DATA;
using OxyIIProtocol::OP_READ_FILE_END;

namespace {

// Build a valid OxyII reply frame for tests. Returns the raw bytes to enqueue
// on MockBleClient. Tests don't care about seq matching — orchestrator
// decodes whatever the mock returns and the seq value is for the ring's
// benefit, not ours.
std::vector<uint8_t> buildFrame(uint8_t opcode, const uint8_t* payload, size_t payloadLen,
                                 uint8_t seq = 0) {
    std::vector<uint8_t> frame(OxyIIProtocol::FRAME_HEADER_LEN + payloadLen + 1);
    OxyIIProtocol::encodeFrame(opcode, payload, payloadLen, seq,
                                frame.data(), frame.size());
    return frame;
}

std::vector<uint8_t> buildEmptyReply(uint8_t opcode) {
    return buildFrame(opcode, nullptr, 0);
}

// Build a 60-byte GET_INFO reply payload with a known serial + firmware.
std::vector<uint8_t> buildGetInfoReply(const char* sn, const char* fw) {
    uint8_t payload[60] = {0};
    // [9..16] firmware ascii
    size_t fwLen = strnlen(fw, 8);
    memcpy(payload + 9, fw, fwLen);
    // [37] sn length, [38..] sn
    size_t snLen = strnlen(sn, 32);
    payload[37] = static_cast<uint8_t>(snLen);
    memcpy(payload + 38, sn, snLen);
    return buildFrame(OP_GET_INFO, payload, sizeof(payload));
}

// Build a GET_FILE_LIST reply with N filenames (each a 14-char timestamp).
std::vector<uint8_t> buildFileListReply(const std::vector<const char*>& names) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(names.size()));
    for (auto* n : names) {
        size_t nameLen = strlen(n);
        if (nameLen > OxyIIProtocol::MAX_FILE_NAME) nameLen = OxyIIProtocol::MAX_FILE_NAME;
        uint8_t slot[OxyIIProtocol::FILE_NAME_SLOT] = {0};
        memcpy(slot, n, nameLen);
        payload.insert(payload.end(), slot, slot + sizeof(slot));
    }
    return buildFrame(OP_GET_FILE_LIST, payload.data(), payload.size());
}

// Set up the canonical opening exchange (everything before GET_FILE_LIST).
// Tests append additional replies (file-list, F2/F3/F4) on top.
void enqueueOpeningExchange(MockBleClient& mock, const char* sn = "T8520TEST",
                             const char* fw = "2D010002") {
    mock.enqueueResponse(buildEmptyReply(OP_AUTH));
    mock.enqueueResponse(buildEmptyReply(OP_KEEPALIVE));
    mock.enqueueResponse(buildEmptyReply(OP_SET_UTC_TIME));
    mock.enqueueResponse(buildEmptyReply(OP_HANDSHAKE));
    mock.enqueueResponse(buildGetInfoReply(sn, fw));
}

OxyIIConfig defaultConfig() {
    OxyIIConfig cfg;
    cfg.deviceNamePrefix = "T8520";
    cfg.scanSeconds = 5;
    cfg.mtu = 247;
    cfg.cmdTimeoutMs = 1000;
    return cfg;
}

}  // namespace

void setUp(void) {
    Preferences::clearAll();
    MockTimeState::reset();
}
void tearDown(void) { Preferences::clearAll(); }

// -------------------- happy paths --------------------

void test_happy_path_one_new_file() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));

    // Per-file: F2 ack, then F3 returns one full chunk (8 bytes) + one shorter
    // chunk (4 bytes) → loop ends on shorter, then F4 ack.
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_START));
    {
        uint8_t chunk1[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk1, sizeof(chunk1)));
    }
    {
        uint8_t chunk2[4] = {0x33, 0x44, 0x55, 0x66};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk2, sizeof(chunk2)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    std::vector<uint8_t> capturedBytes;
    String capturedFilename;
    auto onComplete = [&](const String& fn, const uint8_t* data, size_t len) {
        capturedFilename = fn;
        capturedBytes.assign(data, data + len);
        return true;
    };

    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);
    O2RingSyncResult result = sync.run();

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::OK, (int)result);
    TEST_ASSERT_EQUAL_size_t(1, sync.lastSyncedCount());
    TEST_ASSERT_EQUAL_STRING("20260427213521", sync.lastSyncedFilename().c_str());
    TEST_ASSERT_EQUAL_STRING("20260427213521", capturedFilename.c_str());
    // 8 + 4 bytes pulled
    TEST_ASSERT_EQUAL_size_t(12, capturedBytes.size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, capturedBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x66, capturedBytes[11]);
    // dedup state recorded the filename
    TEST_ASSERT_TRUE(state.hasSeen("20260427213521"));
    // requestMtu called once with 247
    TEST_ASSERT_EQUAL_size_t(1, mock.mtuRequests.size());
    TEST_ASSERT_EQUAL_UINT16(247, mock.mtuRequests[0]);
}

void test_happy_path_no_new_files_returns_ok() {
    MockBleClient mock;
    O2RingState state;
    state.load();
    state.markSeen("20260427213521");
    state.save();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));
    // No F2/F3/F4 expected — file is already synced.

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    O2RingSyncResult result = sync.run();

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::OK, (int)result);
    TEST_ASSERT_EQUAL_size_t(0, sync.lastSyncedCount());
}

void test_dedup_skips_already_synced() {
    MockBleClient mock;
    O2RingState state;
    state.load();
    state.markSeen("20260427100000");  // already synced
    state.save();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427100000", "20260427213521"}));
    // Only the new filename should drive an F2/F3/F4 sequence.
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_START));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_DATA));  // 0-byte chunk = end
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    String pulled;
    auto onComplete = [&](const String& fn, const uint8_t*, size_t) {
        pulled = fn;
        return true;
    };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    O2RingSyncResult result = sync.run();

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::OK, (int)result);
    TEST_ASSERT_EQUAL_size_t(1, sync.lastSyncedCount());
    TEST_ASSERT_EQUAL_STRING("20260427213521", pulled.c_str());
}

// -------------------- error branches --------------------

void test_connect_fails_returns_connect_failed_when_device_found() {
    MockBleClient mock;
    mock.shouldConnect = false;
    mock.deviceFoundFlag = true;
    O2RingState state;
    state.load();

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::CONNECT_FAILED, (int)sync.run());
}

void test_connect_fails_returns_no_device_found_when_scan_empty() {
    MockBleClient mock;
    mock.shouldConnect = false;
    mock.deviceFoundFlag = false;
    O2RingState state;
    state.load();

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NO_DEVICE_FOUND, (int)sync.run());
}

void test_mtu_negotiation_below_target_returns_mtu_failed() {
    MockBleClient mock;
    mock.negotiatedMtu = 23;  // default ATT MTU — same gate that bites real T8520
    O2RingState state;
    state.load();

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::MTU_FAILED, (int)sync.run());
}

void test_auth_reply_missing_returns_auth_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();
    // Don't enqueue any responses — first sendCommand (AUTH) gets nothing.

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::AUTH_FAILED, (int)sync.run());
}

void test_get_info_reply_with_no_serial_returns_get_info_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    mock.enqueueResponse(buildEmptyReply(OP_AUTH));
    mock.enqueueResponse(buildEmptyReply(OP_KEEPALIVE));
    mock.enqueueResponse(buildEmptyReply(OP_SET_UTC_TIME));
    mock.enqueueResponse(buildEmptyReply(OP_HANDSHAKE));
    // GET_INFO reply with sn_len = 0
    mock.enqueueResponse(buildGetInfoReply("", "2D010002"));

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::GET_INFO_FAILED, (int)sync.run());
}

void test_file_list_missing_returns_file_list_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    // No GET_FILE_LIST reply enqueued.

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::FILE_LIST_FAILED, (int)sync.run());
}

void test_file_transfer_disconnect_mid_pull_returns_file_transfer_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_START));
    // First chunk arrives ...
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    // ... but the next F3 read times out (no more responses queued).

    bool callbackInvoked = false;
    auto onComplete = [&](const String&, const uint8_t*, size_t) {
        callbackInvoked = true;
        return true;
    };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::FILE_TRANSFER_FAILED, (int)sync.run());
    TEST_ASSERT_FALSE(callbackInvoked);
    TEST_ASSERT_FALSE(state.hasSeen("20260427213521"));
}

void test_on_file_complete_failure_leaves_filename_unsynced() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_START));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_DATA));  // EOF
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return false; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::FILE_TRANSFER_FAILED, (int)sync.run());
    TEST_ASSERT_FALSE(state.hasSeen("20260427213521"));
}

void test_dedup_pruned_to_ring_list() {
    MockBleClient mock;
    O2RingState state;
    state.load();
    // Old entry that the ring no longer reports
    state.markSeen("20260101000000");
    state.save();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_START));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_DATA));
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    auto onComplete = [&](const String&, const uint8_t*, size_t) { return true; };
    O2RingOxyIISync sync(mock, state, defaultConfig(), onComplete);
    sync.run();

    // After sync: stale entry pruned, current one retained
    TEST_ASSERT_FALSE(state.hasSeen("20260101000000"));
    TEST_ASSERT_TRUE(state.hasSeen("20260427213521"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_one_new_file);
    RUN_TEST(test_happy_path_no_new_files_returns_ok);
    RUN_TEST(test_dedup_skips_already_synced);
    RUN_TEST(test_connect_fails_returns_connect_failed_when_device_found);
    RUN_TEST(test_connect_fails_returns_no_device_found_when_scan_empty);
    RUN_TEST(test_mtu_negotiation_below_target_returns_mtu_failed);
    RUN_TEST(test_auth_reply_missing_returns_auth_failed);
    RUN_TEST(test_get_info_reply_with_no_serial_returns_get_info_failed);
    RUN_TEST(test_file_list_missing_returns_file_list_failed);
    RUN_TEST(test_file_transfer_disconnect_mid_pull_returns_file_transfer_failed);
    RUN_TEST(test_on_file_complete_failure_leaves_filename_unsynced);
    RUN_TEST(test_dedup_pruned_to_ring_list);
    return UNITY_END();
}
