#ifndef O2RING_STATE_H
#define O2RING_STATE_H

#include <Arduino.h>
#include <set>
#include <vector>

#ifdef UNIT_TEST
#include "MockPreferences.h"
#else
#include <Preferences.h>
#endif

class O2RingState {
public:
    O2RingState();
    void load();
    void save();
    bool hasSeen(const String& filename) const;
    void markSeen(const String& filename);

    // Evict entries not present in keep. Used to bound the NVS string to the
    // device's current on-ring file list so the serialized form cannot grow
    // past the 4000-byte nvs_set_str ceiling. See issue #2.
    void retainOnly(const std::vector<String>& keep);

private:
    std::set<String> seen;
    Preferences prefs;

    static const char* NAMESPACE;
    static const char* KEY_SEEN;
};

#endif // O2RING_STATE_H
