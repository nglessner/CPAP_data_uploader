#ifndef ESP32_BLE_CLIENT_H
#define ESP32_BLE_CLIENT_H

#ifndef UNIT_TEST

#include "IBleClient.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>
#include <BLEAdvertisedDevice.h>
#include "O2RingProtocol.h"

class Esp32BleClient : public IBleClient {
public:
    Esp32BleClient();
    ~Esp32BleClient();

    bool connect(const String& namePrefix, uint32_t scanSecs) override;
    bool writeChunked(const uint8_t* data, size_t len) override;
    bool readResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                      uint32_t timeoutMs) override;
    void disconnect() override;
    bool isConnected() const override;

private:
    BLEClient* client;
    BLERemoteCharacteristic* writeChar;
    BLERemoteCharacteristic* notifyChar;
    bool _connected;

    // Notification accumulation buffer (used by static callback)
    static uint8_t _notifyBuf[1024];
    static size_t  _notifyLen;
    static bool    _notifyReady;

    static void notifyCallback(BLERemoteCharacteristic* pChar,
                                uint8_t* pData, size_t length, bool isNotify);
};

#endif // UNIT_TEST
#endif // ESP32_BLE_CLIENT_H
