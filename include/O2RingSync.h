#ifndef O2RING_SYNC_H
#define O2RING_SYNC_H

#include <Arduino.h>
#include "Config.h"
#include "IBleClient.h"
#include "O2RingState.h"

#ifdef ENABLE_SMB_UPLOAD
#include "SMBUploader.h"
#endif

enum class O2RingSyncResult {
    OK,
    DEVICE_NOT_FOUND,
    SMB_ERROR,
    BLE_ERROR,
    NOTHING_TO_SYNC
};

class O2RingSync {
public:
    O2RingSync(Config* cfg, IBleClient* ble);

    O2RingSyncResult run();

private:
    Config* config;
    IBleClient* ble;
    O2RingState state;

    bool sendCommand(uint8_t cmd, uint16_t block, const uint8_t* data, uint16_t dataLen);
    bool receiveResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                         uint32_t timeoutMs = 5000);

    bool downloadAndUpload(const String& filename);
};

#endif // O2RING_SYNC_H
