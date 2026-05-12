#ifndef CRASH_DIAG_H
#define CRASH_DIAG_H

// Persistent crash / wedge diagnostics.
//
// Survives panics, task watchdog timeouts, brownouts, and software resets so
// the next boot can answer "what state was the firmware in when it died, and
// what did the heap look like just before?" All writes are NVS-only — never
// touches the SD bus, safe to call before SDCardManager::takeControl().
//
// Backing store: NVS namespace "cpap_diag", single blob key "snap" packed via
// putBytes(). One physical write per stamp regardless of how many fields
// change; wear-leveled by the underlying NVS partition.

#include <stdint.h>
#include <esp_system.h>

namespace CrashDiag {

struct PreviousBoot {
    bool                valid;          // false on first-ever boot
    esp_reset_reason_t  resetReason;    // reset cause that ended the previous boot
    uint8_t             lastFsmState;   // raw UploadState enum value at last stamp
    uint32_t            lastStampMillis;// millis() value at last stamp (within prev boot)
    uint32_t            lastFreeHeap;
    uint32_t            lastMaxAlloc;
    uint16_t            bootCount;      // boot counter from the previous boot
};

// Read previous-boot snapshot from NVS, log it, then stamp the current boot's
// reset reason. Must be called BEFORE any SD bus access — NVS only.
void begin(esp_reset_reason_t currentReason);

// Snapshot of the previous boot, captured during begin(). Stable for the rest
// of this boot's lifetime.
const PreviousBoot& previousBoot();

// True if the previous boot ended in PANIC / TASK_WDT / INT_WDT / WDT /
// BROWNOUT — any reset cause that suggests the firmware may have been
// holding the SD bus when it died. This is the panic-boot gate signal.
bool wasAbnormalReset();

// Stamp current FSM state + heap to NVS. Called from transitionTo() and from
// the main-loop heartbeat. Cheap (single putBytes), no rate-limit applied
// here — caller is responsible for not hammering this from a tight loop.
void stamp(uint8_t fsmState);

// Convenience wrapper for the periodic main-loop heartbeat. Rate-limits
// internally to one NVS write per ~120s. Pass the current FSM state so the
// stamp survives even when no transition has happened.
void heartbeat(uint8_t fsmState);

// Called once the firmware demonstrates it can complete a clean state
// transition after a panic boot (e.g. enters LISTENING successfully). Clears
// wasAbnormalReset() for the rest of this boot so normal SD bus operation
// resumes. The on-NVS prev-reset code is NOT cleared — only this boot's
// in-RAM panic-boot flag is.
void clearPanicBootFlag();

} // namespace CrashDiag

#endif // CRASH_DIAG_H
