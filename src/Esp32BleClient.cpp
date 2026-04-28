#if defined(ENABLE_O2RING_SYNC) && !defined(UNIT_TEST)

#include "Esp32BleClient.h"
#include "Logger.h"
#include "OxyIIProtocol.h"

uint8_t          Esp32BleClient::_notifyBuf[1024];
volatile size_t  Esp32BleClient::_notifyLen  = 0;
volatile bool    Esp32BleClient::_notifyReady = false;
bool             Esp32BleClient::bleInitialized = false;

void Esp32BleClient::initStack() {
    if (bleInitialized) return;
    LOGF("[O2Ring BLE] NimBLEDevice::init (free=%u, max_alloc=%u)",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    NimBLEDevice::init("");
    // OxyII (T8520) requires ATT MTU >= 247 before file-transfer commands are
    // accepted. Set the device-wide preferred MTU here; NimBLE auto-exchanges
    // during connect, and requestMtu() later verifies the negotiated value.
    NimBLEDevice::setMTU(247);
    bleInitialized = true;
    LOGF("[O2Ring BLE] NimBLE stack ready (free=%u, max_alloc=%u)",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

// Match the four OxyII-mode signatures pull_v2.py uses, in priority order.
// The OxyII-mode advertisement rarely carries the OxyII service UUID; matching
// only on that (the previous behavior) misses the ring almost every time.
// Returns a static label naming which signature hit, or nullptr if none did.
static const char* matchOxyIISignature(NimBLEAdvertisedDevice& d, const NimBLEUUID& target) {
    // 1. Random Static address with prefix F3:7F:ED (post-reset address)
    String addr = d.getAddress().toString().c_str();
    addr.toUpperCase();
    if (addr.startsWith("F3:7F:ED")) return "addr-prefix";
    // 2. Manufacturer ID 0xF34E (OxyII-mode marker)
    if (d.haveManufacturerData()) {
        std::string m = d.getManufacturerData();
        if (m.size() >= 2) {
            uint16_t cid = (uint8_t)m[0] | ((uint16_t)(uint8_t)m[1] << 8);
            if (cid == 0xF34E) return "manuf-id";
        }
    }
    // 3. Local name starting with "S8-AW"
    if (d.haveName()) {
        std::string name = d.getName();
        if (name.rfind("S8-AW", 0) == 0) return "name-prefix";
    }
    // 4. OxyII service UUID in advertisement (rare but cheap)
    if (d.isAdvertisingService(target)) return "service-uuid";
    return nullptr;
}

Esp32BleClient::Esp32BleClient()
    : client(nullptr), writeChar(nullptr), notifyChar(nullptr), _connected(false) {}

Esp32BleClient::~Esp32BleClient() {
    disconnect();
}

void Esp32BleClient::notifyCallback(NimBLERemoteCharacteristic* pChar,
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

bool Esp32BleClient::connect(const String& serviceUuid, uint32_t scanSecs) {
    _lastScanFound = false;
    initStack();  // idempotent; lazy fallback if boot-time init was skipped
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults results = scan->start((uint32_t)scanSecs, false);

    NimBLEUUID target(serviceUuid.c_str());
    NimBLEAddress targetAddr;
    bool found = false;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        const char* via = matchOxyIISignature(d, target);
        if (via) {
            targetAddr = d.getAddress();
            found = true;
            _lastScanFound = true;
            LOGF("[O2Ring BLE] Found OxyII ring via %s: name=%s addr=%s rssi=%d",
                 via,
                 d.haveName() ? d.getName().c_str() : "(unnamed)",
                 d.getAddress().toString().c_str(),
                 d.haveRSSI() ? d.getRSSI() : 0);
            break;
        }
    }
    scan->clearResults();
    if (!found) return false;

    client = NimBLEDevice::createClient();
    if (!client->connect(targetAddr)) {
        LOG_WARN("[O2Ring BLE] Connection failed");
        disconnect();
        return false;
    }

    NimBLERemoteService* svc = client->getService(OxyIIProtocol::SERVICE_UUID());
    if (!svc) {
        LOG_WARN("[O2Ring BLE] Service not found");
        disconnect();
        return false;
    }

    writeChar  = svc->getCharacteristic(OxyIIProtocol::WRITE_UUID());
    notifyChar = svc->getCharacteristic(OxyIIProtocol::NOTIFY_UUID());
    if (!writeChar || !notifyChar) {
        LOG_WARN("[O2Ring BLE] Characteristics not found");
        disconnect();
        return false;
    }

    // NimBLE v1.4 registerForNotify signature:
    //   registerForNotify(notify_callback, bool notifications=true, bool response=false)
    if (!notifyChar->registerForNotify(notifyCallback)) {
        LOG_WARN("[O2Ring BLE] registerForNotify failed");
        disconnect();
        return false;
    }
    _connected = true;
    LOG("[O2Ring BLE] Connected and subscribed");
    return true;
}

bool Esp32BleClient::requestMtu(uint16_t mtu) {
    if (!_connected || !client) return false;
    // NimBLE auto-exchanges MTU on connect using the device-wide preference
    // set in initStack(). This call verifies the negotiated value meets the
    // caller's requirement; if not, the caller should treat it as a hard
    // failure (T8520 silently rejects file commands at sub-247 MTU).
    uint16_t negotiated = client->getMTU();
    LOG("[O2Ring BLE] Negotiated MTU: " + String(negotiated));
    return negotiated >= mtu;
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
        // NimBLE-Arduino manages client lifetime via NimBLEDevice::deleteClient().
        NimBLEDevice::deleteClient(client);
        client = nullptr;
    }
    _connected = false;
    writeChar  = nullptr;
    notifyChar = nullptr;
}

bool Esp32BleClient::isConnected() const {
    return _connected && client && client->isConnected();
}

#endif // ENABLE_O2RING_SYNC && !UNIT_TEST
