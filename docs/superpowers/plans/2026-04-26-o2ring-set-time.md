# O2Ring SetTIME Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Send `{"SetTIME":"YYYY-MM-DD,HH:MM:SS"}` to the O2Ring via `CMD_CONFIG (0x16)` after every successful INFO exchange so the firmware sets the ring's wall-clock without the user touching the Wellue mobile app.

**Architecture:** A new protocol-layer helper formats the JSON payload from a `struct tm`. `O2RingSync::run()` inserts a best-effort `CMD_CONFIG` round-trip between the INFO response parse and the file-download loop. Any failure (write rejected, response timeout, malformed ack) logs a warning and falls through — the file pulls don't depend on the clock being correct.

**Tech Stack:** C++ on Arduino/ESP32 framework, NimBLE-Arduino, PlatformIO, Unity unit tests in the `native` env. Reference impl: `MackeyStingray/o2r/o2r/o2cmd.py`.

**Spec:** `docs/superpowers/specs/2026-04-26-o2ring-set-time-design.md`

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `include/O2RingProtocol.h` | Modify | Add `CMD_CONFIG = 0x16` and `formatSetTimePayload()` helper |
| `test/test_o2ring/test_o2ring_protocol.cpp` | Modify | Unit test for `formatSetTimePayload` |
| `test/mocks/MockBleClient.h` | Modify | Add `writeHistory` vector for multi-write assertions |
| `test/test_o2ring_sync/test_o2ring_sync.cpp` | Modify | Migrate one existing test off `lastWritten`; add 2 new orchestrator tests; enqueue SetTIME ack in 3 tests that flow past INFO |
| `src/O2RingSync.cpp` | Modify | Insert SetTIME block between INFO parse and file-download loop |

No new files, no new test directories.

---

## Task 1: Add `CMD_CONFIG` constant and `formatSetTimePayload` helper

