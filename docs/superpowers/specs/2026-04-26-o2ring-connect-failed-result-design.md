# O2Ring sync: distinguish CONNECT_FAILED from DEVICE_NOT_FOUND

**Date:** 2026-04-26
**Closes:** [nglessner/CPAP_data_uploader#5](https://github.com/nglessner/CPAP_data_uploader/issues/5)

## Problem

`O2RingSync::run()` reports any `IBleClient::connect()` failure as
`O2RingSyncResult::DEVICE_NOT_FOUND` (`src/O2RingSync.cpp:129-135`). That
bucketing conflates two failure modes with different remediation:

- **Scan miss** — name prefix wrong, ring asleep, ring out of range, BT
  controller issue. Fix path: check ring state, name config, range.
- **Post-scan rejection** — ring was found in advertising results but the
  GATT connection / service discovery / notify subscribe failed. Fix path:
  bonding state, conn-param negotiation, security request, characteristic
  UUIDs.

In the 2026-04-25 debugging session this lumping cost hours: the status
card and serial logs both said `DEVICE_NOT_FOUND` while the ESP32 was
actually finding the ring on every cycle and being rejected at GATT level.

## Goal

Add a new result variant `O2RingSyncResult::CONNECT_FAILED` and route the
two failure modes to distinct enum values, persisted status strings, and
log lines. `DEVICE_NOT_FOUND` is reserved strictly for the empty-scan case.

Out of scope: splitting post-connect failures (`BLE_ERROR` from subscribe
/ command-send / response-timeout) — separate future ticket.

## Design

### Interface change

`IBleClient::connect()` today does scan + GATT-connect + service discovery
+ notify-subscribe in a single call returning one bool. To disambiguate
without redesigning the interface, add a single post-hoc accessor:

```cpp
// include/IBleClient.h
virtual bool wasDeviceFound() const = 0;
```

**Semantics:** returns true iff the most recent `connect()` call observed
a name-prefix match in scan results — regardless of whether the
subsequent GATT steps succeeded. Reset to false at the top of each
`connect()` call. Callers query it only after `connect()` returned false.

Considered alternative: split the interface into `findDevice()` +
`openConnection()`. Cleaner separation, but every caller, the Esp32
implementation, the mock, and protocol semantics change. The accessor is
the smaller diff for the same observable behavior.

### O2RingSync behavior

Replace the single failure path at `src/O2RingSync.cpp:129-135`:

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

All other return paths in `run()` are unchanged.

### Esp32BleClient implementation

Add a private member that tracks the scan outcome:

```cpp
// include/Esp32BleClient.h
private:
    bool _lastScanFound = false;

public:
    bool wasDeviceFound() const override { return _lastScanFound; }
```

In `Esp32BleClient::connect()` (`src/Esp32BleClient.cpp:45`):

- Set `_lastScanFound = false` as the first line.
- Inside the scan-results loop, set `_lastScanFound = true` at the same
  point we currently set `found = true` (after the name-prefix match,
  before `client->connect(targetAddr)`).
- All subsequent failure paths (service not found, characteristic not
  found, notify register failed, GATT connect rejected) leave the flag
  set to true — they are "found but couldn't talk to it" cases.

### MockBleClient

Add a public flag that tests can manipulate:

```cpp
// test/mocks/MockBleClient.h
bool deviceFoundFlag = true;       // default preserves existing test semantics
bool wasDeviceFound() const override { return deviceFoundFlag; }
```

Default `true` matches the implicit pre-change behavior: a mock with
`shouldConnect = true` represents a found-and-connected device, so
`wasDeviceFound()` should reflect that. Existing tests that set
`shouldConnect = false` to simulate "not found" should be updated to
also clear `deviceFoundFlag` if the test's intent is genuine
DEVICE_NOT_FOUND.

### Result enum

```cpp
// include/O2RingSync.h
enum class O2RingSyncResult {
    OK,
    DEVICE_NOT_FOUND,
    CONNECT_FAILED,    // <-- new
    SMB_ERROR,
    BLE_ERROR,
    NOTHING_TO_SYNC
};
```

The numeric ordering changes if `CONNECT_FAILED` is inserted before
`SMB_ERROR`. Since `O2RingStatus` persists the int value of the enum,
appending the new variant at the **end** (after `NOTHING_TO_SYNC`) avoids
renumbering existing persisted records:

```cpp
enum class O2RingSyncResult {
    OK,                // 0
    DEVICE_NOT_FOUND,  // 1
    SMB_ERROR,         // 2
    BLE_ERROR,         // 3
    NOTHING_TO_SYNC,   // 4
    CONNECT_FAILED     // 5  <-- new, appended
};
```

### Web handler and dashboard

`src/CpapWebServer.cpp:1351 o2ringResultName()` gets a new case:

```cpp
case O2RingSyncResult::CONNECT_FAILED:    return "CONNECT_FAILED";
```

Dashboard JS (`include/web_ui.h:311`) renders `o.last_result` verbatim, so
the new string `"CONNECT_FAILED"` appears with no client-side mapping
change.

### main.cpp logging

`src/main.cpp handleO2RingSync()` switch (lines 838-850) gets a new arm:

```cpp
case O2RingSyncResult::CONNECT_FAILED:
    LOG_WARN("[O2Ring] Sync failed: device found but connect rejected");
    break;
```

## Tests

Add to `test/test_o2ring_sync/` — two new cases:

1. **DEVICE_NOT_FOUND path**: `MockBleClient` with `shouldConnect = false,
   deviceFoundFlag = false`. Assert `O2RingSync::run()` returns
   `O2RingSyncResult::DEVICE_NOT_FOUND` and `O2RingStatus::getLastResult()`
   persists the matching int.
2. **CONNECT_FAILED path**: `MockBleClient` with `shouldConnect = false,
   deviceFoundFlag = true`. Assert returns `CONNECT_FAILED` and
   persistence matches.

Existing tests that drive the not-found path should be audited to set
`deviceFoundFlag = false` if they intend DEVICE_NOT_FOUND semantics —
otherwise they will flip to `CONNECT_FAILED` after this change.

## Persistence and compatibility

- `O2RingStatus` int-based serialization handles the new variant with no
  schema change. Records written before this change keep their original
  meaning (genuine empty-scan).
- Build flags unchanged. Surface only present under
  `-DENABLE_O2RING_SYNC`.
- No NVS namespace or key changes.

## Acceptance

- `O2RingSyncResult::CONNECT_FAILED` exists and is appended to the enum.
- `IBleClient::wasDeviceFound()` exists and is implemented in
  `Esp32BleClient` and `MockBleClient`.
- `O2RingSync::run()` returns `CONNECT_FAILED` when scan succeeded but
  `connect()` returned false; returns `DEVICE_NOT_FOUND` only when the
  scan saw nothing.
- `/api/o2ring-status` emits `"CONNECT_FAILED"` for the new code; the
  dashboard `d-o2r-res` element renders that string.
- `main.cpp handleO2RingSync()` has a switch arm for the new variant.
- `pio test -e native -f test_o2ring_sync` passes, with two new test
  cases covering both failure paths.
