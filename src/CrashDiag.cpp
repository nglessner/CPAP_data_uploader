#include "CrashDiag.h"

#include <Arduino.h>
#include <Preferences.h>

#include "Logger.h"

namespace {

constexpr const char* NVS_NAMESPACE = "cpap_diag";
constexpr const char* NVS_KEY_SNAP  = "snap";

// On-NVS layout. Packed so the struct can be putBytes/getBytes round-tripped.
struct __attribute__((packed)) DiagSnapshot {
    uint8_t  schemaVersion;     // 1
    uint8_t  fsmState;          // raw UploadState
    uint8_t  resetReason;       // raw esp_reset_reason_t for THIS boot
    uint8_t  reserved;          // pad for alignment / future use
    uint16_t bootCount;         // monotonic across boots
    uint16_t reserved2;
    uint32_t stampMillis;       // millis() at write time
    uint32_t freeHeap;
    uint32_t maxAlloc;
};
static_assert(sizeof(DiagSnapshot) == 20, "DiagSnapshot must be 20 bytes for stable NVS layout");

constexpr uint8_t SCHEMA_VERSION = 1;
constexpr unsigned long HEARTBEAT_MIN_INTERVAL_MS = 120UL * 1000UL;

CrashDiag::PreviousBoot g_prev{};
DiagSnapshot           g_current{};
bool                   g_inited                 = false;
bool                   g_panicBootFlagThisBoot  = false;
unsigned long          g_lastHeartbeatMs        = 0;

const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_UNKNOWN:   return "UNKNOWN";
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "?";
    }
}

bool isAbnormalReason(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

void writeSnapshot() {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        // NVS write failure is non-fatal; log once and move on. Subsequent
        // calls may succeed if the partition was temporarily contended.
        LOG_WARN("[CrashDiag] NVS open (rw) failed");
        return;
    }
    p.putBytes(NVS_KEY_SNAP, &g_current, sizeof(g_current));
    p.end();
}

} // namespace

namespace CrashDiag {

void begin(esp_reset_reason_t currentReason) {
    if (g_inited) return;
    g_inited = true;

    // --- Read previous-boot snapshot (if any) ---
    Preferences p;
    bool prevLoaded = false;
    DiagSnapshot prev{};

    if (p.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
        size_t got = p.getBytes(NVS_KEY_SNAP, &prev, sizeof(prev));
        if (got == sizeof(prev) && prev.schemaVersion == SCHEMA_VERSION) {
            prevLoaded = true;
        }
        p.end();
    }

    g_prev.valid           = prevLoaded;
    g_prev.resetReason     = currentReason; // also useful to surface caller's read
    g_prev.lastFsmState    = prevLoaded ? prev.fsmState : 0;
    g_prev.lastStampMillis = prevLoaded ? prev.stampMillis : 0;
    g_prev.lastFreeHeap    = prevLoaded ? prev.freeHeap : 0;
    g_prev.lastMaxAlloc    = prevLoaded ? prev.maxAlloc : 0;
    g_prev.bootCount       = prevLoaded ? prev.bootCount : 0;

    // The previous boot's reset reason was stamped on its boot, NOT at death.
    // To learn how the previous boot died, we use esp_reset_reason() of THIS
    // boot (currentReason) — that IS the previous boot's exit cause.
    // We keep prev.resetReason (= the boot before that) for context only.
    esp_reset_reason_t prevDiedAs = currentReason;

    // --- Log a single boot-banner line so postmortems are greppable ---
    if (!prevLoaded) {
        LOG_INFOF("[CrashDiag] first boot or empty NVS (this boot reset=%s)",
                  resetReasonName(currentReason));
    } else {
        LOG_INFOF("[CrashDiag] prev boot #%u died as %s; last_fsm=%u "
                  "stamp_ms=%u free=%u max_alloc=%u",
                  (unsigned)prev.bootCount,
                  resetReasonName(prevDiedAs),
                  (unsigned)prev.fsmState,
                  (unsigned)prev.stampMillis,
                  (unsigned)prev.freeHeap,
                  (unsigned)prev.maxAlloc);
    }

    g_panicBootFlagThisBoot = isAbnormalReason(prevDiedAs);
    if (g_panicBootFlagThisBoot) {
        LOG_WARNF("[CrashDiag] PANIC-BOOT mode active (prev exit=%s) — "
                  "SD bus access will be deferred until extended idle confirmed",
                  resetReasonName(prevDiedAs));
    }

    // --- Prime the current-boot snapshot and stamp it ---
    g_current.schemaVersion = SCHEMA_VERSION;
    g_current.fsmState      = 0; // IDLE
    g_current.resetReason   = (uint8_t)currentReason;
    g_current.reserved      = 0;
    g_current.bootCount     = (uint16_t)(g_prev.bootCount + 1);
    g_current.reserved2     = 0;
    g_current.stampMillis   = millis();
    g_current.freeHeap      = ESP.getFreeHeap();
    g_current.maxAlloc      = ESP.getMaxAllocHeap();
    writeSnapshot();

    g_lastHeartbeatMs = millis();
}

const PreviousBoot& previousBoot() {
    return g_prev;
}

bool wasAbnormalReset() {
    return g_panicBootFlagThisBoot;
}

void stamp(uint8_t fsmState) {
    if (!g_inited) return;
    g_current.fsmState    = fsmState;
    g_current.stampMillis = millis();
    g_current.freeHeap    = ESP.getFreeHeap();
    g_current.maxAlloc    = ESP.getMaxAllocHeap();
    writeSnapshot();
    g_lastHeartbeatMs = millis();
}

void heartbeat(uint8_t fsmState) {
    if (!g_inited) return;
    unsigned long now = millis();
    if (now - g_lastHeartbeatMs < HEARTBEAT_MIN_INTERVAL_MS) {
        return;
    }
    stamp(fsmState);
}

void clearPanicBootFlag() {
    if (g_panicBootFlagThisBoot) {
        LOG_INFO("[CrashDiag] firmware proved stable — clearing panic-boot flag");
        g_panicBootFlagThisBoot = false;
    }
}

} // namespace CrashDiag
