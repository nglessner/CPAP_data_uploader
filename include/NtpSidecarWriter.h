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
};

#endif // NTP_SIDECAR_WRITER_H
