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
#include "O2RingFileSink.h"
#include "O2RingOxyIISync.h"

// In-memory sink for tests. Captures filename + concatenated chunks so the
// original assertion patterns (filename, byte count, byte values) keep
// working under the streaming contract. Knobs let tests simulate failure
// at begin, at writeChunk, or at finalize.
class MockFileSink : public O2RingFileSink {
public:
    bool failBegin = false;
    int  failChunkAtIndex = -1;   // -1 = never fail; 0 = fail on first chunk; etc.

    String              capturedFilename;
    uint32_t            capturedTotalSize = 0;
    std::vector<uint8_t> capturedBytes;
    int                 chunkCount = 0;
    bool                beginCalled = false;
    bool                finalizeCalled = false;
    bool                lastFinalizeOk = false;

    bool begin(const String& filename, uint32_t totalSize) override {
        beginCalled = true;
        if (failBegin) return false;
        capturedFilename = filename;
        capturedTotalSize = totalSize;
        capturedBytes.clear();
        chunkCount = 0;
        return true;
    }
    bool writeChunk(const uint8_t* data, size_t len) override {
        if (failChunkAtIndex == chunkCount) {
            chunkCount++;
            return false;
        }
        capturedBytes.insert(capturedBytes.end(), data, data + len);
        chunkCount++;
        return true;
    }
    void finalize(bool ok) override {
        finalizeCalled = true;
        lastFinalizeOk = ok;
    }
};

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

// READ_FILE_START reply payload is u32-LE total file size. The orchestrator
// uses this to bound the F3 loop and pre-allocate the destination buffer.
std::vector<uint8_t> buildReadFileStartReply(uint32_t advertisedSize) {
    uint8_t payload[4] = {
        static_cast<uint8_t>(advertisedSize & 0xFF),
        static_cast<uint8_t>((advertisedSize >> 8) & 0xFF),
        static_cast<uint8_t>((advertisedSize >> 16) & 0xFF),
        static_cast<uint8_t>((advertisedSize >> 24) & 0xFF),
    };
    return buildFrame(OP_READ_FILE_START, payload, sizeof(payload));
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
//
// NOTE: AUTH (0xFF) is sent fire-and-forget — the ring never replies to it
// (confirmed across pair / sync / our snoops; see oxyii_protocol.py and
// pull_v2.py). So we DO NOT enqueue an AUTH reply here.
void enqueueOpeningExchange(MockBleClient& mock, const char* sn = "T8520TEST",
                             const char* fw = "2D010002") {
    mock.enqueueResponse(buildEmptyReply(OP_KEEPALIVE));
    mock.enqueueResponse(buildEmptyReply(OP_SET_UTC_TIME));
    mock.enqueueResponse(buildEmptyReply(OP_HANDSHAKE));
    mock.enqueueResponse(buildGetInfoReply(sn, fw));
}

OxyIIConfig defaultConfig() {
    OxyIIConfig cfg;
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

// -------------------- scan filter --------------------

void test_orchestrator_filters_scan_by_oxyii_service_uuid() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({}));  // empty file list

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);
    sync.run();

    // Service UUID must be the canonical OxyII one — robust to T8520_/S8-AW
    // advertising-name modes.
    TEST_ASSERT_EQUAL_STRING(OxyIIProtocol::SERVICE_UUID(),
                             mock.lastConnectFilter.c_str());
}

// -------------------- happy paths --------------------

void test_happy_path_one_new_file() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));

    // Per-file: F2 advertises size = 12, then F3 returns one 8-byte chunk +
    // one 4-byte chunk; loop terminates when offset reaches advertised size.
    mock.enqueueResponse(buildReadFileStartReply(12));
    {
        uint8_t chunk1[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk1, sizeof(chunk1)));
    }
    {
        uint8_t chunk2[4] = {0x33, 0x44, 0x55, 0x66};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk2, sizeof(chunk2)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);
    O2RingSyncResult result = sync.run();

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::OK, (int)result);
    TEST_ASSERT_EQUAL_size_t(1, sync.lastSyncedCount());
    TEST_ASSERT_EQUAL_STRING("20260427213521", sync.lastSyncedFilename().c_str());
    TEST_ASSERT_EQUAL_STRING("20260427213521", sink.capturedFilename.c_str());
    TEST_ASSERT_EQUAL_UINT32(12, sink.capturedTotalSize);
    // 8 + 4 bytes pulled — chunks streamed individually
    TEST_ASSERT_EQUAL_size_t(12, sink.capturedBytes.size());
    TEST_ASSERT_EQUAL_INT(2, sink.chunkCount);
    TEST_ASSERT_EQUAL_UINT8(0xAA, sink.capturedBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x66, sink.capturedBytes[11]);
    TEST_ASSERT_TRUE(sink.finalizeCalled);
    TEST_ASSERT_TRUE(sink.lastFinalizeOk);
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

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

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
    mock.enqueueResponse(buildReadFileStartReply(4));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    O2RingSyncResult result = sync.run();

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::OK, (int)result);
    TEST_ASSERT_EQUAL_size_t(1, sync.lastSyncedCount());
    TEST_ASSERT_EQUAL_STRING("20260427213521", sink.capturedFilename.c_str());
}

// -------------------- error branches --------------------

void test_connect_fails_returns_connect_failed_when_device_found() {
    MockBleClient mock;
    mock.shouldConnect = false;
    mock.deviceFoundFlag = true;
    O2RingState state;
    state.load();

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::CONNECT_FAILED, (int)sync.run());
}

