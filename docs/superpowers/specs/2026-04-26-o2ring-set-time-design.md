# O2Ring sync: set ring wall-clock from firmware

**Date:** 2026-04-26
**Closes:** [nglessner/CPAP_data_uploader#6](https://github.com/nglessner/CPAP_data_uploader/issues/6)

## Problem

The Wellue O2Ring-S only stamps `.vld` files with correct wall-clock time after the user has paired the ring with the Wellue mobile app at least once — the app sets the clock during onboarding. Pairing with the vendor app is also the suspected cause of GATT connection rejection by the ring after-the-fact (see #3 / `admin/sleep` bonding-unblock plan): the ring may bond exclusively to the first central it sees.

Without firmware-side clock setting, the unblock path "never touch the Wellue app" is procedurally impossible: a fresh ring would record sessions with an uninitialised or default clock, and the timestamps in the resulting `.vld` headers would be useless for cross-referencing CPAP sessions.

## Goal

After every successful `CMD_INFO` exchange and before the file-download loop, the firmware sends the ring's `SetTIME` config write with the ESP32's current local time. The write is fire-and-forget from the orchestrator's point of view — any failure (BLE write rejected, response timeout, malformed ack) is logged but does not abort the sync.

Out of scope:
- Drift check / parse of `CurTIME` from the INFO JSON. Always send.
- A config knob to opt out. `O2RING_ENABLED=false` already disables the entire sync.
- NVS state tracking "did we ever successfully set the clock." Each sync re-sends.
- Setting non-time configuration via `CMD_CONFIG` (alarm thresholds, vibration strength, screen brightness). Separate future work if needed.

## Protocol details

The reference impl `MackeyStingray/o2r/o2r/o2cmd.py` shows the wire format:

```python
def SetTime():
    cmd = '{"SetTIME":"%s"}' % time.strftime('%Y-%m-%d,%H:%M:%S')
    return o2pkt(CMD_CONFIG, data=cmd)
```

Key facts:

- **Opcode:** `CMD_CONFIG = 0x16`. There is no dedicated `SET_TIME` opcode despite issue #6's framing — clock setting is one of many writes that go through `CMD_CONFIG` with a JSON payload.
- **Payload:** ASCII JSON `{"SetTIME":"YYYY-MM-DD,HH:MM:SS"}` — note the **comma** between date and time, not a space. 33 characters.
- **Time zone:** the ring expects local time. The reference uses `time.strftime`; our firmware uses `localtime_r` (already configured by `ScheduleManager.cpp:66 configTime(gmtOffsetSeconds, 0, ntpServer)`).
- **Response:** the reference impl handles `pkt.cmd == CMD_CONFIG` with no payload validation. We treat any well-formed response as success and any failure as warn-and-continue.

## Design

### New protocol surface

`include/O2RingProtocol.h`:

```cpp
static const uint8_t CMD_CONFIG = 0x16;

// Format {"SetTIME":"YYYY-MM-DD,HH:MM:SS"} into out for CMD_CONFIG payload.
// Caller is responsible for converting time_t → struct tm with the correct
// timezone (firmware uses localtime_r; see ScheduleManager configTime).
// Returns bytes written (33 on success), 0 if outCap is too small or on
// snprintf failure.
inline size_t formatSetTimePayload(const struct tm& tm,
                                    char* out, size_t outCap) {
    int n = snprintf(out, outCap,
        "{\"SetTIME\":\"%04d-%02d-%02d,%02d:%02d:%02d\"}",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return (n < 0 || (size_t)n >= outCap) ? 0 : (size_t)n;
}
```

Taking `struct tm` (not `time_t`) makes the helper deterministic in unit tests
without depending on the test process's `TZ` environment variable.

### Orchestrator change

`src/O2RingSync.cpp::run()` — insert a `SetTIME` block between the existing
`LOGF("[O2Ring] Device reports %u file(s)", ...)` line and the existing
`state.load()` line. Pseudocode shape (exact code in the implementation plan):

```cpp
// Set the ring's wall-clock from the ESP32's localtime. Best-effort:
// failures here log a warning but do not abort the sync — file pulls
// don't depend on the clock being correct.
{
    time_t now = time(nullptr);
    struct tm tm;
    char payload[64];
    size_t payloadLen = 0;
    if (localtime_r(&now, &tm) != nullptr) {
        payloadLen = O2RingProtocol::formatSetTimePayload(tm, payload, sizeof(payload));
    }
    if (payloadLen == 0) {
        LOG_WARN("[O2Ring] SetTIME payload format failed");
    } else if (!sendCommand(O2RingProtocol::CMD_CONFIG, 0,
                            (const uint8_t*)payload, (uint16_t)payloadLen)) {
        LOG_WARN("[O2Ring] SetTIME write failed");
    } else {
        uint8_t respBuf[64];
        size_t respLen = 0;
        if (!receiveResponse(respBuf, sizeof(respBuf), respLen, 2000)) {
            LOG_WARN("[O2Ring] SetTIME response timeout");
        } else if (respLen >= 2 && respBuf[1] != O2RingProtocol::CMD_CONFIG) {
            LOG_WARN("[O2Ring] SetTIME response cmd mismatch");
        } else {
            LOG("[O2Ring] SetTIME ok");
        }
    }
}
```

The block does not return early on any path — control always falls through to
`state.load()`.

### Mock test infrastructure

`test/mocks/MockBleClient.h` — add a write-history vector so tests can assert
the SetTIME packet was sent without losing it when subsequent commands clobber
`lastWritten`:

```cpp
std::vector<std::vector<uint8_t>> writeHistory;
```

Populated alongside the existing `lastWritten` assignment in `writeChunked()`.
Existing tests that read `lastWritten` continue to work unchanged.

### Tests

`test/test_o2ring_sync/test_o2ring_sync.cpp` — three new tests:

1. **`test_set_time_payload_format`** — call `formatSetTimePayload` with a
   fixed `struct tm` (e.g., 2026-04-26 19:30:45) and assert the exact string
   `{"SetTIME":"2026-04-26,19:30:45"}` and length 33. Pure unit test, no
   orchestrator involvement.

2. **`test_set_time_sent_after_info_success`** — INFO returns a `FileList`
   with one filename already in the dedup state (so we end on
   `NOTHING_TO_SYNC`). Enqueue an INFO response, then enqueue a
   well-formed `CMD_CONFIG` ack response. Assert:
   - `sync.run()` returns `NOTHING_TO_SYNC`
   - `mockBle->writeHistory.size() >= 2`
   - `writeHistory[0][1] == 0x14` (CMD_INFO)
   - `writeHistory[1][1] == 0x16` (CMD_CONFIG)
   - `writeHistory[1]` payload bytes (after the 7-byte header) contain
     `"SetTIME"` substring.

3. **`test_set_time_failure_does_not_abort`** — INFO returns a `FileList` with
   one filename already in dedup state; do NOT enqueue a CMD_CONFIG ack. The
   `MockBleClient::readResponse` will return false on the SetTIME wait.
   Assert:
   - `sync.run()` returns `NOTHING_TO_SYNC` (not `BLE_ERROR`)
   - `O2RingStatus::getLastResult()` is `NOTHING_TO_SYNC`
   - `writeHistory[1][1] == 0x16` confirms the SetTIME packet was still sent

### Existing test compatibility

Tests that go through INFO and then expect FILE_OPEN responses
(`test_nothing_to_sync_when_all_seen`, `test_info_command_sent_on_connect`,
`test_stale_seen_entries_pruned_after_info`,
`test_status_records_filename_on_nothing_to_sync`) will now hit the SetTIME
path between INFO and FILE_OPEN. Each of those tests must enqueue a
CMD_CONFIG ack response after the INFO response and before any file response,
or rely on the SetTIME-failure-tolerated path (no extra response, but the
LOG_WARN is harmless).

To keep existing tests focused on what they were originally testing, add a
helper `enqueueSetTimeAck()` that pushes a minimal valid CMD_CONFIG response
onto the mock's queue, and call it once per existing test that runs through
INFO success.

## Persistence and compatibility

- No NVS schema changes.
- No new config keys.
- Build flags unchanged. Surface only present under `-DENABLE_O2RING_SYNC`.
- Wire format matches the Wellue mobile app's behavior (per reference impl).
  Existing rings that have been paired with the mobile app continue to work;
  rings that have never seen the app will now have correct timestamps from
  first sync onward.

## Acceptance

- `O2RingProtocol::CMD_CONFIG = 0x16` exists.
- `O2RingProtocol::formatSetTimePayload(struct tm, char*, size_t)` exists,
  produces the documented format, and has a passing unit test.
- `O2RingSync::run()` sends a `CMD_CONFIG` packet with `SetTIME` JSON between
  INFO success and the file-download loop on every run that gets past INFO.
- Failure of any step in the SetTIME block (format, write, response) is
  logged at WARN level and does not abort the sync.
- `MockBleClient::writeHistory` is populated and existing tests still pass.
- New tests in `test/test_o2ring_sync/` cover the format, the
  packet-was-sent-on-success path, and the failure-tolerant path.
- `pio test -e native` passes (full suite).
- `pio run -e pico32` builds without size regression beyond noise.
