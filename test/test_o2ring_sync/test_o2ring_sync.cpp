#include <unity.h>
#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H
#include "../mocks/MockPreferences.h"
#include "../mocks/MockBleClient.h"
#include "../mocks/MockWiFi.h"

#include "O2RingProtocol.h"
#include "O2RingState.h"
#include "../../src/O2RingState.cpp"
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

void setUp(void) {
    Preferences::clearAll();
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
    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::DEVICE_NOT_FOUND, (int)result);
}

void test_nothing_to_sync_when_all_seen() {
    // INFO response with one file
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));

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

    // First byte of lastWritten should be 0xAA and CMD should be CMD_INFO
    TEST_ASSERT_GREATER_THAN(0, (int)mockBle->lastWritten.size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, mockBle->lastWritten[0]);
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_INFO, mockBle->lastWritten[1]);
}

void test_stale_seen_entries_pruned_after_info() {
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));

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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_device_not_found_returns_error);
    RUN_TEST(test_nothing_to_sync_when_all_seen);
    RUN_TEST(test_info_command_sent_on_connect);
    RUN_TEST(test_stale_seen_entries_pruned_after_info);
    return UNITY_END();
}
