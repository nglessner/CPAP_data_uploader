#ifndef O2RING_OXYII_SYNC_H
#define O2RING_OXYII_SYNC_H

#include <Arduino.h>
#include <cstdint>
#include <functional>
#include <vector>

#include "IBleClient.h"
#include "O2RingState.h"

// Outcome of a sync run. Distinct values per state-machine step so the
// dashboard / status card can pinpoint where the failure was.
enum class O2RingSyncResult {
    OK = 0,
    NO_DEVICE_FOUND      = 1,
    CONNECT_FAILED       = 2,
    MTU_FAILED           = 3,
    SUBSCRIBE_FAILED     = 4,   // (reserved — current Esp32BleClient subscribes inside connect())
    AUTH_FAILED          = 5,
    HANDSHAKE_FAILED     = 6,   // 0x10 / 0xC0 / 0x00 sequence
    GET_INFO_FAILED      = 7,
    FILE_LIST_FAILED     = 8,
    FILE_TRANSFER_FAILED = 9,
};

// Run-time configuration for one sync invocation. Kept as a separate
// struct so callers (live firmware vs unit tests) can fill the same fields
// without coupling to Config.
struct OxyIIConfig {
    String   deviceNamePrefix;     // e.g. "T8520"
    uint32_t scanSeconds = 10;
    uint16_t mtu         = 247;
    uint32_t cmdTimeoutMs = 5000;
};

// Orchestrator for the OxyII sync against a Wellue T8520 / O2Ring-S.
//
// One instance per sync attempt. After construction, call run() once; it
// drives the full state machine (scan → connect → MTU → command flow → per-
// file pull → disconnect) and returns a terminal result.
//
// Persistence of pulled file bytes is delegated to onFileComplete: the
// orchestrator buffers each file in RAM (max ≤32 KB observed in the wild)
// and hands the buffer + filename to the callback when 0xF4 arrives. The
// callback returns true on success (file is then marked synced in dedup
// state); false leaves the filename un-synced for the next attempt.
class O2RingOxyIISync {
public:
    using OnFileComplete = std::function<bool(const String& filename,
                                              const uint8_t* data,
                                              size_t len)>;

    O2RingOxyIISync(IBleClient& ble,
                    O2RingState& state,
                    const OxyIIConfig& config,
                    OnFileComplete onFileComplete);

    O2RingSyncResult run();

    // Number of files successfully pulled in the most recent run() call.
    size_t lastSyncedCount() const { return _lastSyncedCount; }

    // Filename of the most recent file pulled (empty if zero pulled).
    const String& lastSyncedFilename() const { return _lastSyncedFilename; }

private:
    IBleClient&       _ble;
    O2RingState&      _state;
    OxyIIConfig       _config;
    OnFileComplete    _onFileComplete;
    uint8_t           _seq = 0;
    uint8_t           _sessionKey[16] = {0};
    bool              _sessionKeyDerived = false;
    size_t            _lastSyncedCount = 0;
    String            _lastSyncedFilename;

    // Scratch buffer for incoming notify frames. NimBLE-Arduino caps notify
    // payloads at MTU - 3, so 514 bytes covers a full chunk plus envelope.
    static constexpr size_t kNotifyBufCap = 600;

    // Send-then-receive a single command. Returns the decoded reply bytes
    // (payload only, NOT the wire envelope) on success, or empty on any
    // failure. seq is bumped automatically.
    bool sendCommand(uint8_t opcode, const uint8_t* payload, size_t payloadLen,
                     uint8_t* outPayload, size_t outCap, size_t& outLen);

    // Like sendCommand but encrypts the payload with _sessionKey before send.
    // Used for 0xFF (AUTH) and 0xF2 (READ_FILE_START).
    bool sendEncryptedCommand(uint8_t opcode, const uint8_t* payload, size_t payloadLen,
                              uint8_t* outPayload, size_t outCap, size_t& outLen);

    // Pull one file via the F2/F3-loop/F4 sequence. Returns true if all bytes
    // arrived and onFileComplete returned true; false on any failure.
    bool pullFile(const String& filename);
};

#endif  // O2RING_OXYII_SYNC_H
