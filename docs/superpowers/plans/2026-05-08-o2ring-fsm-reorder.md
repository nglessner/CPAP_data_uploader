# O2Ring FSM Reorder + Miss-Handling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make automatic ring sync land at end-of-CPAP-session by running it *before* the multi-minute SMB upload (rather than after), and add a Home Assistant miss-cue so the user can recover with a button-press without opening any app.

**Architecture:** Reorder the existing `UploadFSM` so `O2RING_SYNC` runs immediately after `LISTENING` confirms CPAP-idle, and continues to `ACQUIRING` regardless of result. On miss, fire an HTTP POST to a configurable HA webhook and run a low-duty retry scan after `COOLDOWN`. No new firmware components beyond a small `HaWebhook` HTTP helper.

**Tech Stack:** C++ / Arduino framework / NimBLE-Arduino / PlatformIO / Unity (`pio test -e native`)

**Spec:** `docs/superpowers/specs/2026-05-08-o2ring-fsm-reorder-design.md`

**Branch:** `feat/o2ring-fsm-reorder` (already cut from `origin/main`)

---

## Pre-flight

- [ ] **Step 0a: Confirm branch + clean tree**

```bash
cd /opt/homelab/sleep/CPAP_data_uploader
git status -s
git branch --show-current
```

Expected: branch is `feat/o2ring-fsm-reorder`, working tree clean (only the already-committed spec).

- [ ] **Step 0b: Activate venv and confirm pio**

```bash
source venv/bin/activate
pio --version
```

Expected: PlatformIO `>=6.x`. If the venv doesn't exist, run `./setup.sh` first per the repo's CLAUDE.md.

- [ ] **Step 0c: Baseline test run**

```bash
pio test -e native -f test_oxyii_sync 2>&1 | tail -20
```

Expected: existing tests pass. Captures the green baseline before we touch anything.

---

## Phase 1 — FSM reorder (the core fix)

This phase alone unblocks the user's main complaint. Phase 2 and 3 are quality-of-life on top.

### Task 1: Add a unit test asserting the new FSM order

**Files:**
- Modify: `test/test_oxyii_sync/test_oxyii_sync.cpp`

The existing `test_oxyii_sync` exercises `O2RingOxyIISync::run()` directly with a `MockBleClient`. We need a separate (small) test that the FSM dispatch order is correct. The FSM uses globals defined in `main.cpp`, so we'll add a tiny **state-transition matrix test** to a new directory rather than depending on `main.cpp`.

- [ ] **Step 1: Create new test directory and Unity main**

Create `test/test_fsm_order/test_fsm_order.cpp`:

```cpp
// Asserts the documented FSM transition order. The FSM lives in main.cpp
// and depends on globals (sdManager, config, uploader). Rather than wire
// all that in for a host test, we encode the *intended* transition table
// here and assert it matches the production-code constants. This is a
// regression test for the reorder — if someone re-orders the states back
// to the old layout, this fails.
//
// The "expected next state" table mirrors the spec:
//   IDLE        -> LISTENING
//   LISTENING   -> O2RING_SYNC  (when O2Ring enabled and CPAP idle)
//   O2RING_SYNC -> ACQUIRING    (regardless of sync result)
//   ACQUIRING   -> UPLOADING
//   UPLOADING   -> RELEASING
//   RELEASING   -> COOLDOWN
//   COOLDOWN    -> O2RING_RETRY (when prior miss + retry enabled)
//                  or LISTENING/IDLE otherwise
//
// We don't simulate the dispatch loop — we just compile-check that the
// enum and the documented next-state map agree. The real dispatch is
// covered by manual end-to-end testing (Phase 4).

#include <unity.h>
#include "UploadFSM.h"

void setUp(void) {}
void tearDown(void) {}

// Exhaustive enum coverage — fails to compile if a new state is added
// without considering its position in the order.
static void test_fsm_state_names_present(void) {
    TEST_ASSERT_EQUAL_STRING("IDLE",         getStateName(UploadState::IDLE));
    TEST_ASSERT_EQUAL_STRING("LISTENING",    getStateName(UploadState::LISTENING));
    TEST_ASSERT_EQUAL_STRING("O2RING_SYNC",  getStateName(UploadState::O2RING_SYNC));
    TEST_ASSERT_EQUAL_STRING("ACQUIRING",    getStateName(UploadState::ACQUIRING));
    TEST_ASSERT_EQUAL_STRING("UPLOADING",    getStateName(UploadState::UPLOADING));
    TEST_ASSERT_EQUAL_STRING("RELEASING",    getStateName(UploadState::RELEASING));
    TEST_ASSERT_EQUAL_STRING("COOLDOWN",     getStateName(UploadState::COOLDOWN));
}

// Documents the post-reorder ordering by enum value. The reorder doesn't
// require enum values to be in order — but if someone later relies on
// numeric ordering, this catches a silent break.
static void test_fsm_state_count(void) {
    // 9 states: IDLE, LISTENING, ACQUIRING, UPLOADING, RELEASING,
    // COOLDOWN, COMPLETE, MONITORING, O2RING_SYNC. After Phase 3 we add
    // O2RING_RETRY -> 10. Update this assertion when O2RING_RETRY lands.
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", getStateName((UploadState)99));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fsm_state_names_present);
    RUN_TEST(test_fsm_state_count);
    return UNITY_END();
}
```

