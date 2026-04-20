#ifndef UNIT_TEST

#include "Esp32BleClient.h"
#include "Logger.h"

uint8_t          Esp32BleClient::_notifyBuf[1024];
volatile size_t  Esp32BleClient::_notifyLen  = 0;
volatile bool    Esp32BleClient::_notifyReady = false;

Esp32BleClient::Esp32BleClient()
    : client(nullptr), writeChar(nullptr), notifyChar(nullptr), _connected(false) {}

Esp32BleClient::~Esp32BleClient() {
    disconnect();
}

void Esp32BleClient::notifyCallback(BLERemoteCharacteristic* pChar,
                                     uint8_t* pData, size_t length, bool isNotify) {
    size_t space = sizeof(_notifyBuf) - _notifyLen;
    size_t toCopy = length < space ? length : space;
    memcpy(_notifyBuf + _notifyLen, pData, toCopy);
    _notifyLen += toCopy;

    // Check if we have a complete packet
    if (_notifyLen >= 7) {
        uint16_t dataLen = (uint16_t)_notifyBuf[5] | ((uint16_t)_notifyBuf[6] << 8);
        size_t expected = 7 + dataLen + 1;
        if (_notifyLen >= expected) {
            _notifyReady = true;
        }
    }
}

bool Esp32BleClient::connect(const String& namePrefix, uint32_t scanSecs) {
    static bool bleInitialized = false;
    if (!bleInitialized) {
        BLEDevice::init("");
        bleInitialized = true;
    }
    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    BLEScanResults results = scan->start((int)scanSecs, false);

    BLEAddress targetAddr("");
    bool found = false;
    for (int i = 0; i < results.getCount(); i++) {
        BLEAdvertisedDevice d = results.getDevice(i);
        if (d.haveName() && String(d.getName().c_str()).startsWith(namePrefix)) {
            targetAddr = d.getAddress();
            found = true;
            LOGF("[O2Ring BLE] Found device: %s (%s)",
                 d.getName().c_str(), d.getAddress().toString().c_str());
            break;
        }
    }
    scan->clearResults();
    if (!found) return false;

    client = BLEDevice::createClient();
    if (!client->connect(targetAddr)) {
        LOG_WARN("[O2Ring BLE] Connection failed");
        disconnect();
        return false;
    }

    BLERemoteService* svc = client->getService(O2RingProtocol::SERVICE_UUID);
    if (!svc) {
        LOG_WARN("[O2Ring BLE] Service not found");
        disconnect();
        return false;
    }

    writeChar  = svc->getCharacteristic(O2RingProtocol::WRITE_UUID);
    notifyChar = svc->getCharacteristic(O2RingProtocol::NOTIFY_UUID);
    if (!writeChar || !notifyChar) {
        LOG_WARN("[O2Ring BLE] Characteristics not found");
        disconnect();
        return false;
    }

    notifyChar->registerForNotify(notifyCallback);
    _connected = true;
    LOG("[O2Ring BLE] Connected and subscribed");
    return true;
}

bool Esp32BleClient::writeChunked(const uint8_t* data, size_t len) {
    if (!_connected || !writeChar) return false;
    for (size_t i = 0; i < len; i += 20) {
        size_t chunk = (len - i) < 20 ? (len - i) : 20;
        writeChar->writeValue((uint8_t*)data + i, chunk, false);
        delay(20);
    }
    return true;
}

bool Esp32BleClient::readResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                                   uint32_t timeoutMs) {
    // Caller invariant: one outstanding command at a time (writeChunked → readResponse pairs).
    // Late notifications from a prior command would corrupt this buffer if the invariant is broken.
    _notifyLen   = 0;
    _notifyReady = false;
    unsigned long deadline = millis() + timeoutMs;
    while (!_notifyReady && millis() < deadline) {
        delay(10);
    }
    if (!_notifyReady) { outLen = 0; return false; }
    outLen = _notifyLen < bufCap ? _notifyLen : bufCap;
    memcpy(buffer, _notifyBuf, outLen);
    _notifyLen   = 0;
    _notifyReady = false;
    return true;
}

void Esp32BleClient::disconnect() {
    if (client) {
        if (client->isConnected()) {
            client->disconnect();
        }
        delete client;
        client = nullptr;
    }
    _connected = false;
    writeChar  = nullptr;
    notifyChar = nullptr;
}

bool Esp32BleClient::isConnected() const {
    return _connected && client && client->isConnected();
}

#endif // UNIT_TEST
