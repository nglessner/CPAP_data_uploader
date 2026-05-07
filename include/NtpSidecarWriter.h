#ifndef NTP_SIDECAR_WRITER_H
#define NTP_SIDECAR_WRITER_H

#ifdef UNIT_TEST
#include "Arduino.h"
#else
#include <Arduino.h>
#endif

class NtpSidecarWriter {
public:
    static bool isDatalogEdf(const String& path);
};

#endif // NTP_SIDECAR_WRITER_H
