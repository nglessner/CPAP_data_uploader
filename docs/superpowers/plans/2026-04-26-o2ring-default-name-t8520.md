# O2Ring Default Device Name → `T8520` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Change the compiled-in default for `O2RING_DEVICE_NAME` from `"O2Ring"` to `"T8520"` so out-of-the-box scans match modern Viatom O2Ring-S firmware (advertised as `T8520_<mac4>`).

**Architecture:** One-line C++ default in `Config.cpp`, the corresponding test assertion in `test_config.cpp`, and three doc edits in `CONFIG_REFERENCE.md`. No behavioral change to scan/match logic.

**Tech Stack:** C++ on Arduino/ESP32 framework, PlatformIO, Unity unit tests in the `native` env.

**Spec:** `docs/superpowers/specs/2026-04-26-o2ring-default-name-t8520-design.md`

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `src/Config.cpp:47` | Modify | Default initializer for `o2ringDeviceName` |
| `test/test_config/test_config.cpp:924` | Modify | `test_o2ring_defaults` assertion |
| `docs/CONFIG_REFERENCE.md` | Modify | Default in section 9 table + example, add legacy-override paragraph |

No new files. No mock or orchestrator changes.

---

## Task 1: TDD the default flip

The existing test `test_o2ring_defaults` asserts the old default. Flip the assertion first (turning the test RED), then change the production default (turning it GREEN). This is the same RED-GREEN discipline used for behavior changes elsewhere in the project.

**Files:**
- Modify: `test/test_config/test_config.cpp:924`
- Modify: `src/Config.cpp:47`

- [ ] **Step 1: Flip the test assertion**

In `test/test_config/test_config.cpp`, locate `test_o2ring_defaults` (starts around line 921). Replace:

```cpp
void test_o2ring_defaults() {
    Config config;
    TEST_ASSERT_FALSE(config.isO2RingEnabled());
    TEST_ASSERT_EQUAL_STRING("O2Ring", config.getO2RingDeviceName().c_str());
    TEST_ASSERT_EQUAL_STRING("oximetry/raw", config.getO2RingPath().c_str());
    TEST_ASSERT_EQUAL_INT(30, config.getO2RingScanSeconds());
}
```

with:

```cpp
void test_o2ring_defaults() {
    Config config;
    TEST_ASSERT_FALSE(config.isO2RingEnabled());
    TEST_ASSERT_EQUAL_STRING("T8520", config.getO2RingDeviceName().c_str());
    TEST_ASSERT_EQUAL_STRING("oximetry/raw", config.getO2RingPath().c_str());
    TEST_ASSERT_EQUAL_INT(30, config.getO2RingScanSeconds());
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `source venv/bin/activate && pio test -e native -f test_config`

Expected: 1 failure in `test_o2ring_defaults` with a message like `Expected 'T8520' was 'O2Ring'`. All other tests in the suite pass.

If the failure mode is different (compile error, crash, different assertion firing), STOP and report — don't paper over it.

- [ ] **Step 3: Change the production default**

In `src/Config.cpp`, replace line 47:

```cpp
    o2ringDeviceName("O2Ring"),    // Default: match Wellue O2Ring advertised name prefix
```

with:

```cpp
    o2ringDeviceName("T8520"),    // Default: match Viatom O2Ring-S advertised name prefix
```

- [ ] **Step 4: Run the test and verify it passes**

Run: `pio test -e native -f test_config`

Expected: all tests pass, including `test_o2ring_defaults`.

- [ ] **Step 5: Verify pico32 build still passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: build succeeds. The change is a 6-byte string literal swap; no measurable size impact.

- [ ] **Step 6: Commit**

```bash
git add src/Config.cpp test/test_config/test_config.cpp
git commit -m "feat(o2ring): change default device name from O2Ring to T8520

Real Wellue O2Ring-S firmware advertises as T8520_<mac4> (Viatom
internal model code). The previous default \"O2Ring\" never matched a
live device, making the feature appear silently broken on first run.
Users with legacy firmware advertising as O2Ring* can override via
O2RING_DEVICE_NAME in config.txt. Closes #4."
```

---

## Task 2: Update `docs/CONFIG_REFERENCE.md`

Three edits within section 9 ("O2Ring BLE Sync (optional)"):
- Table-row default
- Example block default
- New paragraph documenting the legacy override path

**Files:**
- Modify: `docs/CONFIG_REFERENCE.md` (lines around 103, 111, and section 9 body)

- [ ] **Step 1: Update the table-row default**

In `docs/CONFIG_REFERENCE.md`, replace:

```
| `O2RING_DEVICE_NAME` | `O2Ring` | BLE advertised name prefix to scan for |
```

with:

```
| `O2RING_DEVICE_NAME` | `T8520` | BLE advertised name prefix to scan for |
```

- [ ] **Step 2: Update the example block default**

In the same section's example block, replace:

```
O2RING_DEVICE_NAME = O2Ring
```

with:

```
O2RING_DEVICE_NAME = T8520
```

- [ ] **Step 3: Add the legacy-override paragraph**

Locate the line that currently reads (immediately after the example block):

```
The sync runs after each CPAP upload cycle, once the SD card has been released back to the CPAP machine. `.vld` session files are downloaded from the ring over BLE and written directly to the SMB share — no SD card access is required.
```

Insert a new paragraph immediately BEFORE that "The sync runs..." line, separated by a blank line above and below:

```
The default `T8520` matches modern Viatom O2Ring-S firmware (advertised as `T8520_<last4-mac>`). Older Viatom rings sometimes advertise as `O2Ring` or another vendor-specific prefix; if the firmware never finds your ring with the default, override via `O2RING_DEVICE_NAME = <prefix>` after confirming the actual advertised name with a BLE scanner (e.g. `bluetoothctl scan on`, the nRF Connect mobile app, etc.).
```

- [ ] **Step 4: Spot-check the rendered section**

Run: `grep -n -A2 "O2RING_DEVICE_NAME\|T8520\|legacy" docs/CONFIG_REFERENCE.md | head -30`

Expected: every occurrence of the key now shows `T8520` as the value, and the new paragraph mentioning "legacy" / "default `T8520`" appears once.

- [ ] **Step 5: Commit**

```bash
git add docs/CONFIG_REFERENCE.md
git commit -m "docs(o2ring): document T8520 as default and legacy override path

Reflects the firmware default change in #4. Adds a paragraph explaining
that older Viatom rings advertising as O2Ring* can override via
O2RING_DEVICE_NAME after confirming the live advertised name with a
BLE scanner."
```

---

## Task 3: Final verification

No code changes. Confirm cumulative state.

- [ ] **Step 1: Full native suite**

Run: `source venv/bin/activate && pio test -e native`

Expected: all suites pass, same count as before (no test added or removed).

- [ ] **Step 2: pico32 build**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: succeeds.

- [ ] **Step 3: Confirm clean tree**

Run: `git status`

Expected: working tree clean except for pre-existing items (`.gitignore` modification and untracked plan docs from earlier in the session, unrelated to this work).

- [ ] **Step 4: Review branch's commit log**

Run: `git log --oneline main..HEAD`

Expected: 3 commits (counting the spec commit already on the branch):
1. `docs(o2ring): spec for changing default device name to T8520` (already committed)
2. `feat(o2ring): change default device name from O2Ring to T8520`
3. `docs(o2ring): document T8520 as default and legacy override path`
