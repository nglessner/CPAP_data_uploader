#ifndef MOCK_WIFI_H
#define MOCK_WIFI_H

#ifdef UNIT_TEST

#include "Arduino.h"

class MockWiFi {
public:
    String _macAddress = "AC:0B:FB:6F:A1:94";  // deterministic default for tests
    String macAddress() { return _macAddress; }
    void setMockMacAddress(const String& mac) { _macAddress = mac; }
};

extern MockWiFi WiFi;

#endif // UNIT_TEST

#endif // MOCK_WIFI_H
