#ifdef ENABLE_O2RING_SYNC

#include "O2RingSync.h"
#include "O2RingProtocol.h"
#include "Logger.h"
#include "O2RingStatus.h"
#include <vector>

O2RingSync::O2RingSync(Config* cfg, IBleClient* bleClient)
    : config(cfg), ble(bleClient) {}

bool O2RingSync::sendCommand(uint8_t cmd, uint16_t block,
                              const uint8_t* data, uint16_t dataLen) {
    uint8_t packet[256];
    if ((size_t)8 + dataLen > sizeof(packet)) {
        LOG_ERRORF("[O2Ring] sendCommand: dataLen %u exceeds packet buffer", (unsigned)dataLen);
        return false;
    }
    size_t len = O2RingProtocol::buildPacket(packet, cmd, block, data, dataLen);
    return ble->writeChunked(packet, len);
}

bool O2RingSync::receiveResponse(uint8_t* buffer, size_t bufCap, size_t& outLen,
                                  uint32_t timeoutMs) {
    return ble->readResponse(buffer, bufCap, outLen, timeoutMs);
}

bool O2RingSync::downloadAndUpload(const String& filename) {
    // FILE_OPEN: send filename as raw bytes with explicit null terminator.
    // We use a uint8_t buffer to avoid String(char) ambiguity with '\0'.
    size_t fnLen = filename.length();
    std::vector<uint8_t> fnBuf(fnLen + 1);
    memcpy(fnBuf.data(), filename.c_str(), fnLen);
    fnBuf[fnLen] = 0x00;

    if (!sendCommand(O2RingProtocol::CMD_FILE_OPEN, 0,
                     fnBuf.data(), (uint16_t)fnBuf.size())) {
        LOG_ERRORF("[O2Ring] FILE_OPEN write failed: %s", filename.c_str());
        return false;
    }

    uint8_t respBuf[256];
    size_t respLen = 0;
    if (!receiveResponse(respBuf, sizeof(respBuf), respLen)) {
        LOG_ERRORF("[O2Ring] FILE_OPEN no response: %s", filename.c_str());
        return false;
    }

    // Extract DATA portion (bytes 7 .. respLen-2, excluding CRC)
    if (respLen < 8) {
        LOG_ERRORF("[O2Ring] FILE_OPEN response too short: %s", filename.c_str());
        return false;
    }
    uint32_t fileSize = 0;
    if (!O2RingProtocol::parseFileOpenResponse(respBuf + 7, respLen - 8, fileSize)) {
        LOGF("[O2Ring] FILE_OPEN failed (error status): %s", filename.c_str());
        return false;
    }
    LOGF("[O2Ring] File: %s  size: %u bytes", filename.c_str(), (unsigned)fileSize);

    // Helper to close the open file handle. CLOSE response CRC errors are not fatal.
    auto fileClose = [&]() {
        sendCommand(O2RingProtocol::CMD_FILE_CLOSE, 0, nullptr, 0);
        receiveResponse(respBuf, sizeof(respBuf), respLen);
    };

    // Read file in blocks
    std::vector<uint8_t> fileData;
    fileData.reserve(fileSize);
    uint16_t block = 0;
    size_t received = 0;
    while (received < fileSize) {
        if (!sendCommand(O2RingProtocol::CMD_FILE_READ, block, nullptr, 0)) {
            LOG_ERRORF("[O2Ring] FILE_READ write failed at block %u", (unsigned)block);
            fileClose();
            return false;
        }
        size_t rLen = 0;
        if (!receiveResponse(respBuf, sizeof(respBuf), rLen)) {
            LOGF("[O2Ring] FILE_READ no response at block %u", (unsigned)block);
            fileClose();
            return false;
        }
        if (rLen < 9) break;
        uint16_t dataLen = (uint16_t)respBuf[5] | ((uint16_t)respBuf[6] << 8);
        if ((size_t)(7 + dataLen) > rLen) break;
        for (uint16_t i = 0; i < dataLen && received < fileSize; i++) {
            fileData.push_back(respBuf[7 + i]);
            received++;
        }
        block++;
    }

    // FILE_CLOSE
    fileClose();

    if (received < fileSize) {
        LOGF("[O2Ring] Incomplete file: %s (%u/%u bytes)", filename.c_str(),
             (unsigned)received, (unsigned)fileSize);
        return false;
    }

#ifdef ENABLE_SMB_UPLOAD
    SMBUploader smb(config->getEndpoint(),
                    config->getEndpointUser(),
                    config->getEndpointPassword());
    if (!smb.begin()) {
        LOG_ERROR("[O2Ring] SMB connect failed");
        return false;
    }
    String dir = "/" + config->getO2RingPath() + "/" + config->getDeviceSegment();
    smb.createDirectory(dir);
    String remotePath = dir + "/" + filename;
    bool uploaded = smb.uploadRawBuffer(remotePath, fileData.data(), fileData.size());
    smb.end();
    if (!uploaded) {
        LOG_ERROR("[O2Ring] SMB uploadBuffer failed");
        return false;
    }
#endif

    return true;
}

