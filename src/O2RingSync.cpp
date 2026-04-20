#include "O2RingSync.h"
#include "O2RingProtocol.h"
#include "Logger.h"
#include <vector>

O2RingSync::O2RingSync(Config* cfg, IBleClient* bleClient)
    : config(cfg), ble(bleClient) {}

bool O2RingSync::sendCommand(uint8_t cmd, uint16_t block,
                              const uint8_t* data, uint16_t dataLen) {
    uint8_t packet[256];
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

    // Read file in blocks
    std::vector<uint8_t> fileData;
    fileData.reserve(fileSize);
    uint16_t block = 0;
    size_t received = 0;
    while (received < fileSize) {
        if (!sendCommand(O2RingProtocol::CMD_FILE_READ, block, nullptr, 0)) {
            LOG_ERRORF("[O2Ring] FILE_READ write failed at block %u", (unsigned)block);
            return false;
        }
        size_t rLen = 0;
        if (!receiveResponse(respBuf, sizeof(respBuf), rLen)) {
            LOGF("[O2Ring] FILE_READ no response at block %u", (unsigned)block);
            return false;
        }
        if (rLen < 9) break;
        uint16_t dataLen = (uint16_t)respBuf[5] | ((uint16_t)respBuf[6] << 8);
        for (uint16_t i = 0; i < dataLen && received < fileSize; i++) {
            fileData.push_back(respBuf[7 + i]);
            received++;
        }
        block++;
    }

    // FILE_CLOSE
    sendCommand(O2RingProtocol::CMD_FILE_CLOSE, 0, nullptr, 0);
    receiveResponse(respBuf, sizeof(respBuf), respLen);

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
    String remotePath = "/" + config->getO2RingPath() + "/" + filename;
    smb.createDirectory("/" + config->getO2RingPath());

    // Full SMB buffer upload is wired up in Task 6.
    LOGF("[O2Ring] Would upload %u bytes to %s", (unsigned)fileData.size(), remotePath.c_str());
    smb.end();
#endif

    return true;
}

O2RingSyncResult O2RingSync::run() {
    LOG("[O2Ring] Starting sync");

    if (!ble->connect(config->getO2RingDeviceName(), config->getO2RingScanSeconds())) {
        LOG_WARN("[O2Ring] Device not found");
        return O2RingSyncResult::DEVICE_NOT_FOUND;
    }
    LOG("[O2Ring] Connected to O2Ring-S");

    if (!sendCommand(O2RingProtocol::CMD_INFO, 0, nullptr, 0)) {
        ble->disconnect();
        return O2RingSyncResult::BLE_ERROR;
    }

    uint8_t respBuf[512];
    size_t respLen = 0;
    if (!receiveResponse(respBuf, sizeof(respBuf), respLen, 8000)) {
        ble->disconnect();
        return O2RingSyncResult::BLE_ERROR;
    }

    std::vector<String> fileList;
    uint16_t dataLen = (uint16_t)respBuf[5] | ((uint16_t)respBuf[6] << 8);
    if (!O2RingProtocol::parseInfoFileList(respBuf + 7, dataLen, fileList)) {
        ble->disconnect();
        return O2RingSyncResult::BLE_ERROR;
    }
    LOGF("[O2Ring] Device reports %u file(s)", (unsigned)fileList.size());

    state.load();
    std::vector<String> toDownload;
    for (auto& f : fileList) {
        if (!state.hasSeen(f)) toDownload.push_back(f);
    }

    if (toDownload.empty()) {
        LOG("[O2Ring] Nothing new to sync");
        ble->disconnect();
        return O2RingSyncResult::NOTHING_TO_SYNC;
    }

    bool anyFailed = false;
    for (auto& f : toDownload) {
        if (downloadAndUpload(f)) {
            state.markSeen(f);
            state.save();
            LOGF("[O2Ring] Synced: %s", f.c_str());
        } else {
            anyFailed = true;
            LOGF("[O2Ring] Failed to sync: %s", f.c_str());
        }
    }

    ble->disconnect();
    return anyFailed ? O2RingSyncResult::BLE_ERROR : O2RingSyncResult::OK;
}
