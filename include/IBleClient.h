#ifndef IBLE_CLIENT_H
#define IBLE_CLIENT_H

#include <Arduino.h>
#include <cstdint>

class IBleClient {
public:
    virtual ~IBleClient() = default;

    // Scan for a device advertising the given GATT service UUID and connect
    // to the first match. Returns true if connected within scanSecs seconds.
    //
    // Filtering by service UUID (rather than advertised name) is robust to
    // the T8520's two advertising modes (T8520_<mac> when idle vs. S8-AW
    // <serial> when in OxyII mode) and to firmware-rev name changes; both
    // modes still expose the same E8FB0001-... service.
    virtual bool connect(const String& serviceUuid, uint32_t scanSecs) = 0;

    // Write data to the write characteristic, chunked at 20 bytes.
    virtual bool writeChunked(const uint8_t* data, size_t len) = 0;

    // Accumulate notify responses until a complete packet is received or timeout.
    // Writes raw response (full packet including header and CRC) into buffer.
    // outLen is set to the number of bytes written.
    virtual bool readResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                              uint32_t timeoutMs) = 0;

    // Negotiate ATT MTU on the active connection. Returns true iff the
    // negotiated MTU is at least `mtu`. Required by OxyII (T8520): commands
    // 0xF2/0xF3/0xF4 are silently rejected when default MTU (23) is in
    // effect — the firmware uses MTU as a state flag for file transfer.
    // Caller invokes after connect() and before any file-transfer command.
    virtual bool requestMtu(uint16_t mtu) = 0;

    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // Returns true iff the most recent connect() observed a service-UUID
    // match in scan results — regardless of whether the subsequent GATT
    // steps succeeded. Reset to false at the start of each connect()
    // call. Callers query it only after connect() returned false to
    // distinguish empty-scan from post-scan failure.
    virtual bool wasDeviceFound() const = 0;
};

#endif // IBLE_CLIENT_H
