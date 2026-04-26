#ifndef IBLE_CLIENT_H
#define IBLE_CLIENT_H

#include <Arduino.h>
#include <cstdint>

class IBleClient {
public:
    virtual ~IBleClient() = default;

    // Scan for a device whose advertised name starts with namePrefix.
    // Returns true if connected within scanSecs seconds.
    virtual bool connect(const String& namePrefix, uint32_t scanSecs) = 0;

    // Write data to the write characteristic, chunked at 20 bytes.
    virtual bool writeChunked(const uint8_t* data, size_t len) = 0;

    // Accumulate notify responses until a complete packet is received or timeout.
    // Writes raw response (full packet including header and CRC) into buffer.
    // outLen is set to the number of bytes written.
    virtual bool readResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                              uint32_t timeoutMs) = 0;

    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // Returns true iff the most recent connect() observed a name-prefix
    // match in scan results — regardless of whether the subsequent GATT
    // steps succeeded. Reset to false at the start of each connect()
    // call. Callers query it only after connect() returned false to
    // distinguish empty-scan from post-scan failure.
    virtual bool wasDeviceFound() const = 0;
};

#endif // IBLE_CLIENT_H
