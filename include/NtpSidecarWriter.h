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
};

#endif // NTP_SIDECAR_WRITER_H