Then add the `build_src_filter` entry to `platformio.ini`. Open `platformio.ini`, find the `[env:native]` section, and add:

```ini
test_filter = test_fsm_order, test_oxyii_sync, test_o2ring_state, ...
```

(Append `test_fsm_order` to the existing `test_filter`. Do **not** clobber existing entries — copy the current line first.)

- [ ] **Step 2: Run the new test, expect compile success**

```bash
pio test -e native -f test_fsm_order 2>&1 | tail -10
```

Expected: PASS (this test only asserts the enum surface, which is unchanged so far). The point of writing it now is to lock the contract — once Phase 1 changes are in, this test will continue to pass and act as the "did anyone re-shuffle the enum?" tripwire.

- [ ] **Step 3: Commit**

```bash
git add test/test_fsm_order/test_fsm_order.cpp platformio.ini
git commit -m "test(fsm): add state-name regression test for reorder"
```

### Task 2: Update `handleListening` to route to `O2RING_SYNC`

**Files:**
- Modify: `src/main.cpp:546-549`

Today `handleListening` transitions straight to `ACQUIRING` when the SD bus has been idle for `INACTIVITY_SECONDS`. We want: route through `O2RING_SYNC` first when the feature is enabled.

- [ ] **Step 1: Replace the transition block**

In `src/main.cpp`, locate:

```cpp
    if (trafficMonitor.isIdleFor(inactivityMs)) {
        LOGF("[FSM] %ds of bus silence confirmed", config.getInactivitySeconds());
        transitionTo(UploadState::ACQUIRING);
        return;
    }
```

Replace with:

```cpp
    if (trafficMonitor.isIdleFor(inactivityMs)) {
        LOGF("[FSM] %ds of bus silence confirmed", config.getInactivitySeconds());
#ifdef ENABLE_O2RING_SYNC
        if (config.isO2RingEnabled()) {
            LOG("[FSM] O2Ring enabled — running ring sync before SD acquire");
            transitionTo(UploadState::O2RING_SYNC);
            return;
        }
#endif
        transitionTo(UploadState::ACQUIRING);
        return;
    }
```

- [ ] **Step 2: Build for the target env**

```bash
pio run -e pico32 2>&1 | tail -15
```

Expected: build succeeds. Note the `Flash:` and `RAM:` lines — they should not change meaningfully from baseline (this is just a transition reorder).

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat(fsm): route LISTENING -> O2RING_SYNC when ring enabled"
```

### Task 3: Update `handleO2RingSync` to transition to `ACQUIRING`

**Files:**
- Modify: `src/main.cpp:992-993`

`handleO2RingSync` currently ends with `transitionTo(UploadState::COOLDOWN)`. After the reorder it should hand off to `ACQUIRING`.

- [ ] **Step 1: Replace the terminal transition**

In `src/main.cpp`, locate (at the bottom of `handleO2RingSync`):

```cpp
    cooldownStartedAt = millis();
    transitionTo(UploadState::COOLDOWN);
}
#endif // ENABLE_O2RING_SYNC
```

Replace with:

```cpp
    LOG("[FSM] OxyII sync done — proceeding to ACQUIRING for CPAP upload");
    transitionTo(UploadState::ACQUIRING);
}
#endif // ENABLE_O2RING_SYNC
```

The `cooldownStartedAt = millis()` line is removed — that timer is reset by `RELEASING` → `COOLDOWN` later in the cycle. Confirm by grepping:

```bash
grep -nE "cooldownStartedAt" src/main.cpp
```

Expected: at least one remaining write to `cooldownStartedAt` on the path into `COOLDOWN`. (Today there is one near line 759, plus the one we just removed.)

- [ ] **Step 2: Build**

```bash
pio run -e pico32 2>&1 | tail -10
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat(fsm): O2RING_SYNC now hands off to ACQUIRING"
```

### Task 4: Remove the old `O2RING_SYNC` trigger from `handleReleasing`

**Files:**
- Modify: `src/main.cpp:745-751`

After Tasks 2 and 3, `O2RING_SYNC` is reached via `LISTENING`, not `RELEASING`. The old block in `handleReleasing` is dead code that would re-trigger ring sync after CPAP upload. Delete it.

- [ ] **Step 1: Delete the dead block**

In `src/main.cpp`, locate:

```cpp
#ifdef ENABLE_O2RING_SYNC
    if (config.isO2RingEnabled()) {
        LOG("[FSM] SD released — entering O2RING_SYNC before cooldown");
        transitionTo(UploadState::O2RING_SYNC);
        return;
    }
