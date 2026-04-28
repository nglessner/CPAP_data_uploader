#ifndef OXYII_PROTOCOL_H
#define OXYII_PROTOCOL_H

// OxyII application-layer protocol for Wellue T8520 / O2Ring-S.
//
// Pure-C++ inline functions; no Arduino, FreeRTOS, or ESP-IDF deps. Builds in
// the native test env and on the ESP32. Mirrors scripts/o2r_pair/oxyii_protocol.py
// in the admin/sleep repo, validated against captured ViHealth-app snoop frames.
//
// AES-128/ECB/PKCS7 wrap/unwrap is intentionally NOT in this header — encryption
// is per-command and integrates with mbedtls on-device, so it lives in a separate
// translation unit gated by the platform. The frame codec (CRC + envelope) is
// fully testable on the host, which is what's covered here.
//
// Frame format (request and response share opcode):
//
//     +------+-----+------+------+-----+--------+--------+----------+-----+
//     | 0xA5 | cmd | ~cmd | flag | seq | len_lo | len_hi | payload  | crc |
//     +------+-----+------+------+-----+--------+--------+----------+-----+
//        1     1     1      1     1     1        1        len bytes   1
//
//   - flag is 0x00 in normal app->device commands.
//   - len is little-endian payload byte count.
//   - crc is CRC-8/poly 0x07/init 0/no reflection/no xorout, computed over
//     the entire frame except the trailing crc byte itself.

#include <cstdint>
#include <cstring>

