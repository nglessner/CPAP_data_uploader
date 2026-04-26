# O2Ring CONNECT_FAILED vs DEVICE_NOT_FOUND Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `O2RingSyncResult::DEVICE_NOT_FOUND` so the enum reports `CONNECT_FAILED` when scan succeeded but the GATT connection didn't establish — keeping `DEVICE_NOT_FOUND` strictly for the empty-scan case.

**Architecture:** Add a single accessor `IBleClient::wasDeviceFound()` that returns whether the most recent `connect()` saw a name-prefix match. `O2RingSync::run()` queries it after `connect()` returns false to choose between the two enum values. The new variant is **appended** to the end of `O2RingSyncResult` so persisted int values in NVS keep their meaning.

**Tech Stack:** C++ on Arduino/ESP32 framework, NimBLE-Arduino, PlatformIO, Unity unit tests in the `native` env.

**Spec:** `docs/superpowers/specs/2026-04-26-o2ring-connect-failed-result-design.md`

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `include/IBleClient.h` | Modify | Add pure virtual `wasDeviceFound()` |
| `include/Esp32BleClient.h` | Modify | Declare `_lastScanFound` member + `wasDeviceFound()` override |
| `src/Esp32BleClient.cpp` | Modify | Reset `_lastScanFound` at top of `connect()`, set it true on name-prefix match |
| `test/mocks/MockBleClient.h` | Modify | Add `deviceFoundFlag` member + `wasDeviceFound()` override |
| `include/O2RingSync.h` | Modify | Append `CONNECT_FAILED` to `O2RingSyncResult` enum |
| `src/O2RingSync.cpp` | Modify | Branch on `wasDeviceFound()` after failed `connect()` |
| `test/test_o2ring_sync/test_o2ring_sync.cpp` | Modify | Update 3 existing tests to set `deviceFoundFlag=false`; add 2 new tests for CONNECT_FAILED path |
| `src/CpapWebServer.cpp` | Modify | Add `CONNECT_FAILED` case in `o2ringResultName()` |
| `src/main.cpp` | Modify | Add `CONNECT_FAILED` arm in `handleO2RingSync()` switch |

---

## Task 1: Plumb `wasDeviceFound()` through the BLE interface and implementations

This is a compile-only task. No behavior changes; just make the new method exist on `IBleClient`, `Esp32BleClient`, and `MockBleClient` so subsequent tasks can call it. Without this step, adding the pure virtual method would break the `pico32` build immediately.

**Files:**
- Modify: `include/IBleClient.h`
- Modify: `include/Esp32BleClient.h`
- Modify: `src/Esp32BleClient.cpp`
- Modify: `test/mocks/MockBleClient.h`

- [ ] **Step 1: Add pure virtual to `IBleClient`**

In `include/IBleClient.h`, after the `isConnected()` declaration (line 25), add:

```cpp
    // Returns true iff the most recent connect() observed a name-prefix
    // match in scan results — regardless of whether the subsequent GATT
    // steps succeeded. Reset to false at the start of each connect()
    // call. Callers query it only after connect() returned false to
    // distinguish empty-scan from post-scan failure.
    virtual bool wasDeviceFound() const = 0;
```

- [ ] **Step 2: Add member + override declaration to `Esp32BleClient`**

In `include/Esp32BleClient.h`, in the `public:` block after `isConnected()` (line 26), add:

```cpp
    bool wasDeviceFound() const override { return _lastScanFound; }
```

In the `private:` block after `bool _connected;` (line 40), add:

```cpp
    bool _lastScanFound = false;
```

- [ ] **Step 3: Add stub initialization to `Esp32BleClient::connect()`**

This step only resets the flag at function entry; the "set true on scan match" line is added in Task 6. Splitting it this way keeps Task 1 a pure infrastructure commit.

In `src/Esp32BleClient.cpp`, immediately after the `connect()` function header at line 45 and before `initStack();` at line 46, add:

```cpp
    _lastScanFound = false;
```

- [ ] **Step 4: Add field + override to `MockBleClient`**

In `test/mocks/MockBleClient.h`, after `bool connected = false;` (line 24), add:

```cpp
    // Default true: a mock with shouldConnect=true represents a found
    // and connected device. Tests that simulate genuine "scan miss"
    // must set this to false explicitly.
    bool deviceFoundFlag = true;
```

