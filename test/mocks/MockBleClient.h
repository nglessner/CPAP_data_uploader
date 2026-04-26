#ifndef MOCK_BLE_CLIENT_H
#define MOCK_BLE_CLIENT_H

#ifdef UNIT_TEST

#include "IBleClient.h"

// Arduino.h defines min/max as macros that conflict with std::deque template
// instantiations (used by std::queue). Undefine them after Arduino.h is pulled
// in so that the STL internals can compile cleanly.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <vector>
#include <queue>

class MockBleClient : public IBleClient {
public:
    bool shouldConnect = true;
    bool connected = false;
    // Default true: a mock with shouldConnect=true represents a found
    // and connected device. Tests that simulate genuine "scan miss"
    // must set this to false explicitly.
    bool deviceFoundFlag = true;

    // Queue of responses to return from readResponse(), in order.
    std::queue<std::vector<uint8_t>> responses;

    // Last packet written via writeChunked (for assertions).
    std::vector<uint8_t> lastWritten;

    // Full chronological history of writeChunked() calls. Each entry is the
    // bytes of one packet. Lets tests assert on packets that lastWritten
    // would otherwise have been overwritten by subsequent commands.
    std::vector<std::vector<uint8_t>> writeHistory;

    bool connect(const String& namePrefix, uint32_t scanSecs) override {
        connected = shouldConnect;
        return connected;
    }

    bool writeChunked(const uint8_t* data, size_t len) override {
        lastWritten.assign(data, data + len);
        writeHistory.emplace_back(data, data + len);
        return true;
    }

    bool readResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                      uint32_t timeoutMs) override {
        if (responses.empty()) { outLen = 0; return false; }
        auto& resp = responses.front();
        outLen = resp.size();
        if (outLen > bufCap) outLen = bufCap;
        memcpy(buffer, resp.data(), outLen);
        responses.pop();
        return true;
    }

    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    bool wasDeviceFound() const override { return deviceFoundFlag; }

    // Helper: enqueue a raw response packet
    void enqueueResponse(const std::vector<uint8_t>& resp) {
        responses.push(resp);
    }
};

#endif // UNIT_TEST
#endif // MOCK_BLE_CLIENT_H
