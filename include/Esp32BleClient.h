#ifndef ESP32_BLE_CLIENT_H
#define ESP32_BLE_CLIENT_H

#ifndef UNIT_TEST

#include "IBleClient.h"
#include <NimBLEDevice.h>
#include "O2RingProtocol.h"

// Backed by NimBLE-Arduino. The original implementation used Bluedroid
// (arduino-esp32's built-in BLE), but `BLEDevice::init()` aborted inside
// `coex_core_enable` whenever WiFi was already associated — reproducibly
// on the SD WIFI PRO hardware, regardless of contiguous heap. NimBLE has
// a different coex integration path and does not trip that failure, and
// it also uses ~60% less RAM. See admin/sleep#133.
class Esp32BleClient : public IBleClient {
public:
    Esp32BleClient();
    ~Esp32BleClient();

    bool connect(const String& namePrefix, uint32_t scanSecs) override;
    bool writeChunked(const uint8_t* data, size_t len) override;
    bool readResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                      uint32_t timeoutMs) override;
    bool requestMtu(uint16_t mtu) override;
    void disconnect() override;
    bool isConnected() const override;
    bool wasDeviceFound() const override { return _lastScanFound; }

    // Bring up the NimBLE stack. Idempotent. Call once at boot (after WiFi)
    // when O2Ring is enabled — FSM-time lazy init fails inside
    // esp_bt_controller_enable once the heap has been fragmented by SMB
    // buffers / web server / etc. See admin/sleep#133.
    static void initStack();

private:
    static bool bleInitialized;

    NimBLEClient* client;
    NimBLERemoteCharacteristic* writeChar;
    NimBLERemoteCharacteristic* notifyChar;
    bool _connected;
    bool _lastScanFound = false;

    // Notification accumulation buffer (used by static callback)
    static uint8_t          _notifyBuf[1024];
    static volatile size_t  _notifyLen;
    static volatile bool    _notifyReady;

    static void notifyCallback(NimBLERemoteCharacteristic* pChar,
                                uint8_t* pData, size_t length, bool isNotify);
};

#endif // UNIT_TEST
#endif // ESP32_BLE_CLIENT_H