Pure protocol-layer addition, fully unit-testable in `test_o2ring`. Use TDD: write the format test first, watch it fail (helper doesn't exist), implement, verify pass.

**Files:**
- Modify: `include/O2RingProtocol.h`
- Modify: `test/test_o2ring/test_o2ring_protocol.cpp`

- [ ] **Step 1: Write the failing test**

In `test/test_o2ring/test_o2ring_protocol.cpp`, immediately before the `int main()` block at line 134, add:

```cpp
void test_format_set_time_payload_basic() {
    struct tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon  = 4 - 1;
    tm.tm_mday = 26;
    tm.tm_hour = 19;
    tm.tm_min  = 30;
    tm.tm_sec  = 45;

    char buf[64];
    size_t n = O2RingProtocol::formatSetTimePayload(tm, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(33, (int)n);
    TEST_ASSERT_EQUAL_STRING("{\"SetTIME\":\"2026-04-26,19:30:45\"}", buf);
}

void test_format_set_time_payload_buffer_too_small() {
    struct tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon  = 0;
    tm.tm_mday = 1;

    char buf[16];  // 33 chars + NUL won't fit
    size_t n = O2RingProtocol::formatSetTimePayload(tm, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(0, (int)n);
}

void test_format_set_time_payload_zero_pads_single_digits() {
    struct tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon  = 1 - 1;
    tm.tm_mday = 3;
    tm.tm_hour = 4;
    tm.tm_min  = 5;
    tm.tm_sec  = 6;

    char buf[64];
    size_t n = O2RingProtocol::formatSetTimePayload(tm, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(33, (int)n);
    TEST_ASSERT_EQUAL_STRING("{\"SetTIME\":\"2026-01-03,04:05:06\"}", buf);
}
```

Register the new tests by replacing:

```cpp
    RUN_TEST(test_parse_file_open_too_short);
    return UNITY_END();
```

with:

```cpp
    RUN_TEST(test_parse_file_open_too_short);
    RUN_TEST(test_format_set_time_payload_basic);
    RUN_TEST(test_format_set_time_payload_buffer_too_small);
    RUN_TEST(test_format_set_time_payload_zero_pads_single_digits);
    return UNITY_END();
```

- [ ] **Step 2: Run the failing test**

Run: `pio test -e native -f test_o2ring`

Expected: build fails with "no member named 'formatSetTimePayload' in namespace 'O2RingProtocol'" (or similar). Compile error is the expected RED state — the helper doesn't exist yet.

- [ ] **Step 3: Implement the helper and constant**

In `include/O2RingProtocol.h`, immediately after the line:

```cpp
static const uint8_t CMD_FILE_CLOSE = 0x05;
```

add:

```cpp
static const uint8_t CMD_CONFIG     = 0x16;
```

Then immediately before the closing `} // namespace O2RingProtocol` line at the bottom of the file, add:

```cpp
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

- [ ] **Step 4: Run the test and verify it passes**

Run: `pio test -e native -f test_o2ring`

Expected: all tests pass, including the 3 new ones (`test_format_set_time_payload_basic`, `test_format_set_time_payload_buffer_too_small`, `test_format_set_time_payload_zero_pads_single_digits`).

- [ ] **Step 5: Verify pico32 build still passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. The new helper is `inline` and only adds the `snprintf` call; no measurable size impact.

- [ ] **Step 6: Commit**

```bash
git add include/O2RingProtocol.h test/test_o2ring/test_o2ring_protocol.cpp
git commit -m "feat(o2ring): add CMD_CONFIG opcode and formatSetTimePayload helper

CMD_CONFIG (0x16) is the Wellue config-write opcode. SetTIME is one of
the writes that go through it as JSON. Helper takes struct tm so unit
tests are deterministic without depending on the test process's TZ env."
```

---

## Task 2: Add `writeHistory` to `MockBleClient`

Mock infrastructure addition. Tests need to assert that the SetTIME packet was sent without losing it when subsequent commands clobber `lastWritten`. Existing tests continue to use `lastWritten` and pass unchanged.

**Files:**
- Modify: `test/mocks/MockBleClient.h`

- [ ] **Step 1: Add the writeHistory member**

In `test/mocks/MockBleClient.h`, immediately after the line `std::vector<uint8_t> lastWritten;` (the existing assertion-helper field), add:

```cpp
    // Full chronological history of writeChunked() calls. Each entry is the
    // bytes of one packet. Lets tests assert on packets that lastWritten
    // would otherwise have been overwritten by subsequent commands.
    std::vector<std::vector<uint8_t>> writeHistory;
```

- [ ] **Step 2: Populate writeHistory in writeChunked**

Replace the existing `writeChunked` body:

```cpp
    bool writeChunked(const uint8_t* data, size_t len) override {
        lastWritten.assign(data, data + len);
        return true;
    }
```

with:

```cpp
    bool writeChunked(const uint8_t* data, size_t len) override {
        lastWritten.assign(data, data + len);
        writeHistory.emplace_back(data, data + len);
        return true;
    }
```

- [ ] **Step 3: Verify all native tests still pass**

Run: `pio test -e native`

Expected: all suites pass with no behavior changes. `writeHistory` is populated but no test reads it yet.

- [ ] **Step 4: Verify pico32 build passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. The mock is excluded from production builds via `#ifdef UNIT_TEST`, so this is a sanity check.

- [ ] **Step 5: Commit**

```bash
git add test/mocks/MockBleClient.h
git commit -m "test(mocks): add writeHistory to MockBleClient

Vector of all writeChunked() calls in chronological order, alongside the
existing lastWritten field. Lets tests assert on intermediate packets
that subsequent commands would otherwise have overwritten in lastWritten."
```

---

## Task 3: Migrate `test_info_command_sent_on_connect` from `lastWritten` to `writeHistory[0]`

Once Task 4 introduces SetTIME, the orchestrator's last write per run will be CMD_CONFIG, not CMD_INFO. The existing assertion `lastWritten[1] == CMD_INFO` would fail. Switch this single test to assert against `writeHistory[0]` (the first packet sent) so it remains correct regardless of later writes.

This task is a no-op against current behavior — `writeHistory[0]` and `lastWritten` are the same packet today (only one write happens before the test asserts). Tests stay green.

**Files:**
- Modify: `test/test_o2ring_sync/test_o2ring_sync.cpp:87-100`

- [ ] **Step 1: Update the assertion**

In `test/test_o2ring_sync/test_o2ring_sync.cpp`, replace:

```cpp
void test_info_command_sent_on_connect() {
    // INFO response
    const char* json = R"({"CurBAT":"75%","FileList":"","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));

    O2RingSync sync(cfg, mockBle);
    sync.run();

    // First byte of lastWritten should be 0xAA and CMD should be CMD_INFO
    TEST_ASSERT_GREATER_THAN(0, (int)mockBle->lastWritten.size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, mockBle->lastWritten[0]);
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_INFO, mockBle->lastWritten[1]);
}
```

with:

```cpp
void test_info_command_sent_on_connect() {
    // INFO response
    const char* json = R"({"CurBAT":"75%","FileList":"","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));

    O2RingSync sync(cfg, mockBle);
    sync.run();

    // First written packet must be CMD_INFO. Assert against writeHistory[0]
    // so subsequent writes (e.g., SetTIME after INFO) don't clobber the
    // assertion target.
    TEST_ASSERT_GREATER_THAN(0, (int)mockBle->writeHistory.size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, mockBle->writeHistory[0][0]);
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_INFO, mockBle->writeHistory[0][1]);
}
```

- [ ] **Step 2: Run tests and verify pass**

Run: `pio test -e native -f test_o2ring_sync`

Expected: all 9 tests pass. `writeHistory[0]` and `lastWritten` carry identical bytes at this point (no SetTIME yet), so the migrated assertion checks the same thing.

- [ ] **Step 3: Commit**

```bash
git add test/test_o2ring_sync/test_o2ring_sync.cpp
git commit -m "test(o2ring): pin info-command assertion to writeHistory[0]

Once SetTIME is added between INFO and the rest of the sync, lastWritten
will hold the CMD_CONFIG packet at end-of-run, not CMD_INFO. Switching
this assertion to writeHistory[0] keeps it pointing at the first written
packet and decouples it from later writes."
```

---

## Task 4: TDD — write failing tests for the SetTIME orchestrator path

Two new tests assert (a) the SetTIME packet is sent after a successful INFO and (b) failure to receive an ack does not abort the sync. Both fail today because `O2RingSync::run()` doesn't call SetTIME at all — `writeHistory` only contains the INFO packet.

**Files:**
- Modify: `test/test_o2ring_sync/test_o2ring_sync.cpp`

- [ ] **Step 1: Add the two new tests**

In `test/test_o2ring_sync/test_o2ring_sync.cpp`, immediately before the `int main()` block (after the last `RUN_TEST(...)` block from prior work), add:

```cpp
// Build an empty CMD_CONFIG ack response packet that the mock can serve
// when O2RingSync sends SetTIME. makeResponse uses buildPacket which writes
// 0xAA as the start byte, but the orchestrator does not validate the start
// byte — it only reads cmd at index 1, dataLen at indexes 5/6, and data at
// index 7+. Empty data → 8-byte packet with cmd=0x16 at index 1.
static std::vector<uint8_t> makeSetTimeAck() {
    return makeResponse(O2RingProtocol::CMD_CONFIG, 0, nullptr, 0);
}

void test_set_time_sent_after_info_success() {
    // INFO returns one file already in the dedup state, so we end on
    // NOTHING_TO_SYNC after SetTIME completes.
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    mockBle->enqueueResponse(makeSetTimeAck());

    {
        O2RingState state;
        state.load();
        state.markSeen("20260116233312.vld");
        state.save();
    }

    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC, (int)result);

    // Two packets must have been written: INFO first, SetTIME second.
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, (int)mockBle->writeHistory.size());
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_INFO,
                            mockBle->writeHistory[0][1]);
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_CONFIG,
                            mockBle->writeHistory[1][1]);

    // Payload (bytes 7..end-1, after the 7-byte header and before CRC)
    // must contain the "SetTIME" key.
    auto& pkt = mockBle->writeHistory[1];
    TEST_ASSERT_GREATER_THAN(8, (int)pkt.size());
    std::string payload((const char*)(pkt.data() + 7), pkt.size() - 8);
    TEST_ASSERT_TRUE(payload.find("\"SetTIME\":") != std::string::npos);
}

void test_set_time_failure_does_not_abort() {
    // INFO returns one file already in dedup state. NO SetTIME ack is
    // enqueued, so the orchestrator's wait will time out at the mock
    // (returns false from readResponse with empty queue). Sync must still
    // complete with NOTHING_TO_SYNC, not BLE_ERROR.
    const char* json = R"({"CurBAT":"75%","FileList":"20260116233312.vld","Model":"O2Ring","SN":"1234"})";
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
    // Intentionally no makeSetTimeAck() enqueued.

    {
        O2RingState state;
        state.load();
        state.markSeen("20260116233312.vld");
        state.save();
    }

    O2RingSync sync(cfg, mockBle);
    O2RingSyncResult result = sync.run();
    TEST_ASSERT_EQUAL_INT((int)O2RingSyncResult::NOTHING_TO_SYNC, (int)result);

    // SetTIME write must still have happened — only the response was missing.
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, (int)mockBle->writeHistory.size());
    TEST_ASSERT_EQUAL_UINT8(O2RingProtocol::CMD_CONFIG,
                            mockBle->writeHistory[1][1]);
}
```

Register them in `main()` by replacing:

```cpp
    RUN_TEST(test_status_recorded_on_connect_failed);
    return UNITY_END();
```

with:

```cpp
    RUN_TEST(test_status_recorded_on_connect_failed);
    RUN_TEST(test_set_time_sent_after_info_success);
    RUN_TEST(test_set_time_failure_does_not_abort);
    return UNITY_END();
```

Confirm `<string>` is already included — `test/test_o2ring_sync/test_o2ring_sync.cpp` already pulls in STL headers via `MockBleClient.h`. If the build complains about `std::string`, add `#include <string>` near the top of the test file.

- [ ] **Step 2: Run tests and verify they fail in the right way**

Run: `pio test -e native -f test_o2ring_sync`

Expected: exactly 2 failures with the message pattern `Expected 2 was 1` or similar — one for each new test, both because `writeHistory.size() == 1` (only INFO was written; SetTIME is not yet implemented). All 9 prior tests pass.

If the failure mode is different — e.g., a compile error, a crash, or a different assertion firing first — STOP and report BLOCKED. Do not "fix" the test to pass; that defeats the purpose of writing failing tests first.

- [ ] **Step 3: Do NOT commit**

The tree is intentionally RED. Task 5 takes it back to GREEN by implementing SetTIME and updating the existing tests that flow past INFO.

---

## Task 5: Implement SetTIME in `O2RingSync::run()` and update existing tests

This commit takes the tree from RED to GREEN. It does three things in one commit:

1. Adds the SetTIME block to `src/O2RingSync.cpp::run()` between INFO parse and `state.load()`.
2. Updates 3 existing tests that flow past INFO to enqueue a SetTIME ack response between their INFO response and any subsequent file responses (so the FIFO mock queue lines up with the new write order).
3. The 2 new tests from Task 4 transition from FAIL to PASS automatically once the impl lands.

**Files:**
- Modify: `src/O2RingSync.cpp`
- Modify: `test/test_o2ring_sync/test_o2ring_sync.cpp` (existing tests only — Task 4's new tests stay as written)

- [ ] **Step 1: Add the SetTIME block to `O2RingSync::run()`**

In `src/O2RingSync.cpp`, locate the block that starts at line 162 (the existing `LOGF("[O2Ring] Device reports %u file(s)", (unsigned)fileList.size());` line) and ends just before the `state.load();` line at line 172. Between those two lines (after the existing `latestOnDevice` computation block at lines 163-170 and before `state.load();`), insert:

```cpp
    // Set the ring's wall-clock from the ESP32's localtime. Best-effort:
    // any failure here logs a warning but does not abort the sync — file
    // pulls don't depend on the clock being correct, and the ring will
    // accept the next sync's SetTIME write just as well.
    {
        time_t now = time(nullptr);
        struct tm tmnow;
        char payload[64];
        size_t payloadLen = 0;
        if (localtime_r(&now, &tmnow) != nullptr) {
            payloadLen = O2RingProtocol::formatSetTimePayload(
                tmnow, payload, sizeof(payload));
        }
        if (payloadLen == 0) {
            LOG_WARN("[O2Ring] SetTIME payload format failed");
        } else if (!sendCommand(O2RingProtocol::CMD_CONFIG, 0,
                                (const uint8_t*)payload,
                                (uint16_t)payloadLen)) {
            LOG_WARN("[O2Ring] SetTIME write failed");
        } else {
            uint8_t respBuf[64];
            size_t respLen = 0;
            if (!receiveResponse(respBuf, sizeof(respBuf), respLen, 2000)) {
                LOG_WARN("[O2Ring] SetTIME response timeout");
            } else if (respLen >= 2
                       && respBuf[1] != O2RingProtocol::CMD_CONFIG) {
                LOG_WARN("[O2Ring] SetTIME response cmd mismatch");
            } else {
                LOG("[O2Ring] SetTIME ok");
            }
        }
    }

```

The block is enclosed in its own `{ ... }` scope so the local variables (`now`, `tmnow`, `payload`, etc.) don't leak into the surrounding function. No early return or break — control always falls through to the existing `state.load();` line.

- [ ] **Step 2: Update `test_nothing_to_sync_when_all_seen` to enqueue SetTIME ack**

In `test/test_o2ring_sync/test_o2ring_sync.cpp`, locate `test_nothing_to_sync_when_all_seen` (starts around line 70). Find the line:

```cpp
    mockBle->enqueueResponse(makeResponse(O2RingProtocol::CMD_INFO, 0,
        (const uint8_t*)json, (uint16_t)strlen(json)));
```

Immediately after that line (and before `// Pre-mark the file as seen`), add:

```cpp
    mockBle->enqueueResponse(makeSetTimeAck());
```

- [ ] **Step 3: Update `test_stale_seen_entries_pruned_after_info` similarly**

In the same file, locate `test_stale_seen_entries_pruned_after_info` (starts around line 102). Find its INFO enqueue line and immediately after it add:

```cpp
    mockBle->enqueueResponse(makeSetTimeAck());
```

- [ ] **Step 4: Update `test_status_records_filename_on_nothing_to_sync` similarly**

Locate `test_status_records_filename_on_nothing_to_sync` (starts around line 144). Find its INFO enqueue line and immediately after it add:

```cpp
    mockBle->enqueueResponse(makeSetTimeAck());
```

- [ ] **Step 5: Run all native tests**

Run: `pio test -e native -f test_o2ring_sync`

Expected: 11/11 tests pass.
- 7 original tests (post-Task 1-3): pass
- 2 RED tests from Task 4: now GREEN
- 2 newer tests added in the prior PR (`test_connect_failed_*`, `test_status_recorded_on_connect_failed`): still pass

Run: `pio test -e native`

Expected: full suite passes. Other suites are unaffected — only `test_o2ring_sync` and `test_o2ring` (from Task 1) changed.

- [ ] **Step 6: Verify pico32 build passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. New code adds ~1 KB flash (snprintf format string + the SetTIME block in run()); RAM impact is the 64-byte stack `payload` buffer plus the 64-byte stack `respBuf`.

- [ ] **Step 7: Commit**

```bash
git add src/O2RingSync.cpp test/test_o2ring_sync/test_o2ring_sync.cpp
git commit -m "feat(o2ring): set ring wall-clock via CMD_CONFIG SetTIME

After every successful INFO and before the file-download loop, send the
ring a {\"SetTIME\":\"YYYY-MM-DD,HH:MM:SS\"} write via CMD_CONFIG (0x16)
with the ESP32's localtime. Best-effort: format/write/response failures
log a warning and fall through. Closes #6."
```

---

## Task 6: Final verification

No code changes. Confirm the cumulative state.

- [ ] **Step 1: Touched-module native tests**

Run: `pio test -e native -f test_o2ring_sync`

Expected: 11/11 pass.

Run: `pio test -e native -f test_o2ring`

Expected: pass (15 tests — 12 original + 3 new from Task 1).

- [ ] **Step 2: Full native suite**

Run: `pio test -e native`

Expected: all suites pass. Compare against the pre-task baseline of 171; new total is 171 + 3 (Task 1 helper tests) + 2 (Task 4 orchestrator tests) = 176.

- [ ] **Step 3: pico32 build**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: succeeds. Note flash/RAM lines for any unexpected jumps; expect a small (<1%) flash increase.

- [ ] **Step 4: Confirm clean tree**

Run: `git status`

Expected: working tree clean except for pre-existing items (the `.gitignore` modification and untracked plan docs).

- [ ] **Step 5: Review branch's commit log**

Run: `git log --oneline main..HEAD`

Expected: 5 commits in this order, plus the pre-existing spec commit:

1. `docs(o2ring): spec for firmware-side SetTIME via CMD_CONFIG` (already on branch)
2. `feat(o2ring): add CMD_CONFIG opcode and formatSetTimePayload helper`
3. `test(mocks): add writeHistory to MockBleClient`
4. `test(o2ring): pin info-command assertion to writeHistory[0]`
5. `feat(o2ring): set ring wall-clock via CMD_CONFIG SetTIME`

Each commit is independently buildable (compile + native tests pass) so the branch is bisect-friendly.
