#ifndef O2RING_STATUS_H
#define O2RING_STATUS_H

#include <Arduino.h>

#ifdef UNIT_TEST
#include "MockPreferences.h"
#else
#include <Preferences.h>
#endif

// Persisted snapshot of the most recent O2RingSync::run() outcome.
// Backed by a single pipe-delimited NVS string so the whole record is
// atomic per load()/save(). Namespace "o2ring_stat", key "last".
//
// Format: "<unix>|<result_int>|<count>|<filename>"
// Empty string -> no prior sync recorded (hasData() == false).
class O2RingStatus {
public:
    O2RingStatus();

    void load();
    void save();

    // Record a completed run. `result` is the numeric value of
    // O2RingSyncResult. `filesSynced` is the number of files that
    // successfully uploaded. `lastFilename` is the most recent filename
    // observed in the device's INFO response (empty if unknown).
    void record(int result, uint16_t filesSynced, const String& lastFilename);

    // Variant for runs that did not reach INFO — keeps whatever filename
    // was previously stored so a transient BLE blip doesn't blank out a
    // known-good filename. Caller must have called load() first.
    void recordPreservingFilename(int result);

    bool hasData() const { return lastUnix != 0; }
    uint32_t getLastUnix() const { return lastUnix; }
    int getLastResult() const { return lastResult; }
    uint16_t getFilesSynced() const { return filesSynced; }
    const String& getLastFilename() const { return lastFilename; }

private:
    uint32_t lastUnix;
    int lastResult;
    uint16_t filesSynced;
    String lastFilename;
    Preferences prefs;

    static const char* NAMESPACE;
    static const char* KEY_LAST;
};

#endif // O2RING_STATUS_H
