#ifndef O2RING_PROTOCOL_H
#define O2RING_PROTOCOL_H

#include <Arduino.h>
#include <vector>
#include <cstdint>
#include <cstring>

namespace O2RingProtocol {

// BLE GATT UUIDs
static const char* SERVICE_UUID = "14839ac4-7d7e-415c-9a42-167340cf2339";
static const char* NOTIFY_UUID  = "0734594a-a8e7-4b1a-a6b1-cd5243059a57";
static const char* WRITE_UUID   = "8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3";

// Command codes
static const uint8_t CMD_INFO       = 0x14;
static const uint8_t CMD_FILE_OPEN  = 0x03;
static const uint8_t CMD_FILE_READ  = 0x04;
static const uint8_t CMD_FILE_CLOSE = 0x05;
static const uint8_t CMD_CONFIG     = 0x16;

// CRC-8 (Wellue variant) — computed over all packet bytes except the CRC itself
inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t chk = crc ^ data[i];
        crc = 0;
        if (chk & 0x01) crc  = 0x07;
        if (chk & 0x02) crc ^= 0x0e;
        if (chk & 0x04) crc ^= 0x1c;
        if (chk & 0x08) crc ^= 0x38;
        if (chk & 0x10) crc ^= 0x70;
        if (chk & 0x20) crc ^= 0xe0;
        if (chk & 0x40) crc ^= 0xc7;
        if (chk & 0x80) crc ^= 0x89;
    }
    return crc;
}

// Build a request packet into out[]. Returns total packet length.
// out must be at least 8 + dataLen bytes.
inline size_t buildPacket(uint8_t* out, uint8_t cmd, uint16_t block,
                           const uint8_t* data, uint16_t dataLen) {
    out[0] = 0xAA;
    out[1] = cmd;
    out[2] = cmd ^ 0xFF;
    out[3] = (uint8_t)(block & 0xFF);
    out[4] = (uint8_t)((block >> 8) & 0xFF);
    out[5] = (uint8_t)(dataLen & 0xFF);
    out[6] = (uint8_t)((dataLen >> 8) & 0xFF);
    if (data && dataLen > 0) {
        memcpy(out + 7, data, dataLen);
    }
    out[7 + dataLen] = crc8(out, 7 + dataLen);
    return (size_t)(8 + dataLen);
}

// Given the first 7 bytes of a response, return the expected total response length.
inline size_t expectedResponseLen(const uint8_t* header) {
    uint16_t dataLen = (uint16_t)header[5] | ((uint16_t)header[6] << 8);
    return (size_t)(7 + dataLen + 1);
}

// Parse INFO response DATA field (raw JSON bytes) into a list of VLD filenames.
// Returns false if "FileList" key is not found.
inline bool parseInfoFileList(const uint8_t* data, size_t dataLen,
                               std::vector<String>& out) {
    out.clear();
    String json((const char*)data, (unsigned int)dataLen);
    int keyIdx = json.indexOf("\"FileList\":");
    if (keyIdx < 0) return false;
    int quoteStart = json.indexOf('"', (size_t)(keyIdx + 11));
    if (quoteStart < 0) return false;
    int quoteEnd = json.indexOf('"', (size_t)(quoteStart + 1));
    if (quoteEnd < 0) return false;
    String fileList = json.substring((size_t)(quoteStart + 1), (size_t)quoteEnd);
    if (fileList.length() == 0) return true;
    int start = 0;
    while (start < (int)fileList.length()) {
        int comma = fileList.indexOf(',', (size_t)start);
        if (comma < 0) {
            out.push_back(fileList.substring((size_t)start));
            break;
        }
        out.push_back(fileList.substring((size_t)start, (size_t)comma));
        start = comma + 1;
    }
    return true;
}

// Parse FILE_OPEN response DATA field.
// data[0] = status (0 = success). File size at data[6..9] (LE u32).
// Returns false if status != 0 or dataLen < 10.
inline bool parseFileOpenResponse(const uint8_t* data, size_t dataLen, uint32_t& fileSize) {
    if (dataLen < 10) return false;
    if (data[0] != 0) return false;
    fileSize = (uint32_t)data[6]
             | ((uint32_t)data[7] << 8)
             | ((uint32_t)data[8] << 16)
             | ((uint32_t)data[9] << 24);
    return true;
}

// VLD file header (26 bytes at offset 0)
struct VldHeader {
    uint16_t version;       // must be 3
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
    uint16_t durationSecs;
};

// Parse VLD header from first 26 bytes of file data.
// Returns false if bufLen < 26 or version != 3.
inline bool parseVldHeader(const uint8_t* buf, size_t bufLen, VldHeader& h) {
    if (bufLen < 26) return false;
    h.version      = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    h.year         = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    h.month        = buf[4];
    h.day          = buf[5];
    h.hour         = buf[6];
    h.minute       = buf[7];
    h.second       = buf[8];
    h.durationSecs = (uint16_t)buf[18] | ((uint16_t)buf[19] << 8);
    return h.version == 3;
}

// Format {"SetTIME":"YYYY-MM-DD,HH:MM:SS"} into out for CMD_CONFIG payload.
// Caller is responsible for converting time_t → struct tm with the correct
// timezone (firmware uses localtime_r; see ScheduleManager configTime).
// Returns bytes written (33 on success), 0 if outCap is too small or on
// snprintf failure.
inline size_t formatSetTimePayload(const struct tm& tm,
                                    char* out, size_t outCap) {
    int n = snprintf(out, outCap,
        "{\"SetTIME\":\"%04d-%02d-%02d,%02d:%02d:%02d\"}",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return (n < 0 || (size_t)n >= outCap) ? 0 : (size_t)n;
}

} // namespace O2RingProtocol

#endif // O2RING_PROTOCOL_H
