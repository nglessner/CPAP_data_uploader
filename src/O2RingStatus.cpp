#include "O2RingStatus.h"
#include <time.h>

const char* O2RingStatus::NAMESPACE = "o2ring_stat";
const char* O2RingStatus::KEY_LAST  = "last";

O2RingStatus::O2RingStatus()
    : lastUnix(0), lastResult(-1), filesSynced(0), lastFilename("") {}

void O2RingStatus::load() {
    lastUnix = 0;
    lastResult = -1;
    filesSynced = 0;
    lastFilename = "";

    prefs.begin(NAMESPACE, true);
    String stored = prefs.getString(KEY_LAST, "");
    prefs.end();
    if (stored.length() == 0) return;

    int p1 = stored.indexOf('|');
    int p2 = (p1 >= 0) ? stored.indexOf('|', p1 + 1) : -1;
    int p3 = (p2 >= 0) ? stored.indexOf('|', p2 + 1) : -1;
    if (p1 < 0 || p2 < 0 || p3 < 0) return;  // malformed -> treat as empty

    lastUnix     = (uint32_t)strtoul(stored.substring(0, p1).c_str(), nullptr, 10);
    lastResult   = (int)strtol(stored.substring(p1 + 1, p2).c_str(), nullptr, 10);
    filesSynced  = (uint16_t)strtoul(stored.substring(p2 + 1, p3).c_str(), nullptr, 10);
    lastFilename = stored.substring(p3 + 1);
}

void O2RingStatus::save() {
    String serialised;
    serialised += String((unsigned long)lastUnix);
    serialised += "|";
    serialised += String((int)lastResult);
    serialised += "|";
    serialised += String((unsigned long)filesSynced);
    serialised += "|";
    serialised += lastFilename;

    prefs.begin(NAMESPACE, false);
    prefs.putString(KEY_LAST, serialised);
    prefs.end();
}

void O2RingStatus::record(int result, uint16_t synced,
                          const String& filename) {
    lastUnix = (uint32_t)time(nullptr);
    lastResult = result;
    filesSynced = synced;
    lastFilename = filename;
}

void O2RingStatus::recordPreservingFilename(int result) {
    lastUnix = (uint32_t)time(nullptr);
    lastResult = result;
    filesSynced = 0;
    // lastFilename left untouched
}