namespace OxyIIProtocol {

// BLE GATT UUIDs (NOT the legacy 14839ac4 OxyII; T8520 uses these)
inline const char* SERVICE_UUID() { return "e8fb0001-a14b-98f9-831b-4e2941d01248"; }
inline const char* WRITE_UUID()   { return "e8fb0002-a14b-98f9-831b-4e2941d01248"; }
inline const char* NOTIFY_UUID()  { return "e8fb0003-a14b-98f9-831b-4e2941d01248"; }

// Opcodes — extracted from doab/dof.java dispatch table in the lepu-blepro AAR
constexpr uint8_t OP_AUTH               = 0xFF;
constexpr uint8_t OP_KEEPALIVE          = 0x10;
constexpr uint8_t OP_SET_UTC_TIME       = 0xC0;
constexpr uint8_t OP_HANDSHAKE          = 0x00;
constexpr uint8_t OP_GET_INFO           = 0xE1;
constexpr uint8_t OP_GET_BATTERY        = 0xE4;
constexpr uint8_t OP_GET_FILE_LIST      = 0xF1;
constexpr uint8_t OP_READ_FILE_START    = 0xF2;
constexpr uint8_t OP_READ_FILE_DATA     = 0xF3;
constexpr uint8_t OP_READ_FILE_END      = 0xF4;

constexpr uint8_t FRAME_LEAD = 0xA5;
constexpr size_t  FRAME_HEADER_LEN = 7;
constexpr size_t  AES_BLOCK_SIZE = 16;
constexpr size_t  AES_KEY_SIZE   = 16;

// CRC-8/poly=0x07, init=0, no reflection. Caller passes the frame minus the
// trailing CRC byte (i.e. compute, append, send).
inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

// Build a complete request frame into out[]. Returns total byte count, or 0 on
// invalid input (out too small for header + payload + crc). out must be at
// least FRAME_HEADER_LEN + payloadLen + 1 bytes.
inline size_t encodeFrame(uint8_t opcode, const uint8_t* payload, size_t payloadLen,
                          uint8_t seq, uint8_t* out, size_t outCap) {
    if (payloadLen > 0xFFFF) return 0;
    size_t total = FRAME_HEADER_LEN + payloadLen + 1;
    if (outCap < total) return 0;

    out[0] = FRAME_LEAD;
    out[1] = opcode;
    out[2] = static_cast<uint8_t>(~opcode);
    out[3] = 0x00; // flag
    out[4] = seq;
    out[5] = static_cast<uint8_t>(payloadLen & 0xFF);
    out[6] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    if (payload && payloadLen > 0) {
        memcpy(out + FRAME_HEADER_LEN, payload, payloadLen);
    }
    out[FRAME_HEADER_LEN + payloadLen] = crc8(out, FRAME_HEADER_LEN + payloadLen);
    return total;
}

// Decode result from validating an incoming notify frame.
struct DecodedFrame {
    bool     ok;
    uint8_t  opcode;
    uint8_t  seq;
    size_t   payloadOffset;   // index into the input frame where payload begins
    size_t   payloadLen;
    uint8_t  errorCode;       // populated when ok == false; see DecodeError below
};

enum DecodeError : uint8_t {
    DECODE_OK             = 0,
    DECODE_TOO_SHORT      = 1,
    DECODE_BAD_LEAD       = 2,
    DECODE_BAD_COMPLEMENT = 3,
    DECODE_BAD_LENGTH     = 4,
    DECODE_BAD_CRC        = 5,
};

inline DecodedFrame decodeFrame(const uint8_t* frame, size_t frameLen) {
    DecodedFrame d{};
    if (frameLen < FRAME_HEADER_LEN + 1) {
        d.errorCode = DECODE_TOO_SHORT;
        return d;
    }
    if (frame[0] != FRAME_LEAD) {
        d.errorCode = DECODE_BAD_LEAD;
        return d;
    }
    uint8_t op = frame[1];
    if (frame[2] != static_cast<uint8_t>(~op)) {
        d.errorCode = DECODE_BAD_COMPLEMENT;
        return d;
    }
    uint16_t length = static_cast<uint16_t>(frame[5]) |
                      (static_cast<uint16_t>(frame[6]) << 8);
    size_t expectedTotal = FRAME_HEADER_LEN + length + 1;
    if (frameLen != expectedTotal) {
        d.errorCode = DECODE_BAD_LENGTH;
        return d;
    }
    uint8_t expectedCrc = crc8(frame, expectedTotal - 1);
    if (frame[expectedTotal - 1] != expectedCrc) {
        d.errorCode = DECODE_BAD_CRC;
        return d;
    }
    d.ok = true;
    d.opcode = op;
    d.seq = frame[4];
    d.payloadOffset = FRAME_HEADER_LEN;
    d.payloadLen = length;
    return d;
}

// Even-indexed bytes of MD5("lepucloud") — Wellue's well-known salt. These are
// the constant inputs to the session-key derivation; the AAR computes the MD5
// once at module load. Pre-computed here to avoid pulling mbedtls into the
// pure-protocol layer.
inline const uint8_t* lepucloudSaltEvenBytes() {
    // MD5("lepucloud") = c2 a7 cf 50 da fe d8 85 a8 f8 f7 ea c4 43 35 f3
    // Even indices [0,2,4,6,8,10,12,14] = c2 cf da d8 a8 f7 c4 35
    static const uint8_t kSalt[8] = {
        0xc2, 0xcf, 0xda, 0xd8, 0xa8, 0xf7, 0xc4, 0x35
    };
    return kSalt;
}

// Compute the 16-byte AES session key per Wellue's doaj.doaf.doa(String).
//
// Layout:
//   [0..7]   = even-indexed bytes of MD5("lepucloud")
//   [8..11]  = first 4 ASCII chars of `serial`
//   [12..15] = timestamp shifted by 0,1,2,3 — NOT byte-extract — per the AAR's
//              literal `(byte)(l2 >> n3)` for n3 = 0..3.
//
// Returns false when serial is shorter than 4 chars.
inline bool deriveSessionKey(const char* serial, size_t serialLen,
                             uint64_t timestampSeconds, uint8_t outKey[AES_KEY_SIZE]) {
    if (serialLen < 4) return false;
    const uint8_t* salt = lepucloudSaltEvenBytes();
    for (int i = 0; i < 8; i++) outKey[i] = salt[i];
    for (int i = 0; i < 4; i++) outKey[8 + i] = static_cast<uint8_t>(serial[i]);
    for (int n = 0; n < 4; n++) {
        outKey[12 + n] = static_cast<uint8_t>((timestampSeconds >> n) & 0xFF);
    }
    return true;
}

// Build the READ_FILE_START (0xF2) payload: a 20-byte buffer with a 16-byte
// null-padded ASCII filename slot followed by a u32 LE file_type field.
// Returns false when outCap < 20 or filenameLen > 16.
inline bool buildReadFileStartPayload(const char* filename, size_t filenameLen,
                                       uint8_t fileType, uint8_t* out, size_t outCap) {
    if (outCap < 20 || filenameLen > 16) return false;
    memset(out, 0, 20);
    memcpy(out, filename, filenameLen);
    out[16] = fileType;
    out[17] = 0;
    out[18] = 0;
    out[19] = 0;
    return true;
}

// Build the READ_FILE_DATA (0xF3) payload: a single u32 LE offset.
inline bool buildReadFileDataPayload(uint32_t offset, uint8_t* out, size_t outCap) {
    if (outCap < 4) return false;
    out[0] = static_cast<uint8_t>(offset & 0xFF);
    out[1] = static_cast<uint8_t>((offset >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((offset >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((offset >> 24) & 0xFF);
    return true;
}

// Parsed GET_INFO reply (60-byte payload from the OxyII E1 response).
// Only sn and firmware are surfaced; the rest of the payload bytes are
// reserved for a future caller that wants battery/storage/etc. info.
struct DeviceInfo {
    bool ok;
    char    firmwareVersion[9];   // 8-char ASCII + null
    char    serialNumber[33];     // generous cap; T8520 reports 10 chars
};

inline DeviceInfo parseGetInfoReply(const uint8_t* payload, size_t payloadLen) {
    DeviceInfo info{};
    if (payloadLen < 48) return info;
    // firmware version at [9..17]
    for (int i = 0; i < 8; i++) info.firmwareVersion[i] = static_cast<char>(payload[9 + i]);
    info.firmwareVersion[8] = '\0';
    // strip trailing nulls in fw string
    for (int i = 7; i >= 0 && info.firmwareVersion[i] == '\0'; i--) info.firmwareVersion[i] = '\0';
    // serial-number length at [37], chars start at [38]
    uint8_t snLen = payload[37];
    if (snLen == 0 || static_cast<size_t>(38 + snLen) > payloadLen ||
        snLen >= sizeof(info.serialNumber)) {
        info.serialNumber[0] = '\0';
    } else {
        for (size_t i = 0; i < snLen; i++) {
            info.serialNumber[i] = static_cast<char>(payload[38 + i]);
        }
        info.serialNumber[snLen] = '\0';
    }
    info.ok = true;
    return info;
}

// One file entry from a GET_FILE_LIST reply. Names are timestamp strings
// like "20260427105949" (14 chars).
constexpr size_t FILE_NAME_SLOT = 16;
constexpr size_t MAX_FILE_NAME  = 14;

struct FileListIter {
    const uint8_t* plaintext;
    size_t         plaintextLen;
    uint8_t        count;       // declared count from byte [0]
    size_t         pos;         // next-slot offset into plaintext
};

inline FileListIter beginFileList(const uint8_t* plaintext, size_t plaintextLen) {
    FileListIter it{};
    it.plaintext = plaintext;
    it.plaintextLen = plaintextLen;
    if (plaintextLen >= 1) {
        it.count = plaintext[0];
        it.pos = 1;
    }
    return it;
}

// Pull the next filename into outName (must be MAX_FILE_NAME + 1 = 17 bytes).
// Returns true if a name was emitted, false at end-of-list or on truncation.
inline bool nextFilename(FileListIter& it, char outName[MAX_FILE_NAME + 2]) {
    if (it.pos + FILE_NAME_SLOT > it.plaintextLen) return false;
    const uint8_t* slot = it.plaintext + it.pos;
    size_t n = 0;
    while (n < MAX_FILE_NAME && slot[n] != 0x00) {
        outName[n] = static_cast<char>(slot[n]);
        n++;
    }
    outName[n] = '\0';
    it.pos += FILE_NAME_SLOT;
    return n > 0;
}

}  // namespace OxyIIProtocol

#endif  // OXYII_PROTOCOL_H
