#ifndef O2RING_STATE_H
#define O2RING_STATE_H

#include <Arduino.h>
#include <set>

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

private:
    std::set<String> seen;
    Preferences prefs;

    static const char* NAMESPACE;
    static const char* KEY_SEEN;
};

#endif // O2RING_STATE_H