After the `isConnected()` override (line 54), add:

```cpp
    bool wasDeviceFound() const override { return deviceFoundFlag; }
```

- [ ] **Step 5: Verify native test build still compiles and passes**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 7 existing tests pass. (The new method exists on the mock with default `true`, but no caller queries it yet.)

- [ ] **Step 6: Verify pico32 build still compiles**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. No size regression beyond noise (one bool member + one inline accessor).

- [ ] **Step 7: Commit**

```bash
git add include/IBleClient.h include/Esp32BleClient.h src/Esp32BleClient.cpp test/mocks/MockBleClient.h
git commit -m "feat(ble): add IBleClient::wasDeviceFound() accessor

Returns whether the most recent connect() observed a matching device in
scan results, regardless of whether GATT steps then succeeded. Mock
defaults to true; Esp32BleClient resets the flag at connect() entry.
Wiring of the post-scan true branch lands with the O2RingSync change."
```

---

## Task 2: Append `CONNECT_FAILED` variant to the enum

Compile-only change. The new value is appended after `NOTHING_TO_SYNC` so existing persisted int values in NVS retain their meaning (no migration needed).

**Files:**
- Modify: `include/O2RingSync.h:13-19`

- [ ] **Step 1: Append the variant**

In `include/O2RingSync.h`, replace the existing enum:

```cpp
enum class O2RingSyncResult {
    OK,
    DEVICE_NOT_FOUND,
    SMB_ERROR,
    BLE_ERROR,
    NOTHING_TO_SYNC
};
```

with:

```cpp
enum class O2RingSyncResult {
    OK,                // 0
    DEVICE_NOT_FOUND,  // 1
    SMB_ERROR,         // 2
    BLE_ERROR,         // 3
    NOTHING_TO_SYNC,   // 4
    CONNECT_FAILED     // 5 — scan succeeded, GATT connect failed
};
```

- [ ] **Step 2: Verify native build passes**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 7 tests still pass. Adding an unused enum variant is invisible at runtime.

- [ ] **Step 3: Verify pico32 build passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. The web handler's `o2ringResultName()` has a `default:` case so the missing arm is non-fatal at compile time (it would return `"UNKNOWN"` if hit).

- [ ] **Step 4: Commit**

```bash
git add include/O2RingSync.h
git commit -m "feat(o2ring): append CONNECT_FAILED to O2RingSyncResult

Reserved for the case where scan succeeded but IBleClient::connect()
returned false. Appended at the end of the enum so existing NVS-
persisted int values keep their meaning. No callers yet."
```

---

## Task 3: Update existing "device not found" tests to set `deviceFoundFlag = false`

Three existing tests drive `mockBle->shouldConnect = false` and expect `DEVICE_NOT_FOUND`. With Task 1 the mock's `deviceFoundFlag` defaults to `true`, so once Task 4 implements the new branching, these tests would silently flip to expecting `CONNECT_FAILED`. Update them now to lock in the genuine-empty-scan intent.

