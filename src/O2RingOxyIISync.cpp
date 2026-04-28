#include "O2RingOxyIISync.h"

#include <ctime>
#include <cstring>

#include "Logger.h"
#include "OxyIIAes.h"
#include "OxyIIProtocol.h"

using OxyIIProtocol::DecodedFrame;
using OxyIIProtocol::DeviceInfo;
using OxyIIProtocol::FileListIter;

namespace {

// Build an 8-byte SET_UTC_TIME payload from a struct tm. Layout per the
// captured ViHealth snoop: year LE u16, month, day, hour, minute, second,
// timezone (1 byte, generally 0xCE for negative-offset hex tags or simply
// the local hour offset; we set it to 0 — ring accepts).
size_t buildSetTimePayload(const struct tm& tm, uint8_t* out, size_t outCap) {
    if (outCap < 8) return 0;
    int year = tm.tm_year + 1900;
    out[0] = static_cast<uint8_t>(year & 0xFF);
    out[1] = static_cast<uint8_t>((year >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>(tm.tm_mon + 1);
    out[3] = static_cast<uint8_t>(tm.tm_mday);
    out[4] = static_cast<uint8_t>(tm.tm_hour);
    out[5] = static_cast<uint8_t>(tm.tm_min);
    out[6] = static_cast<uint8_t>(tm.tm_sec);
    out[7] = 0x00;
    return 8;
}

}  // namespace

O2RingOxyIISync::O2RingOxyIISync(IBleClient& ble,
                                  O2RingState& state,
                                  const OxyIIConfig& config,
                                  OnFileComplete onFileComplete)
    : _ble(ble), _state(state), _config(config), _onFileComplete(std::move(onFileComplete)) {}

bool O2RingOxyIISync::sendCommand(uint8_t opcode, const uint8_t* payload, size_t payloadLen,
                                   uint8_t* outPayload, size_t outCap, size_t& outLen) {
    uint8_t frame[OxyIIProtocol::FRAME_HEADER_LEN + 600 + 1];
    size_t frameLen = OxyIIProtocol::encodeFrame(opcode, payload, payloadLen,
                                                  _seq++, frame, sizeof(frame));
    if (frameLen == 0) {
        LOG_ERROR("[OxyII] encode frame failed");
        return false;
    }

    if (!_ble.writeChunked(frame, frameLen)) {
        LOG_ERRORF("[OxyII] write failed for opcode 0x%02x", opcode);
        return false;
    }

    uint8_t replyFrame[kNotifyBufCap];
    size_t replyLen = 0;
    if (!_ble.readResponse(replyFrame, sizeof(replyFrame), replyLen, _config.cmdTimeoutMs)) {
        LOG_ERRORF("[OxyII] no reply for opcode 0x%02x", opcode);
        return false;
    }

    DecodedFrame d = OxyIIProtocol::decodeFrame(replyFrame, replyLen);
    if (!d.ok) {
        LOG_ERRORF("[OxyII] decode failed for opcode 0x%02x (err %u)",
                   opcode, (unsigned)d.errorCode);
        return false;
    }
    if (d.opcode != opcode) {
        LOG_ERRORF("[OxyII] reply opcode mismatch: sent 0x%02x got 0x%02x",
                   opcode, d.opcode);
        return false;
    }

    outLen = d.payloadLen;
    if (outLen > outCap) outLen = outCap;
    if (outLen > 0) memcpy(outPayload, replyFrame + d.payloadOffset, outLen);
    return true;
}

bool O2RingOxyIISync::sendEncryptedCommand(uint8_t opcode, const uint8_t* payload, size_t payloadLen,
                                            uint8_t* outPayload, size_t outCap, size_t& outLen) {
    if (!_sessionKeyDerived) {
        LOG_ERROR("[OxyII] sendEncryptedCommand before key derived");
        return false;
    }
    uint8_t cipher[64];
    size_t cipherLen = OxyIIAes::encryptEcbPkcs7(payload, payloadLen, _sessionKey,
                                                  cipher, sizeof(cipher));
    if (cipherLen == 0) {
        LOG_ERROR("[OxyII] AES encrypt failed");
        return false;
    }
    return sendCommand(opcode, cipher, cipherLen, outPayload, outCap, outLen);
}

bool O2RingOxyIISync::pullFile(const String& filename) {
    // F2 — READ_FILE_START. Encrypted 20-byte payload: 16-byte filename slot
    // (null-padded) + u32 LE file_type (0 = .dat / OXY).
    uint8_t startPayload[20];
    if (!OxyIIProtocol::buildReadFileStartPayload(filename.c_str(), filename.length(),
                                                   /*fileType=*/0, startPayload, sizeof(startPayload))) {
        LOG_ERROR("[OxyII] readFileStart payload build failed");
        return false;
    }

    uint8_t reply[kNotifyBufCap];
    size_t replyLen = 0;
    if (!sendEncryptedCommand(OxyIIProtocol::OP_READ_FILE_START,
                               startPayload, sizeof(startPayload),
                               reply, sizeof(reply), replyLen)) {
        return false;
    }

    // F3 loop — fetch chunks at increasing offsets until a chunk comes back
    // shorter than the previous one (signaling end of file).
    std::vector<uint8_t> fileBytes;
    fileBytes.reserve(8 * 1024);   // initial guess; std::vector will grow if needed

    uint32_t offset = 0;
    size_t   prevChunkLen = 0;
    bool     finished = false;

    while (!finished) {
        uint8_t dataPayload[4];
        if (!OxyIIProtocol::buildReadFileDataPayload(offset, dataPayload, sizeof(dataPayload))) {
            LOG_ERROR("[OxyII] readFileData payload build failed");
            return false;
        }
        size_t chunkLen = 0;
        if (!sendCommand(OxyIIProtocol::OP_READ_FILE_DATA,
                         dataPayload, sizeof(dataPayload),
                         reply, sizeof(reply), chunkLen)) {
            LOG_ERRORF("[OxyII] readFileData failed at offset %u", (unsigned)offset);
            return false;
        }
        if (chunkLen > 0) {
            fileBytes.insert(fileBytes.end(), reply, reply + chunkLen);
            offset += chunkLen;
        }
        // End-of-file signal: a chunk shorter than the previous one. The first
        // short-or-equal chunk after at least one chunk closes the loop.
        if (prevChunkLen != 0 && chunkLen < prevChunkLen) {
            finished = true;
        } else if (chunkLen == 0) {
            finished = true;
        }
        prevChunkLen = chunkLen;
    }

    // F4 — READ_FILE_END
    if (!sendCommand(OxyIIProtocol::OP_READ_FILE_END, nullptr, 0,
                     reply, sizeof(reply), replyLen)) {
        LOG_ERROR("[OxyII] readFileEnd failed");
        return false;
    }

    if (fileBytes.empty()) {
        LOG_ERROR("[OxyII] file pull produced zero bytes");
        return false;
    }

    if (!_onFileComplete(filename, fileBytes.data(), fileBytes.size())) {
        LOG_ERRORF("[OxyII] onFileComplete returned false for %s", filename.c_str());
        return false;
    }

    _lastSyncedCount++;
    _lastSyncedFilename = filename;
    return true;
}

O2RingSyncResult O2RingOxyIISync::run() {
    LOG("[OxyII] Starting sync");
    _seq = 0;
    _sessionKeyDerived = false;
    _lastSyncedCount = 0;
    _lastSyncedFilename = "";

    if (!_ble.connect(_config.deviceNamePrefix, _config.scanSeconds)) {
        if (_ble.wasDeviceFound()) {
            LOG_WARN("[OxyII] Device found but connection failed");
            return O2RingSyncResult::CONNECT_FAILED;
        }
        LOG_WARN("[OxyII] Device not found");
        return O2RingSyncResult::NO_DEVICE_FOUND;
    }

    if (!_ble.requestMtu(_config.mtu)) {
        LOG_ERRORF("[OxyII] MTU negotiation failed (wanted %u)", _config.mtu);
        _ble.disconnect();
        return O2RingSyncResult::MTU_FAILED;
    }

    // Derive session key tentatively from a placeholder serial; we'll re-
    // derive after GET_INFO. AUTH (0xFF) is sent first per ViHealth's order,
    // before the serial is known — but the key it uses is the same lepucloud-
    // derived form with a degenerate serial slot. Real T8520 sessions are
    // re-keyed after GET_INFO; we do the same.
    if (!OxyIIProtocol::deriveSessionKey("0000", 4,
                                          static_cast<uint64_t>(time(nullptr)),
                                          _sessionKey)) {
        LOG_ERROR("[OxyII] initial session-key derivation failed");
        _ble.disconnect();
        return O2RingSyncResult::AUTH_FAILED;
    }
    _sessionKeyDerived = true;

    uint8_t reply[kNotifyBufCap];
    size_t replyLen = 0;

    // 0xFF AUTH — encrypted 16-byte payload (zeros are accepted)
    {
        uint8_t authPayload[16] = {0};
        if (!sendEncryptedCommand(OxyIIProtocol::OP_AUTH, authPayload, sizeof(authPayload),
                                   reply, sizeof(reply), replyLen)) {
            _ble.disconnect();
            return O2RingSyncResult::AUTH_FAILED;
        }
    }

    // 0x10 KEEPALIVE
    {
        uint8_t kp[1] = {0x00};
        if (!sendCommand(OxyIIProtocol::OP_KEEPALIVE, kp, sizeof(kp),
                         reply, sizeof(reply), replyLen)) {
            _ble.disconnect();
            return O2RingSyncResult::HANDSHAKE_FAILED;
        }
    }

    // 0xC0 SET_UTC_TIME — best-effort; failure is not fatal for file pull
    {
        time_t now = time(nullptr);
        struct tm tmnow;
        uint8_t setTimePayload[8] = {0};
        size_t setTimeLen = 0;
        if (localtime_r(&now, &tmnow) != nullptr) {
            setTimeLen = buildSetTimePayload(tmnow, setTimePayload, sizeof(setTimePayload));
        }
        if (setTimeLen == 8) {
            sendCommand(OxyIIProtocol::OP_SET_UTC_TIME,
                        setTimePayload, sizeof(setTimePayload),
                        reply, sizeof(reply), replyLen);
        }
    }

    // 0x00 HANDSHAKE
    if (!sendCommand(OxyIIProtocol::OP_HANDSHAKE, nullptr, 0,
                     reply, sizeof(reply), replyLen)) {
        _ble.disconnect();
        return O2RingSyncResult::HANDSHAKE_FAILED;
    }

    // 0xE1 GET_INFO — extract serial for session-key re-derivation
    if (!sendCommand(OxyIIProtocol::OP_GET_INFO, nullptr, 0,
                     reply, sizeof(reply), replyLen)) {
        _ble.disconnect();
        return O2RingSyncResult::GET_INFO_FAILED;
    }
    DeviceInfo info = OxyIIProtocol::parseGetInfoReply(reply, replyLen);
    if (!info.ok || info.serialNumber[0] == '\0') {
        LOG_ERROR("[OxyII] GET_INFO returned no serial");
        _ble.disconnect();
        return O2RingSyncResult::GET_INFO_FAILED;
    }
    LOGF("[OxyII] sn=%s fw=%s", info.serialNumber, info.firmwareVersion);

    // Re-derive session key with real serial. The key is the same algorithm
    // both phone and ring run; mismatch here would silently fail F2.
    if (!OxyIIProtocol::deriveSessionKey(info.serialNumber, strlen(info.serialNumber),
                                          static_cast<uint64_t>(time(nullptr)),
                                          _sessionKey)) {
        LOG_ERROR("[OxyII] session-key re-derivation failed");
        _ble.disconnect();
        return O2RingSyncResult::GET_INFO_FAILED;
    }

    // 0xF1 GET_FILE_LIST
    if (!sendCommand(OxyIIProtocol::OP_GET_FILE_LIST, nullptr, 0,
                     reply, sizeof(reply), replyLen)) {
        _ble.disconnect();
        return O2RingSyncResult::FILE_LIST_FAILED;
    }

    std::vector<String> deviceFiles;
    {
        FileListIter it = OxyIIProtocol::beginFileList(reply, replyLen);
        char name[OxyIIProtocol::MAX_FILE_NAME + 2];
        while (OxyIIProtocol::nextFilename(it, name)) {
            deviceFiles.emplace_back(name);
        }
    }
    LOGF("[OxyII] Device reports %u file(s)", (unsigned)deviceFiles.size());

    // Pull each file not in dedup state
    bool anyFailed = false;
    for (const auto& filename : deviceFiles) {
        if (_state.hasSeen(filename)) {
            LOGF("[OxyII] Skipping already-synced %s", filename.c_str());
            continue;
        }
        if (pullFile(filename)) {
            _state.markSeen(filename);
            _state.save();
        } else {
            LOGF("[OxyII] File transfer failed for %s — leaving un-synced", filename.c_str());
            anyFailed = true;
            break;  // bail on first transfer failure; FSM retry will re-attempt
        }
    }

    // Prune dedup set to current device file list (keeps NVS string bounded
    // — see admin/sleep #2 for the historical reason behind this).
    _state.retainOnly(deviceFiles);
    _state.save();

    _ble.disconnect();

    return anyFailed ? O2RingSyncResult::FILE_TRANSFER_FAILED : O2RingSyncResult::OK;
}
