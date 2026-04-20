#include "O2RingState.h"

const char* O2RingState::NAMESPACE = "o2ring";
const char* O2RingState::KEY_SEEN  = "synced";

O2RingState::O2RingState() {}

void O2RingState::load() {
    seen.clear();
    prefs.begin(NAMESPACE, true);
    String stored = prefs.getString(KEY_SEEN, "");
    prefs.end();
    if (stored.length() == 0) return;
    int start = 0;
    while (start < (int)stored.length()) {
        int comma = stored.indexOf(',', start);
        if (comma < 0) {
            seen.insert(stored.substring(start));
            break;
        }
        seen.insert(stored.substring(start, comma));
        start = comma + 1;
    }
}

void O2RingState::save() {
    String serialised;
    bool first = true;
    for (const auto& f : seen) {
        if (!first) serialised += String(",");
        serialised += f;
        first = false;
    }
    prefs.begin(NAMESPACE, false);
    prefs.putString(KEY_SEEN, serialised);
    prefs.end();
}

bool O2RingState::hasSeen(const String& filename) const {
    return seen.count(filename) > 0;
}

void O2RingState::markSeen(const String& filename) {
    seen.insert(filename);
}