O2RingSyncResult O2RingSync::run() {
    LOG("[O2Ring] Starting sync");

    O2RingStatus status;
    status.load();

    if (!ble->connect(config->getO2RingDeviceName(),
                      config->getO2RingScanSeconds())) {
        O2RingSyncResult code;
        if (ble->wasDeviceFound()) {
            LOG_WARN("[O2Ring] Device found but connection failed");
            code = O2RingSyncResult::CONNECT_FAILED;
        } else {
            LOG_WARN("[O2Ring] Device not found");
            code = O2RingSyncResult::DEVICE_NOT_FOUND;
        }
        status.recordPreservingFilename((int)code);
        status.save();
        return code;
    }
    LOG("[O2Ring] Connected to O2Ring-S");

    if (!sendCommand(O2RingProtocol::CMD_INFO, 0, nullptr, 0)) {
        ble->disconnect();
        status.recordPreservingFilename((int)O2RingSyncResult::BLE_ERROR);
        status.save();
        return O2RingSyncResult::BLE_ERROR;
    }

    uint8_t respBuf[512];
    size_t respLen = 0;
    if (!receiveResponse(respBuf, sizeof(respBuf), respLen, 8000)) {
        ble->disconnect();
        status.recordPreservingFilename((int)O2RingSyncResult::BLE_ERROR);
        status.save();
        return O2RingSyncResult::BLE_ERROR;
    }

    std::vector<String> fileList;
    uint16_t dataLen = (uint16_t)respBuf[5] | ((uint16_t)respBuf[6] << 8);
    if (!O2RingProtocol::parseInfoFileList(respBuf + 7, dataLen, fileList)) {
        ble->disconnect();
        status.recordPreservingFilename((int)O2RingSyncResult::BLE_ERROR);
        status.save();
        return O2RingSyncResult::BLE_ERROR;
    }
    LOGF("[O2Ring] Device reports %u file(s)", (unsigned)fileList.size());

    // Compute the lexicographic max of the observed FileList so status can
    // show "last filename seen on device" — a liveness signal even on
    // NOTHING_TO_SYNC. Empty FileList -> empty string.
    String latestOnDevice;
    for (const auto& f : fileList) {
        if (latestOnDevice < f) latestOnDevice = f;
    }

    // Set the ring's wall-clock from the ESP32's localtime. Best-effort:
    // any failure here logs a warning but does not abort the sync — file
    // pulls don't depend on the clock being correct, and the ring will
    // accept the next sync's SetTIME write just as well.
    {
        time_t now = time(nullptr);
        struct tm tmnow;
        char payload[64];
        size_t payloadLen = 0;
        if (localtime_r(&now, &tmnow) != nullptr) {
            payloadLen = O2RingProtocol::formatSetTimePayload(
                tmnow, payload, sizeof(payload));
        }
        if (payloadLen == 0) {
            LOG_WARN("[O2Ring] SetTIME payload format failed");
        } else if (!sendCommand(O2RingProtocol::CMD_CONFIG, 0,
                                (const uint8_t*)payload,
                                (uint16_t)payloadLen)) {
            LOG_WARN("[O2Ring] SetTIME write failed");
        } else {
            uint8_t respBuf[64];
            size_t respLen = 0;
            if (!receiveResponse(respBuf, sizeof(respBuf), respLen, 2000)) {
                LOG_WARN("[O2Ring] SetTIME response timeout");
            } else if (respLen >= 2
                       && respBuf[1] != O2RingProtocol::CMD_CONFIG) {
                LOG_WARN("[O2Ring] SetTIME response cmd mismatch");
            } else {
                LOG("[O2Ring] SetTIME ok");
            }
        }
    }

    state.load();
    // Bound the dedup set to what the device currently reports. History outside
    // the ring's on-device file list is dead weight and, unpruned, would grow
    // the serialized NVS string past the 4000-byte per-entry ceiling. See #2.
    state.retainOnly(fileList);
    state.save();

    std::vector<String> toDownload;
    for (auto& f : fileList) {
        if (!state.hasSeen(f)) toDownload.push_back(f);
    }

    if (toDownload.empty()) {
        LOG("[O2Ring] Nothing new to sync");
        ble->disconnect();
        status.record((int)O2RingSyncResult::NOTHING_TO_SYNC, 0, latestOnDevice);
        status.save();
        return O2RingSyncResult::NOTHING_TO_SYNC;
    }

    uint16_t synced = 0;
    bool anyFailed = false;
    for (auto& f : toDownload) {
        if (downloadAndUpload(f)) {
            state.markSeen(f);
            state.save();
            synced++;
            LOGF("[O2Ring] Synced: %s", f.c_str());
        } else {
            anyFailed = true;
            LOGF("[O2Ring] Failed to sync: %s", f.c_str());
        }
    }

    ble->disconnect();
    O2RingSyncResult result = anyFailed
        ? O2RingSyncResult::BLE_ERROR
        : O2RingSyncResult::OK;
    status.record((int)result, synced, latestOnDevice);
    status.save();
    return result;
}

#endif // ENABLE_O2RING_SYNC