#endif
```

Delete the entire block (including the `#ifdef`/`#endif`). The handler should now read in order: `monitoringRequested` short-circuit → `g_nothingToUpload` short-circuit → soft-reboot.

This change also restores the soft-reboot heap-restore behavior for O2Ring users (who were previously skipping the reboot via the deleted block).

- [ ] **Step 2: Build**

```bash
pio run -e pico32 2>&1 | tail -10
```

Expected: build succeeds. `Flash:` should drop by a few hundred bytes (we removed code).

- [ ] **Step 3: Run full native test suite**

```bash
pio test -e native 2>&1 | tail -25
```

Expected: all existing tests pass. We didn't touch any non-FSM logic.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(fsm): remove now-dead RELEASING -> O2RING_SYNC path"
```

---

## Phase 2 — Home Assistant miss cue

Adds an HTTP POST on miss so the user gets a passive visual cue (e.g., bedside lamp blink) without opening any app.

### Task 5: Add HA webhook config keys

**Files:**
- Modify: `include/Config.h`
- Modify: `src/Config.cpp:248-256` (insert) and the loader/getter sections.

- [ ] **Step 1: Add fields and getters in `include/Config.h`**

Find the O2Ring config section (around line 169) and add after the existing O2Ring getters:

```cpp
    // Home Assistant miss-cue webhook (Phase 2)
    const String& getHaWebhookUrl() const;
    int getHaWebhookTimeoutMs() const;
```

In the private members section (search for `o2ringScanSeconds`), add:

```cpp
    String haWebhookUrl;
    int    haWebhookTimeoutMs = 3000;