void test_connect_fails_returns_no_device_found_when_scan_empty() {
    MockBleClient mock;
    mock.shouldConnect = false;
    mock.deviceFoundFlag = false;
    O2RingState state;
    state.load();

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NO_DEVICE_FOUND, (int)sync.run());
}

void test_mtu_negotiation_below_target_returns_mtu_failed() {
    MockBleClient mock;
    mock.negotiatedMtu = 23;  // default ATT MTU — same gate that bites real T8520
    O2RingState state;
    state.load();

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::MTU_FAILED, (int)sync.run());
}

void test_auth_no_reply_proceeds_to_keepalive() {
    // AUTH (0xFF) is fire-and-forget — the ring never sends an AUTH reply.
    // With no responses queued at all, the orchestrator should successfully
    // send AUTH (no reply expected) and then fail at the KEEPALIVE step
    // which DOES expect a reply. This pins the new contract: AUTH never
    // returns AUTH_FAILED for "missing reply" because no reply is ever
    // expected.
    MockBleClient mock;
    O2RingState state;
    state.load();

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::HANDSHAKE_FAILED, (int)sync.run());
    // Confirm AUTH was actually written despite no reply.
    TEST_ASSERT_TRUE(mock.writeHistory.size() >= 2);
    TEST_ASSERT_EQUAL_HEX8(OxyIIProtocol::OP_AUTH, mock.writeHistory[0][1]);
    TEST_ASSERT_EQUAL_HEX8(OxyIIProtocol::OP_KEEPALIVE, mock.writeHistory[1][1]);
}

void test_get_info_reply_with_no_serial_returns_get_info_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    // AUTH is fire-and-forget — no reply enqueued.
    mock.enqueueResponse(buildEmptyReply(OP_KEEPALIVE));
    mock.enqueueResponse(buildEmptyReply(OP_SET_UTC_TIME));
    mock.enqueueResponse(buildEmptyReply(OP_HANDSHAKE));
    // GET_INFO reply with sn_len = 0
    mock.enqueueResponse(buildGetInfoReply("", "2D010002"));

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::GET_INFO_FAILED, (int)sync.run());
}

void test_file_list_missing_returns_file_list_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    // No GET_FILE_LIST reply enqueued.

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::FILE_LIST_FAILED, (int)sync.run());
}

void test_file_transfer_disconnect_mid_pull_returns_file_transfer_failed() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));
    // F2 advertises 8 bytes; F3 returns only the first 4, then queue is
    // empty so the next F3 fetch times out before reaching advertised size.
    mock.enqueueResponse(buildReadFileStartReply(8));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    // ... but the next F3 read times out (no more responses queued).

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::FILE_TRANSFER_FAILED, (int)sync.run());
    // Sink was opened (begin called) and a chunk was forwarded, but the
    // disconnect mid-pull aborted before the F4 close — finalize must
    // still fire with ok=false so the partial write is cleaned up.
    TEST_ASSERT_TRUE(sink.beginCalled);
    TEST_ASSERT_TRUE(sink.finalizeCalled);
    TEST_ASSERT_FALSE(sink.lastFinalizeOk);
    TEST_ASSERT_FALSE(state.hasSeen("20260427213521"));
}

void test_on_file_complete_failure_leaves_filename_unsynced() {
    MockBleClient mock;
    O2RingState state;
    state.load();

    enqueueOpeningExchange(mock);
    mock.enqueueResponse(buildFileListReply({"20260427213521"}));
    mock.enqueueResponse(buildReadFileStartReply(4));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    // Streaming-equivalent of "callback returned false": sink fails on
    // the first chunk, simulating a downstream-write error.
    MockFileSink sink;
    sink.failChunkAtIndex = 0;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);

    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::FILE_TRANSFER_FAILED, (int)sync.run());
    TEST_ASSERT_TRUE(sink.finalizeCalled);
    TEST_ASSERT_FALSE(sink.lastFinalizeOk);
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
    mock.enqueueResponse(buildReadFileStartReply(4));
    {
        uint8_t chunk[4] = {1, 2, 3, 4};
        mock.enqueueResponse(buildFrame(OP_READ_FILE_DATA, chunk, sizeof(chunk)));
    }
    mock.enqueueResponse(buildEmptyReply(OP_READ_FILE_END));

    MockFileSink sink;
    O2RingOxyIISync sync(mock, state, defaultConfig(), sink);
    sync.run();

    // After sync: stale entry pruned, current one retained
    TEST_ASSERT_FALSE(state.hasSeen("20260101000000"));
    TEST_ASSERT_TRUE(state.hasSeen("20260427213521"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_orchestrator_filters_scan_by_oxyii_service_uuid);
    RUN_TEST(test_happy_path_one_new_file);
    RUN_TEST(test_happy_path_no_new_files_returns_ok);
    RUN_TEST(test_dedup_skips_already_synced);
    RUN_TEST(test_connect_fails_returns_connect_failed_when_device_found);
    RUN_TEST(test_connect_fails_returns_no_device_found_when_scan_empty);
    RUN_TEST(test_mtu_negotiation_below_target_returns_mtu_failed);
    RUN_TEST(test_auth_no_reply_proceeds_to_keepalive);
    RUN_TEST(test_get_info_reply_with_no_serial_returns_get_info_failed);
    RUN_TEST(test_file_list_missing_returns_file_list_failed);
    RUN_TEST(test_file_transfer_disconnect_mid_pull_returns_file_transfer_failed);
    RUN_TEST(test_on_file_complete_failure_leaves_filename_unsynced);
    RUN_TEST(test_dedup_pruned_to_ring_list);
    return UNITY_END();
}
