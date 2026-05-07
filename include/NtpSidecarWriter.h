#ifndef NTP_SIDECAR_WRITER_H
#define NTP_SIDECAR_WRITER_H

#ifdef UNIT_TEST
#include "Arduino.h"
#include "MockFS.h"
#else
#include <Arduino.h>
#include <FS.h>
#endif

struct EdfHeader {
    char startNaiveStr[20];   // "YYYY-MM-DDTHH:MM:SS" + null
    int  durationSeconds;
};

enum class SkipReason {
    NONE,
    NTP_UNSYNCED,
    EDF_PARSE_FAILED
};

struct SidecarPayload {
    bool       valid;
    SkipReason skipReason;
    time_t     ntp_observed_at_unix;
    time_t     fat_mtime_unix;          // 0 ⇒ omit field from JSON
    EdfHeader  header;
    int        pollIntervalSeconds;
    const char* firmwareVersion;        // e.g. FIRMWARE_VERSION
};

class NtpSidecarWriter {
public:
    static bool isDatalogEdf(const String& path);

    // Reads the first 256 bytes of an open File and parses the EDF header
    // start time (offset 168/176) and duration (offsets 236+244).
    // Returns false on short read or malformed fields.
#ifdef UNIT_TEST
    static bool parseEdfHeader(MockFile& f, EdfHeader& out);
#else
    static bool parseEdfHeader(fs::File& f, EdfHeader& out);
#endif

    // Returns true if `now` looks like a real NTP-synced time
    // (after 2024-01-01 00:00:00 UTC = 1704067200).
    static bool isNtpSynced(time_t now);

    // Serializes the payload as compact JSON into `buf` (capacity `cap`).
    // Returns the number of bytes written (excluding the null terminator),
    // or 0 if the buffer is too small.
    static size_t serializeJson(const SidecarPayload& p, char* buf, size_t cap);

    // Captures everything needed for a sidecar in one shot. Reads the EDF
    // header from disk, snapshots the current time, reads FAT mtime.
    // Returns a fully-populated SidecarPayload on success, or a
    // skipReason-tagged invalid payload if NTP is unsynced or the EDF
    // header is unparseable. Logs a warning via LOG()/LOGF() on skip paths.
#ifdef UNIT_TEST
    static SidecarPayload captureWitness(MockFS& fs, const String& localPath,
                                         int pollIntervalSeconds,
                                         const char* firmwareVersion);
#else
    static SidecarPayload captureWitness(fs::FS& fs, const String& localPath,
                                         int pollIntervalSeconds,
                                         const char* firmwareVersion);
#endif

    // Sink callback signature. ctx is a caller-provided pointer (e.g.
    // SMBUploader*). Returns true on successful write.
    typedef bool (*SinkFn)(void* ctx, const String& remotePath,
                           const uint8_t* data, size_t len);

    // Writes the sidecar JSON to <edfRemotePath>.ntp.json via the sink.
    // - If payload is invalid, returns true without invoking the sink.
    // - If serialisation fails (buffer too small), returns false.
    // - Otherwise returns the sink's return value.
    static bool write(SinkFn sink, void* sinkCtx,
                      const String& edfRemotePath,
                      const SidecarPayload& payload);
};

#endif // NTP_SIDECAR_WRITER_H
