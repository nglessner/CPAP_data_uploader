#include <unity.h>
#include <string>
#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H
#include "../mocks/MockPreferences.h"
#include "../mocks/MockBleClient.h"
#include "../mocks/MockWiFi.h"
#include "../mocks/MockTime.h"

#include "O2RingProtocol.h"
#include "O2RingState.h"
#include "../../src/O2RingState.cpp"
#include "O2RingStatus.h"
#include "../../src/O2RingStatus.cpp"
#include "O2RingSync.h"
#include "../../src/O2RingSync.cpp"

// Minimal Config stub for tests
#include "Config.h"
#include "../../src/Config.cpp"

MockBleClient* mockBle;
Config* cfg;
fs::FS mockSD;

// Helper: build a well-formed response packet
std::vector<uint8_t> makeResponse(uint8_t cmd, uint16_t block,
                                   const uint8_t* data, uint16_t dataLen) {
    std::vector<uint8_t> pkt(8 + dataLen);
    O2RingProtocol::buildPacket(pkt.data(), cmd, block, data, dataLen);
    return pkt;
}

// Build an empty CMD_CONFIG ack response packet that the mock can serve
// when O2RingSync sends SetTIME. makeResponse uses buildPacket which writes
// 0xAA as the start byte, but the orchestrator does not validate the start
// byte — it only reads cmd at index 1, dataLen at indexes 5/6, and data at
// index 7+. Empty data → 8-byte packet with cmd=0x16 at index 1.
static std::vector<uint8_t> makeSetTimeAck() {
    return makeResponse(O2RingProtocol::CMD_CONFIG, 0, nullptr, 0);
}

void setUp(void) {
    Preferences::clearAll();
    MockTimeState::reset();
    mockSD.clear();
    mockBle = new MockBleClient();
    cfg = new Config();
    // Minimal valid config
    std::string configStr =
        "WIFI_SSID = Net\n"
        "WIFI_PASSWORD = pass\n"
        "ENDPOINT = //192.168.0.108/share\n"
        "ENDPOINT_TYPE = SMB\n"
        "ENDPOINT_USER = u\n"
        "ENDPOINT_PASSWORD = p\n"
        "O2RING_ENABLED = true\n"
        "O2RING_DEVICE_NAME = O2Ring\n"
        "O2RING_PATH = oximetry/raw\n"
        "O2RING_SCAN_SECONDS = 5\n";
    mockSD.addFile("/config.txt", configStr);
    cfg->loadFromSD(mockSD);
}

void tearDown(void) {
    delete mockBle;
    delete cfg;
    Preferences::clearAll();
}

void test_device_not_found_returns_error() {
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = false;  // genuine empty-scan
    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::DEVICE_NOT_FOUND, (int)result);
}

void test_nothing_to_sync_when_all_seen() {
    // INFO response with one file
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    mockBle->enqueueResponse(makeSetTimeAck());

    // Pre-mark the file as seen
    O2RingState state;
    state.load();
    state.markSeen("20260116233312.vld");
    state.save();

    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC, (int)result);
}

void test_info_command_sent_on_connect() {
    // INFO response
    const char* json = R"({"CurBAT":"75%","FileList":"","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));

    O2RingSync sync(cfg, mockBle);
    sync.run();

    // First written packet must be CMD_INFO. Assert against writeHistory[0]
    // so subsequent writes (e.g., SetTIME after INFO) don't clobber the
    // assertion target.
    TEST_ASSERT_GREATER_THAN(0, (int)mockBle->writeHistory.size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, mockBle->writeHistory[0][0]);
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_INFO, mockBle->writeHistory[0][1]);
}

void test_stale_seen_entries_pruned_after_info() {
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    mockBle->enqueueResponse(makeSetTimeAck());

    {
        O2RingState state;
        state.load();
        state.markSeen("20260116233312.vld");
        state.markSeen("20250101000000.vld");
        state.markSeen("20250202000000.vld");
        state.save();
    }

    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC, (int)result);

    O2RingState reloaded;
    reloaded.load();
    TEST_ASSERT_TRUE(reloaded.hasSeen("20260116233312.vld"));
    TEST_ASSERT_FALSE(reloaded.hasSeen("20250101000000.vld"));
    TEST_ASSERT_FALSE(reloaded.hasSeen("20250202000000.vld"));
}

void test_status_recorded_on_device_not_found() {
    MockTimeState::setTime(1777000000);
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = false;  // genuine empty-scan

    O2RingSync sync(cfg, mockBle);
    sync.run();

    O2RingStatus status;
    status.load();
    TEST_ASSERT_TRUE(status.hasData());
    TEST_ASSERT_EQUAL_UINT32(1777000000, status.getLastUnix());
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::DEVICE_NOT_FOUND,
                          status.getLastResult());
    TEST_ASSERT_EQUAL_UINT(0, status.getFilesSynced());
    // No INFO response, so no filename should be set on a fresh record
    TEST_ASSERT_EQUAL_STRING("", status.getLastFilename().c_str());
}