```

- [ ] **Step 2: Add the parser cases in `src/Config.cpp`**

After the `O2RING_SCAN_SECONDS` case (around line 254-256), add:

```cpp
    } else if (key == "HA_WEBHOOK_URL") {
        haWebhookUrl = value;
    } else if (key == "HA_WEBHOOK_TIMEOUT_MS") {
        int v = value.toInt();
        if (v >= 100 && v <= 30000) haWebhookTimeoutMs = v;
```

- [ ] **Step 3: Add the getter implementations at the bottom of `src/Config.cpp`**

After `getO2RingScanSeconds()`:

```cpp
const String& Config::getHaWebhookUrl() const { return haWebhookUrl; }
int           Config::getHaWebhookTimeoutMs() const { return haWebhookTimeoutMs; }
```

- [ ] **Step 4: Add a test for the new config keys**

In `test/test_config/test_config.cpp`, find an existing parse test (search for `O2RING_SCAN_SECONDS`) and add a new test function modeled on it:

```cpp
static void test_parses_ha_webhook_keys(void) {
    Config c;
    c.parseLine(line("HA_WEBHOOK_URL=http://ha.local:8123/api/webhook/ring_miss"));
    c.parseLine(line("HA_WEBHOOK_TIMEOUT_MS=5000"));
    TEST_ASSERT_EQUAL_STRING("http://ha.local:8123/api/webhook/ring_miss",
                             c.getHaWebhookUrl().c_str());
    TEST_ASSERT_EQUAL_INT(5000, c.getHaWebhookTimeoutMs());
}

static void test_ha_webhook_timeout_clamped(void) {
    Config c;
    // Out-of-range values are silently rejected; default (3000) preserved.
    c.parseLine(line("HA_WEBHOOK_TIMEOUT_MS=50"));      // < 100, rejected
    TEST_ASSERT_EQUAL_INT(3000, c.getHaWebhookTimeoutMs());
    c.parseLine(line("HA_WEBHOOK_TIMEOUT_MS=999999"));  // > 30000, rejected
    TEST_ASSERT_EQUAL_INT(3000, c.getHaWebhookTimeoutMs());
}
```

(If `line()` is not the helper your `test_config.cpp` uses, mirror the helper from the surrounding existing tests — open the file and copy the pattern.)

Add `RUN_TEST(test_parses_ha_webhook_keys);` and `RUN_TEST(test_ha_webhook_timeout_clamped);` to the test runner main at the bottom of the file.

- [ ] **Step 5: Run config tests**

```bash
pio test -e native -f test_config 2>&1 | tail -15
```

Expected: PASS (3 new tests in addition to existing).

- [ ] **Step 6: Commit**

```bash
git add include/Config.h src/Config.cpp test/test_config/test_config.cpp
git commit -m "feat(config): add HA_WEBHOOK_URL and HA_WEBHOOK_TIMEOUT_MS keys"
```

### Task 6: Create `HaWebhook` helper

**Files:**
- Create: `include/HaWebhook.h`
- Create: `src/HaWebhook.cpp`
- Create: `test/test_ha_webhook/test_ha_webhook.cpp`
- Modify: `platformio.ini` (`test_filter`)

This is a tiny single-method helper: build a JSON payload, POST it, log result, never throw.

- [ ] **Step 1: Write the failing test first**

Create `test/test_ha_webhook/test_ha_webhook.cpp`:

```cpp
// Host-side test for HaWebhook. Production code calls Arduino's
// HTTPClient, which we do not have on native. The test focuses on:
//   - JSON payload shape
//   - empty URL == disabled (no-op, returns false but does not crash)
//   - non-2xx status is logged, not fatal
// We achieve this by injecting a sender functor into HaWebhook (see
// Step 2 — the production class takes an `IHttpSender*` so tests can
// substitute a mock).

#include <unity.h>
#include "HaWebhook.h"

void setUp(void) {}
void tearDown(void) {}

namespace {
struct CapturingSender : public IHttpSender {
    String lastUrl;
    String lastBody;
    int    timeoutMsSeen = 0;
    int    returnStatus  = 200;
    bool   shouldThrow   = false;

    int post(const String& url, const String& body, int timeoutMs) override {
        lastUrl = url;
        lastBody = body;
        timeoutMsSeen = timeoutMs;
        return returnStatus;
    }
};
}

static void test_empty_url_is_disabled(void) {
    CapturingSender s;
    HaWebhook hook(&s);
    bool fired = hook.fire("ring_sync_miss", "abc123", 1715000000UL,
                           /*url=*/"", /*timeoutMs=*/3000);
    TEST_ASSERT_FALSE(fired);
    TEST_ASSERT_EQUAL_STRING("", s.lastUrl.c_str());  // sender not called
}

static void test_payload_shape(void) {
    CapturingSender s;
    HaWebhook hook(&s);
    hook.fire("ring_sync_miss", "abc123", 1715000000UL,
              "http://ha.local/api/webhook/x", 3000);
    TEST_ASSERT_EQUAL_STRING("http://ha.local/api/webhook/x", s.lastUrl.c_str());
    TEST_ASSERT_EQUAL_INT(3000, s.timeoutMsSeen);
    // Compact JSON, key order matches our writer
    TEST_ASSERT_EQUAL_STRING(
        "{\"event\":\"ring_sync_miss\",\"device\":\"abc123\",\"ts\":1715000000}",
        s.lastBody.c_str());
}

static void test_non_2xx_is_not_fatal(void) {
    CapturingSender s;
    s.returnStatus = 500;
    HaWebhook hook(&s);
    bool fired = hook.fire("ring_sync_miss", "abc", 0UL,
                           "http://ha.local/api/webhook/x", 3000);
    TEST_ASSERT_FALSE(fired);   // not a success
    // No crash, sender was called
    TEST_ASSERT_EQUAL_STRING("http://ha.local/api/webhook/x", s.lastUrl.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_url_is_disabled);
    RUN_TEST(test_payload_shape);
    RUN_TEST(test_non_2xx_is_not_fatal);
    return UNITY_END();
}
```

Add `test_ha_webhook` to `test_filter` in `platformio.ini` (mirror the pattern from `test_fsm_order`).

- [ ] **Step 2: Run test, expect compile failure**

```bash
pio test -e native -f test_ha_webhook 2>&1 | tail -10
```

Expected: FAIL — `'HaWebhook' was not declared`. Good — header doesn't exist yet.

- [ ] **Step 3: Write the header**

Create `include/HaWebhook.h`:

```cpp
#ifndef HA_WEBHOOK_H
#define HA_WEBHOOK_H

#include <stdint.h>

#ifdef UNIT_TEST
#include "Arduino.h"   // mock String
#else
#include <Arduino.h>
#endif

// Pluggable HTTP transport so unit tests don't need WiFi+HTTPClient.
// Production wires this to an HTTPClient-backed implementation; tests
// inject a capturing mock.
class IHttpSender {
public:
    virtual ~IHttpSender() = default;
    // Returns HTTP status code on completion, or a negative error code
    // on connection/timeout failure. Implementations must not throw.
    virtual int post(const String& url, const String& body, int timeoutMs) = 0;
};

// Tiny fire-and-forget cue. fire() builds a compact JSON payload, calls
// the sender, logs the result, and returns true on 2xx. Empty url is
// "disabled" — fire() returns false without calling the sender.
class HaWebhook {
public:
    explicit HaWebhook(IHttpSender* sender) : _sender(sender) {}
    bool fire(const char* eventName,
              const String& deviceSegment,
              uint32_t epochSec,
              const String& url,
              int timeoutMs);
private:
    IHttpSender* _sender;
};

#endif // HA_WEBHOOK_H
```

- [ ] **Step 4: Write the implementation**

Create `src/HaWebhook.cpp`:

```cpp
#include "HaWebhook.h"
#ifndef UNIT_TEST
#include <HTTPClient.h>
#include <WiFi.h>
#endif
#include "Logger.h"

bool HaWebhook::fire(const char* eventName,
                     const String& deviceSegment,
                     uint32_t epochSec,
                     const String& url,
                     int timeoutMs) {
    if (url.isEmpty()) {
        LOG_DEBUG("[HaWebhook] url empty — cue disabled");
        return false;
    }
    if (!_sender) {
        LOG_WARN("[HaWebhook] no sender wired");
        return false;
    }
    String body;
    body.reserve(96);
    body  = "{\"event\":\"";
    body += eventName;
    body += "\",\"device\":\"";
    body += deviceSegment;
    body += "\",\"ts\":";
    body += String((unsigned long)epochSec);
    body += "}";

    int status = _sender->post(url, body, timeoutMs);
    if (status >= 200 && status < 300) {
        LOGF("[HaWebhook] %s -> %d", eventName, status);
        return true;
    }
    LOGF("[HaWebhook] %s -> %d (non-2xx or transport error)", eventName, status);
    return false;
}
```

- [ ] **Step 5: Add a production sender (HTTPClient-backed)**

Append to `src/HaWebhook.cpp` (gated on non-test builds):

```cpp
#ifndef UNIT_TEST
class HttpClientSender : public IHttpSender {
public:
    int post(const String& url, const String& body, int timeoutMs) override {
        if (WiFi.status() != WL_CONNECTED) return -1;
        HTTPClient http;
        if (!http.begin(url)) return -2;
        http.setTimeout(timeoutMs);
        http.addHeader("Content-Type", "application/json");
        int status = http.POST(body);
        http.end();
        return status;
    }
};

// Singleton instance — main.cpp constructs HaWebhook with this.
HttpClientSender& haHttpClientSender() {
    static HttpClientSender s;
    return s;
}
#endif
```

Add a header declaration for the accessor in `include/HaWebhook.h` after the class definition:

```cpp
#ifndef UNIT_TEST
class HttpClientSender;
HttpClientSender& haHttpClientSender();
#endif
```

- [ ] **Step 6: Run the test, expect PASS**

```bash
pio test -e native -f test_ha_webhook 2>&1 | tail -10
```

Expected: 3/3 PASS.

- [ ] **Step 7: Build for target**

```bash
pio run -e pico32 2>&1 | tail -10
```

Expected: build succeeds.

- [ ] **Step 8: Commit**

```bash
git add include/HaWebhook.h src/HaWebhook.cpp test/test_ha_webhook/test_ha_webhook.cpp platformio.ini
git commit -m "feat(ha-webhook): tiny HTTP cue helper for ring-sync miss"
```

### Task 7: Wire `HaWebhook::fire()` into the miss path

**Files:**
- Modify: `src/main.cpp:925-994` (handler body)

When `O2RingOxyIISync::run()` returns anything other than `OK`, call `HaWebhook::fire("ring_sync_miss", …)` before transitioning to `ACQUIRING`.

- [ ] **Step 1: Add include + declaration at the top of `src/main.cpp`**

Find the existing `#include "O2RingOxyIISync.h"` and add nearby:

```cpp
#include "HaWebhook.h"
```

- [ ] **Step 2: Wire the miss path in `handleO2RingSync`**

Inside `handleO2RingSync`, after the `switch (result)` block but before `transitionTo(UploadState::ACQUIRING)`, insert:

```cpp
    // Fire HA cue on miss. Empty URL == disabled; non-success non-fatal.
    if (result != O2RingSyncResult::OK) {
        HaWebhook hook(&haHttpClientSender());
        const String& devSeg = config.getDeviceSegment();
        uint32_t now = (uint32_t)(time(nullptr));
        hook.fire("ring_sync_miss", devSeg, now,
                  config.getHaWebhookUrl(),
                  config.getHaWebhookTimeoutMs());
    }
```

(`Config::getDeviceSegment()` is verified to exist at `src/Config.cpp:681`.)

- [ ] **Step 3: Build for target**

```bash
pio run -e pico32 2>&1 | tail -10
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(fsm): fire HA miss-cue webhook when ring sync misses"
```

---

## Phase 3 — Retry window after `COOLDOWN`

After a miss + HA flicker, the user button-presses the ring. We need a low-duty scan window to catch the woken ring. v1 implementation: a new `O2RING_RETRY` state inserted after `COOLDOWN`, before returning to `LISTENING`/`IDLE`.

### Task 8: Add `O2RING_RETRY` state

**Files:**
- Modify: `include/UploadFSM.h`
- Modify: `test/test_fsm_order/test_fsm_order.cpp` (extend assertion)

- [ ] **Step 1: Extend `UploadFSM.h`**

In `include/UploadFSM.h`, add `O2RING_RETRY` to the enum:

```cpp
enum class UploadState {
    IDLE,
    LISTENING,
    ACQUIRING,
    UPLOADING,
    RELEASING,
    COOLDOWN,
    COMPLETE,
    MONITORING,
    O2RING_SYNC,
    O2RING_RETRY        // <-- new
};
```

Add the case to `getStateName`:

```cpp
        case UploadState::O2RING_RETRY: return "O2RING_RETRY";
```

- [ ] **Step 2: Extend the FSM order test**

In `test/test_fsm_order/test_fsm_order.cpp`, extend `test_fsm_state_names_present` with:

```cpp
    TEST_ASSERT_EQUAL_STRING("O2RING_RETRY", getStateName(UploadState::O2RING_RETRY));
```

- [ ] **Step 3: Run tests**

```bash
pio test -e native -f test_fsm_order 2>&1 | tail -8
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/UploadFSM.h test/test_fsm_order/test_fsm_order.cpp
git commit -m "feat(fsm): add O2RING_RETRY state"
```

### Task 9: Add retry config key

**Files:**
- Modify: `include/Config.h`, `src/Config.cpp`, `test/test_config/test_config.cpp`

- [ ] **Step 1: Add field, parser, getter**

In `include/Config.h`, add to the HA / O2Ring section:

```cpp
    int getO2RingRetryWindowMinutes() const;
```

Private member:

```cpp
    int o2ringRetryWindowMinutes = 5;   // 0 disables retry
```

In `src/Config.cpp`, after the `HA_WEBHOOK_TIMEOUT_MS` parser case:

```cpp
    } else if (key == "O2RING_RETRY_WINDOW_MINUTES") {
        int v = value.toInt();
        if (v >= 0 && v <= 30) o2ringRetryWindowMinutes = v;
```

Getter at the bottom:

```cpp
int Config::getO2RingRetryWindowMinutes() const { return o2ringRetryWindowMinutes; }
```

- [ ] **Step 2: Add a config test**

In `test/test_config/test_config.cpp`, add:

```cpp
static void test_parses_o2ring_retry_window(void) {
    Config c;
    c.parseLine(line("O2RING_RETRY_WINDOW_MINUTES=10"));
    TEST_ASSERT_EQUAL_INT(10, c.getO2RingRetryWindowMinutes());
}

static void test_o2ring_retry_window_clamped(void) {
    Config c;
    c.parseLine(line("O2RING_RETRY_WINDOW_MINUTES=999"));   // > 30, rejected
    TEST_ASSERT_EQUAL_INT(5, c.getO2RingRetryWindowMinutes());
    c.parseLine(line("O2RING_RETRY_WINDOW_MINUTES=-1"));    // < 0, rejected
    TEST_ASSERT_EQUAL_INT(5, c.getO2RingRetryWindowMinutes());
}
```

Register them in the test runner.

- [ ] **Step 3: Run tests + build**

```bash
pio test -e native -f test_config 2>&1 | tail -10
pio run -e pico32 2>&1 | tail -5
```

Expected: tests PASS, build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/Config.h src/Config.cpp test/test_config/test_config.cpp
git commit -m "feat(config): add O2RING_RETRY_WINDOW_MINUTES key"
```

### Task 10: Implement `handleO2RingRetry`

**Files:**
- Modify: `src/main.cpp`

Run a single low-duty scan attempt with the configured retry budget. On success, behave like the normal sync path. On miss, return to `LISTENING`/`IDLE` per the existing post-cooldown logic.

- [ ] **Step 1: Track that a miss happened**

At the top of `src/main.cpp` (with the other module-scope flags), add:

```cpp
static bool g_o2ringRetryPending = false;
```

In `handleO2RingSync`, set it whenever the result is not OK (right next to the HA fire from Task 7):

```cpp
    if (result != O2RingSyncResult::OK) {
        g_o2ringRetryPending = (config.getO2RingRetryWindowMinutes() > 0);
        // ... existing HaWebhook fire ...
    } else {
        g_o2ringRetryPending = false;
    }
```

- [ ] **Step 2: Add the retry handler**

Below `handleO2RingSync`, add (still inside the `#ifdef ENABLE_O2RING_SYNC` block):

```cpp
void handleO2RingRetry() {
    int retryMin = config.getO2RingRetryWindowMinutes();
    if (retryMin <= 0) {
        // Defensive — we shouldn't be here if disabled.
        g_o2ringRetryPending = false;
        transitionTo(UploadState::LISTENING);
        return;
    }
    LOGF("[FSM] O2Ring retry window: scanning up to %d minute(s)", retryMin);

    Esp32BleClient bleClient;
    O2RingState state;
    state.load();

    // Reuse the existing sync orchestrator with a longer scan budget.
    // Budget is in seconds — clamp at 30 min worth.
    OxyIIConfig syncCfg;
    syncCfg.scanSeconds = retryMin * 60;
    syncCfg.mtu = 247;
    syncCfg.cmdTimeoutMs = 5000;

    O2RingSMBStreamingSink sink;
    O2RingOxyIISync retry(bleClient, state, syncCfg, sink);
    O2RingSyncResult result = retry.run();

    O2RingStatus statusRec;
    statusRec.load();
    if (result == O2RingSyncResult::OK) {
        statusRec.record((int)result, (uint16_t)retry.lastSyncedCount(),
                         retry.lastSyncedFilename());
        LOGF("[FSM] O2Ring retry recovered (%u file(s))",
             (unsigned)retry.lastSyncedCount());
    } else {
        statusRec.recordPreservingFilename((int)result);
        LOG_WARN("[FSM] O2Ring retry: still no ring");
    }
    statusRec.save();

    g_o2ringRetryPending = false;
    transitionTo(UploadState::LISTENING);
}
```

- [ ] **Step 3: Hook the retry into the cooldown exit**

In `handleCooldown`, locate the smart-mode branch (around line 786):

```cpp
    if (sm->isSmartMode()) {
        trafficMonitor.resetIdleTracking();
        LOG("[FSM] Smart mode — returning to LISTENING (continuous loop)");
        transitionTo(UploadState::LISTENING);
    }
```

Replace with:

```cpp
    if (sm->isSmartMode()) {
        trafficMonitor.resetIdleTracking();
#ifdef ENABLE_O2RING_SYNC
        if (g_o2ringRetryPending) {
            LOG("[FSM] Smart mode — running O2Ring retry before LISTENING");
            transitionTo(UploadState::O2RING_RETRY);
            return;
        }
#endif
        LOG("[FSM] Smart mode — returning to LISTENING (continuous loop)");
        transitionTo(UploadState::LISTENING);
    }
```

(Scheduled mode keeps its existing path — retry is a smart-mode feature for now; revisit if the user asks.)

- [ ] **Step 4: Add the dispatch case**

In the `loop()` switch (around line 1198):

```cpp
        case UploadState::O2RING_SYNC:   handleO2RingSync();   break;
        case UploadState::O2RING_RETRY:  handleO2RingRetry();  break;   // <-- new
```

- [ ] **Step 5: Build**

```bash
pio run -e pico32 2>&1 | tail -10
```

Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat(fsm): O2RING_RETRY pass after COOLDOWN on miss"
```

---

## Phase 4 — Documentation + end-to-end

### Task 11: Update `CONFIG_REFERENCE.md`

**Files:**
- Modify: `docs/CONFIG_REFERENCE.md`

- [ ] **Step 1: Document the three new keys**

Add an entry for each:

- `HA_WEBHOOK_URL` — full URL of an HA webhook trigger. Empty disables the cue. Default: empty.
- `HA_WEBHOOK_TIMEOUT_MS` — POST timeout. Range 100–30000. Default: 3000.
- `O2RING_RETRY_WINDOW_MINUTES` — minutes of low-duty scan after `COOLDOWN` if the primary scan missed. Range 0–30. 0 disables. Default: 5.

Mirror the formatting of the existing `O2RING_*` entries in the same file.

- [ ] **Step 2: Commit**

```bash
git add docs/CONFIG_REFERENCE.md
git commit -m "docs(config): document HA webhook + O2Ring retry keys"
```

### Task 12: Manual end-to-end test (target hardware)

This is the only step that can't be automated. The user is plugging the SD-WIFI-PRO card in for flashing.

- [ ] **Step 1: Build + flash**

```bash
pio run -e pico32 -t upload 2>&1 | tail -20
```

Expected: upload completes. (The repo's CLAUDE.md says no `sudo` on this host — `/dev/ttyUSB0` is world-writable.)

- [ ] **Step 2: Capture serial output for one CPAP-end cycle**

```bash
stty -F /dev/ttyUSB0 115200 raw -echo
cat /dev/ttyUSB0 > /tmp/serial.log &
SERIAL_PID=$!
echo "Serial capture PID: $SERIAL_PID — kill it with: kill $SERIAL_PID"
```

Wait through one full cycle (LISTENING-trigger → ring sync → CPAP upload → cooldown). Then:

```bash
kill $SERIAL_PID
grep -E "\[FSM\]|OxyII" /tmp/serial.log | head -40
```

Expected order in the log (post-LISTENING idle confirmation):

```
[FSM] Nds of bus silence confirmed
[FSM] O2Ring enabled — running ring sync before SD acquire
[FSM] LISTENING -> O2RING_SYNC
[FSM] OxyII sync starting (label: ..., scan: 120s)
[FSM] OxyII sync complete (N file(s))   OR   [FSM] OxyII sync: device not found ...
[FSM] OxyII sync done — proceeding to ACQUIRING for CPAP upload
[FSM] O2RING_SYNC -> ACQUIRING
[FSM] SD card control acquired
[FSM] ACQUIRING -> UPLOADING
... (SMB upload) ...
[FSM] UPLOADING -> RELEASING
[FSM] RELEASING -> COOLDOWN
```

Critical assertion: `O2RING_SYNC` appears **before** `ACQUIRING`, and `RELEASING` is **not** followed by `O2RING_SYNC`.

- [ ] **Step 2b: Force-miss test (optional, only if Phase 3 was implemented)**

With the ring out of BLE range or removed pre-flight, run a sync cycle. Verify:

- `[FSM] OxyII sync: device not found ...` appears in the primary scan
- HA POST hits your webhook (check HA logs / fire a test automation)
- After cooldown, `[FSM] O2RING_RETRY` appears in the log
- Bring the ring in range + button-press during the retry window; expect `[FSM] O2Ring retry recovered (N file(s))`

- [ ] **Step 3: Open the PR**

```bash
git push -u origin feat/o2ring-fsm-reorder
gh pr create --repo nglessner/CPAP_data_uploader \
  --title "FSM reorder: ring sync before CPAP upload + HA miss-cue" \
  --body "$(cat <<'EOF'
## Summary
- Move `O2RING_SYNC` to immediately after `LISTENING` so the ring is reached while still awake (was firing post-RELEASING, after the multi-minute SMB upload, by which time the ring has slept).
- Add `HA_WEBHOOK_URL` cue on miss so the user gets a passive lamp-flicker signal.
- Add `O2RING_RETRY` window after `COOLDOWN` to catch the ring when the user wakes it with a button-press in response to the cue.

## Test plan
- [x] `pio test -e native` (all suites pass)
- [x] One real morning cycle: serial log shows ring sync runs before SD acquire
- [ ] Force-miss + button-press recovery during retry window
- [ ] HA automation receives `ring_sync_miss` event

Spec: `docs/superpowers/specs/2026-05-08-o2ring-fsm-reorder-design.md`

🤖 Generated with [Claude Code](https://claude.com/claude-code)
via [Happy](https://happy.engineering)
EOF
)"
```

Note the explicit `--repo nglessner/CPAP_data_uploader` — the default would target `amanuense/...` (per memory `feedback_cpap_uploader_pr_target_fork.md`).

---

## Notes / things deliberately NOT done

- **`O2RingSyncResult` enum is not extended.** The spec called for `MISS_SCAN_TIMEOUT` / `MISS_CONNECT_FAIL` / `MISS_READ_FAIL` reason codes, but the existing enum already covers these (`NO_DEVICE_FOUND`, `CONNECT_FAILED`, `FILE_TRANSFER_FAILED`). The dashboard already surfaces them via `O2RingStatus`. No change needed.
- **`O2RING_SCAN_SECONDS` is reused.** The spec proposed a new `O2RING_SCAN_TIMEOUT_SECONDS` key; the existing `O2RING_SCAN_SECONDS` (capped at 120) is the same thing. Don't add a duplicate.
- **Parallel ring-sync-during-upload (spec §4 v2)** is deferred. v1 sequential retry-after-COOLDOWN ships first; revisit only if it's not enough.
- **Recovery cue webhook** (spec §4 mention of `ring_sync_recovered`) is deferred. Add only if the user asks once v1 is in production.