This task is "no-op" today (the new branching doesn't exist yet) but pre-aligns the tests with the next task's behavior change. Tests stay green throughout.

**Files:**
- Modify: `test/test_o2ring_sync/test_o2ring_sync.cpp:62-67, 126-143, 169-194`

- [ ] **Step 1: Update `test_device_not_found_returns_error`**

In `test/test_o2ring_sync/test_o2ring_sync.cpp`, replace:

```cpp
void test_device_not_found_returns_error() {
    mockBle->shouldConnect = false;
    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::DEVICE_NOT_FOUND, (int)result);
}
```

with:

```cpp
void test_device_not_found_returns_error() {
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = false;  // genuine empty-scan
    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::DEVICE_NOT_FOUND, (int)result);
}
```

- [ ] **Step 2: Update `test_status_recorded_on_device_not_found`**

Find the test starting at line 126. After the line `mockBle->shouldConnect = false;`, add immediately below:

```cpp
    mockBle->deviceFoundFlag = false;  // genuine empty-scan
```

- [ ] **Step 3: Update `test_status_preserves_filename_when_pre_info_failure`**

Find the test starting at line 169. After the line `mockBle->shouldConnect = false;`, add immediately below:

```cpp
    mockBle->deviceFoundFlag = false;  // simulate genuine empty-scan to keep DEVICE_NOT_FOUND assertion
```

- [ ] **Step 4: Verify all tests still pass**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 7 tests pass. The new field has no effect yet — `O2RingSync::run()` doesn't query `wasDeviceFound()` until Task 4.

- [ ] **Step 5: Commit**

```bash
git add test/test_o2ring_sync/test_o2ring_sync.cpp
git commit -m "test(o2ring): pin deviceFoundFlag=false on empty-scan tests

Existing DEVICE_NOT_FOUND tests rely on the mock's connect() returning
false. Once O2RingSync queries wasDeviceFound() to choose between the
two failure modes, these tests must explicitly clear deviceFoundFlag
to keep their genuine-empty-scan intent."
```

---

## Task 4: TDD — write failing tests for `CONNECT_FAILED` branching

Write the new tests first, watch them fail, then implement in Task 5.

**Files:**
- Modify: `test/test_o2ring_sync/test_o2ring_sync.cpp`

- [ ] **Step 1: Add the two new tests**

In `test/test_o2ring_sync/test_o2ring_sync.cpp`, immediately before the `int main()` block at line 195, add:

```cpp
void test_connect_failed_returns_error_when_scan_hit_but_connect_failed() {
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = true;  // scan saw the device, GATT rejected
    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::CONNECT_FAILED, (int)result);
}

void test_status_recorded_on_connect_failed() {
    MockTimeState::setTime(1777035100);
    mockBle->shouldConnect = false;
    mockBle->deviceFoundFlag = true;

    O2RingSync sync(cfg, mockBle);
    sync.run();

    O2RingStatus status;
    status.load();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::CONNECT_FAILED,
                          status.getLastResult());
    TEST_ASSERT_EQUAL_UINT32(1777035100u, status.getLastUnix());
    TEST_ASSERT_EQUAL_UINT16(0u, status.getFilesSynced());
}
```

- [ ] **Step 2: Register the new tests in `main()`**

Replace:

```cpp
    RUN_TEST(test_status_preserves_filename_when_pre_info_failure);
    return UNITY_END();
```

with:

```cpp
    RUN_TEST(test_status_preserves_filename_when_pre_info_failure);
    RUN_TEST(test_connect_failed_returns_error_when_scan_hit_but_connect_failed);
    RUN_TEST(test_status_recorded_on_connect_failed);
    return UNITY_END();
```

- [ ] **Step 3: Run the tests and verify they fail**

Run: `pio test -e native -f test_o2ring_sync`

Expected: 2 failures.
- `test_connect_failed_returns_error_when_scan_hit_but_connect_failed`: expected `5` (CONNECT_FAILED), got `1` (DEVICE_NOT_FOUND).
- `test_status_recorded_on_connect_failed`: expected `5`, got `1`.

The other 7 tests still pass.

Do **not** commit yet — leaving the tree in red is intentional; the next task makes them pass.

---

## Task 5: Make the failing tests pass — branch in `O2RingSync::run()`

**Files:**
- Modify: `src/O2RingSync.cpp:129-135`

- [ ] **Step 1: Replace the single failure path**

In `src/O2RingSync.cpp`, replace lines 129–135:

```cpp
    if (!ble->connect(config->getO2RingDeviceName(),
                      config->getO2RingScanSeconds())) {
        LOG_WARN("[O2Ring] Device not found");
        status.recordPreservingFilename((int)O2RingSyncResult::DEVICE_NOT_FOUND);
        status.save();
        return O2RingSyncResult::DEVICE_NOT_FOUND;
    }
```

with:

```cpp
    if (!ble->connect(config->getO2RingDeviceName(),
                      config->getO2RingScanSeconds())) {
        O2RingSyncResult code;
        if (ble->wasDeviceFound()) {
            LOG_WARN("[O2Ring] Device found but connection failed");
            code = O2RingSyncResult::CONNECT_FAILED;
        } else {
            LOG_WARN("[O2Ring] Device not found");
            code = O2RingSyncResult::DEVICE_NOT_FOUND;
        }
        status.recordPreservingFilename((int)code);
        status.save();
        return code;
    }
```

- [ ] **Step 2: Run the tests and verify they pass**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 9 tests pass (7 original + 2 new).

- [ ] **Step 3: Commit**

```bash
git add src/O2RingSync.cpp test/test_o2ring_sync/test_o2ring_sync.cpp
git commit -m "feat(o2ring): return CONNECT_FAILED when scan hit but connect failed

O2RingSync::run() now queries IBleClient::wasDeviceFound() after a
failed connect() and returns CONNECT_FAILED when the scan saw the
device. DEVICE_NOT_FOUND is reserved for the genuine empty-scan case.
Closes #5."
```

---

## Task 6: Wire `_lastScanFound = true` in `Esp32BleClient::connect()`

The Esp32 implementation has the member but never sets it to true on a scan hit, so on real hardware every failed `connect()` would still report `DEVICE_NOT_FOUND` (the flag is stuck at false from Task 1 step 3). This task adds the "set true on match" line. No native test covers this — the mock has its own implementation — so verification is via `pio run -e pico32` plus manual reasoning about `connect()` control flow.

**Files:**
- Modify: `src/Esp32BleClient.cpp:45-66`

- [ ] **Step 1: Set the flag on scan hit**

In `src/Esp32BleClient.cpp`, locate the scan-results loop at lines 55–64:

```cpp
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        if (d.haveName() && String(d.getName().c_str()).startsWith(namePrefix)) {
            targetAddr = d.getAddress();
            found = true;
            LOGF("[O2Ring BLE] Found device: %s (%s)",
                 d.getName().c_str(), d.getAddress().toString().c_str());
            break;
        }
    }
```

Replace `found = true;` with:

```cpp
            found = true;
            _lastScanFound = true;
```

The flag is set at the same point as the existing local `found` boolean, before any GATT step. All subsequent failure paths (service-not-found, characteristic-not-found, registerForNotify failure, GATT connect rejection) leave the flag set to true — they are "found but couldn't talk to it" cases, which is the CONNECT_FAILED definition.

- [ ] **Step 2: Verify pico32 build passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds, no warnings about the new code.

- [ ] **Step 3: Verify native tests still pass**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 9 tests pass. (Native build does not include `Esp32BleClient.cpp` — this is just a sanity check that nothing leaked.)

- [ ] **Step 4: Commit**

```bash
git add src/Esp32BleClient.cpp
git commit -m "feat(ble): set _lastScanFound on name-prefix match

Esp32BleClient::connect() now records whether the scan loop saw a
matching advertisement before any GATT step. Surfaces through
wasDeviceFound() so O2RingSync can return CONNECT_FAILED when the
device was reachable in advertising but the GATT connection failed."
```

---

## Task 7: Surface `CONNECT_FAILED` on the web status card and serial logs

**Files:**
- Modify: `src/CpapWebServer.cpp:1351-1360`
- Modify: `src/main.cpp:837-853`

- [ ] **Step 1: Add the web handler string mapping**

In `src/CpapWebServer.cpp`, replace the `o2ringResultName()` switch (lines 1352–1359):

```cpp
    switch ((O2RingSyncResult)code) {
        case O2RingSyncResult::OK:                return "OK";
        case O2RingSyncResult::DEVICE_NOT_FOUND:  return "DEVICE_NOT_FOUND";
        case O2RingSyncResult::SMB_ERROR:         return "SMB_ERROR";
        case O2RingSyncResult::BLE_ERROR:         return "BLE_ERROR";
        case O2RingSyncResult::NOTHING_TO_SYNC:   return "NOTHING_TO_SYNC";
        default:                                  return "UNKNOWN";
    }
```

with:

```cpp
    switch ((O2RingSyncResult)code) {
        case O2RingSyncResult::OK:                return "OK";
        case O2RingSyncResult::DEVICE_NOT_FOUND:  return "DEVICE_NOT_FOUND";
        case O2RingSyncResult::SMB_ERROR:         return "SMB_ERROR";
        case O2RingSyncResult::BLE_ERROR:         return "BLE_ERROR";
        case O2RingSyncResult::NOTHING_TO_SYNC:   return "NOTHING_TO_SYNC";
        case O2RingSyncResult::CONNECT_FAILED:    return "CONNECT_FAILED";
        default:                                  return "UNKNOWN";
    }
```

- [ ] **Step 2: Add the main.cpp log switch arm**

In `src/main.cpp`, locate the switch at lines 837–853:

```cpp
    switch (result) {
        case O2RingSyncResult::OK:
            LOG("[FSM] O2Ring sync complete");
            break;
        case O2RingSyncResult::NOTHING_TO_SYNC:
            LOG("[FSM] O2Ring sync: nothing new");
            break;
        case O2RingSyncResult::DEVICE_NOT_FOUND:
            LOG_WARN("[FSM] O2Ring sync: device not found (ring not in range?)");
            break;
        case O2RingSyncResult::SMB_ERROR:
            LOG_WARN("[FSM] O2Ring sync: SMB upload failed");
            break;
        case O2RingSyncResult::BLE_ERROR:
            LOG_WARN("[FSM] O2Ring sync: BLE error");
            break;
    }
```

Add a new arm before the closing `}`:

```cpp
        case O2RingSyncResult::CONNECT_FAILED:
            LOG_WARN("[FSM] O2Ring sync: device found but GATT connect failed");
            break;
```

So the full switch becomes:

```cpp
    switch (result) {
        case O2RingSyncResult::OK:
            LOG("[FSM] O2Ring sync complete");
            break;
        case O2RingSyncResult::NOTHING_TO_SYNC:
            LOG("[FSM] O2Ring sync: nothing new");
            break;
        case O2RingSyncResult::DEVICE_NOT_FOUND:
            LOG_WARN("[FSM] O2Ring sync: device not found (ring not in range?)");
            break;
        case O2RingSyncResult::SMB_ERROR:
            LOG_WARN("[FSM] O2Ring sync: SMB upload failed");
            break;
        case O2RingSyncResult::BLE_ERROR:
            LOG_WARN("[FSM] O2Ring sync: BLE error");
            break;
        case O2RingSyncResult::CONNECT_FAILED:
            LOG_WARN("[FSM] O2Ring sync: device found but GATT connect failed");
            break;
    }
```

- [ ] **Step 3: Verify pico32 build passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. The new switch arm silences any `-Wswitch` warnings that would otherwise flag the missing case (gcc emits these only when no `default:` is present; both switches we touched have one explicit or implicit, so this is purely about completeness, not a warning fix).

- [ ] **Step 4: Verify native tests still pass**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 9 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/CpapWebServer.cpp src/main.cpp
git commit -m "feat(o2ring): surface CONNECT_FAILED on status card and FSM log

CpapWebServer's o2ringResultName() now emits \"CONNECT_FAILED\" for the
new code; the dashboard renders the string verbatim with no client-side
mapping. main.cpp handleO2RingSync() prints a distinct log line so the
serial console differentiates scan-miss from GATT-rejection at a
glance."
```

---

## Task 8: Final verification

Sanity-check the final tree against both build flavors and confirm no stray failures from the changes.

- [ ] **Step 1: Run the full native test suite for the touched module**

Run: `pio test -e native -f test_o2ring_sync`

Expected: 9/9 tests pass.

- [ ] **Step 2: Run all native tests as a regression check**

Run: `pio test -e native`

Expected: all suites pass. Tasks 1–7 only added members/cases, so unrelated suites should be unaffected.

- [ ] **Step 3: Build pico32 firmware**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. Note flash/RAM lines for any unexpected jumps; a single bool member + small switch arms should round to noise.

- [ ] **Step 4: Build pico32-ota firmware**

Run: `source venv/bin/activate && pio run -e pico32-ota`

Expected: build fails with the documented "section overflow" because `-DENABLE_O2RING_SYNC` doesn't fit pico32-ota's 1.5MB app slot. **This is expected and not a regression** — it predates this change. If the build *succeeds*, even better, but failure here is not a blocker.

If the failure mode changes (e.g., new linker error unrelated to size), investigate before continuing.

- [ ] **Step 5: Confirm no uncommitted changes**

Run: `git status`

Expected: working tree clean. All changes from tasks 1–7 already committed.

- [ ] **Step 6: Review log**

Run: `git log --oneline origin/feature/o2ring-ble-sync..HEAD`

Expected: 6 commits in order:
1. `feat(ble): add IBleClient::wasDeviceFound() accessor`
2. `feat(o2ring): append CONNECT_FAILED to O2RingSyncResult`
3. `test(o2ring): pin deviceFoundFlag=false on empty-scan tests`
4. `feat(o2ring): return CONNECT_FAILED when scan hit but connect failed`
5. `feat(ble): set _lastScanFound on name-prefix match`
6. `feat(o2ring): surface CONNECT_FAILED on status card and FSM log`

Each commit is independently buildable (compile + native tests pass) so the branch is bisect-friendly.