void test_status_records_filename_on_nothing_to_sync() {
    MockTimeState::setTime(1777035170);
    const char* json = R"({"CurBAT":"75%","FileList":"20260115221045.vld,20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    mockBle->enqueueResponse(makeSetTimeAck());

    // Pre-mark both files as already seen so we get NOTHING_TO_SYNC
    O2RingState state;
    state.load();
    state.markSeen("20260115221045.vld");
    state.markSeen("20260116233312.vld");
    state.save();

    O2RingSync sync(cfg, mockBle);
    sync.run();

    O2RingStatus status;
    status.load();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC,
                          status.getLastResult());
    // lexicographic max of the FileList
    TEST_ASSERT_EQUAL_STRING("20260116233312.vld",
                             status.getLastFilename().c_str());
}

void test_status_preserves_filename_when_pre_info_failure() {
    // Prime a previous successful observation
    {
        MockTimeState::setTime(1777000000);
        O2RingStatus s;
        s.load();
        s.record((int)O2RingSyncResult::NOTHING_TO_SYNC, 0,
                 "20260116233312.vld");
        s.save();
    }
    // Now run a sync that fails before INFO
    MockTimeState::setTime(1777035170);
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = false;  // simulate genuine empty-scan to keep DEVICE_NOT_FOUND assertion

    O2RingSync sync(cfg, mockBle);
    sync.run();

    O2RingStatus status;
    status.load();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::DEVICE_NOT_FOUND,
                          status.getLastResult());
    TEST_ASSERT_EQUAL_STRING("20260116233312.vld",
                             status.getLastFilename().c_str());
}

void test_connect_failed_returns_error_when_scan_hit_but_connect_failed() {
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = true;  // scan saw the device, GATT rejected
    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::CONNECT_FAILED, (int)result);
}

void test_status_recorded_on_connect_failed() {
    MockTimeState::setTime(1777035100);
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = true;

    O2RingSync sync(cfg, mockBle);
    sync.run();

    O2RingStatus status;
    status.load();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::CONNECT_FAILED,
                          status.getLastResult());
    TEST_ASSERT_EQUAL_UINT32(1777035100u, status.getLastUnix());
    TEST_ASSERT_EQUAL_UINT16(0u, status.getFilesSynced());
}

void test_set_time_sent_after_info_success() {
    // INFO returns one file already in the dedup state, so we end on
    // NOTHING_TO_SYNC after SetTIME completes.
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    mockBle->enqueueResponse(makeSetTimeAck());

    {
        O2RingState state;
        state.load();
        state.markSeen("20260116233312.vld");
        state.save();
    }

    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC, (int)result);

    // Two packets must have been written: INFO first, SetTIME second.
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, (int)mockBle->writeHistory.size());
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_INFO,
                            mockBle->writeHistory[0][1]);
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_CONFIG,
                            mockBle->writeHistory[1][1]);

    // Payload (bytes 7..end-1, after the 7-byte header and before CRC)
    // must contain the "SetTIME" key.
    auto& pkt = mockBle->writeHistory[1];
    TEST_ASSERT_GREATER_THAN(8, (int)pkt.size());
    std::string payload((const char*)(pkt.data() + 7), pkt.size() - 8);
    TEST_ASSERT_TRUE(payload.find("\"SetTIME\":") != std::string::npos);
}

void test_set_time_failure_does_not_abort() {
    // INFO returns one file already in dedup state. NO SetTIME ack is
    // enqueued, so the orchestrator's wait will time out at the mock
    // (returns false from readResponse with empty queue). Sync must still
    // complete with NOTHING_TO_SYNC, not BLE_ERROR.
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    // Intentionally no makeSetTimeAck() enqueued.

    {
        O2RingState state;
        state.load();
        state.markSeen("20260116233312.vld");
        state.save();
    }

    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC, (int)result);

    // SetTIME write must still have happened — only the response was missing.
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, (int)mockBle->writeHistory.size());
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_CONFIG,
                            mockBle->writeHistory[1][1]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_device_not_found_returns_error);
    RUN_TEST(test_nothing_to_sync_when_all_seen);
    RUN_TEST(test_info_command_sent_on_connect);
    RUN_TEST(test_stale_seen_entries_pruned_after_info);
    RUN_TEST(test_status_recorded_on_device_not_found);
    RUN_TEST(test_status_records_filename_on_nothing_to_sync);
    RUN_TEST(test_status_preserves_filename_when_pre_info_failure);
    RUN_TEST(test_connect_failed_returns_error_when_scan_hit_but_connect_failed);
    RUN_TEST(test_status_recorded_on_connect_failed);
    RUN_TEST(test_set_time_sent_after_info_success);
    RUN_TEST(test_set_time_failure_does_not_abort);
    return UNITY_END();
}
