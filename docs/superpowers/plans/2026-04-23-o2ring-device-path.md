# O2Ring Per-Device SMB Subdir Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upload O2Ring `.vld` files into a per-device subdirectory (`oximetry/raw/<device>/<file>.vld`) so the server-side staging pipeline accepts them.

**Architecture:** Add a `DEVICE_NAME` key to `config.txt`. `Config` resolves a `deviceSegment` string once during `loadFromSD` using a sanitized `DEVICE_NAME`, falling back to the uploader's WiFi MAC (colons stripped, lowercased). The sanitizer and resolver are pure static helpers so they can be unit-tested without mocking WiFi. `O2RingSync` composes the final SMB path using both `getO2RingPath()` and `getDeviceSegment()`; `SMBUploader::createDirectory` is already recursive so no helper is needed.

**Tech Stack:** C++11, Arduino (ESP32), PlatformIO, Unity (unit tests), libsmb2.

**Design spec:** `docs/superpowers/specs/2026-04-23-o2ring-device-path-design.md`

---

## File Structure

**Create:**
- `test/mocks/MockWiFi.h` — minimal WiFi class exposing `macAddress()` with a settable return value, guarded by `UNIT_TEST`.
- `test/test_device_name/test_main.cpp` — unit suite for `sanitizeDeviceSegment` and `resolveDeviceSegment`.

**Modify:**
- `include/Config.h` — add `deviceName`, `deviceSegment` members; `getDeviceName()`, `getDeviceSegment()` getters; static `sanitizeDeviceSegment`, `resolveDeviceSegment` helpers.
- `src/Config.cpp` — parse `DEVICE_NAME`, implement helpers, compute `deviceSegment` at end of `loadFromSD`, WiFi include guarded by `UNIT_TEST`.
- `src/O2RingSync.cpp` — compose `/o2ringPath/deviceSegment/filename` at the upload call site (lines 108-110).
- `test/test_o2ring_sync/test_o2ring_sync.cpp` — include `MockWiFi.h` so `Config::loadFromSD` works under the new WiFi dependency.
- `test/test_config/*.cpp` — same MockWiFi include for the existing Config suite.
- `test/test_credential_migration/*.cpp` — same MockWiFi include (also calls `loadFromSD`).
- `docs/CONFIG_REFERENCE.md` — add `DEVICE_NAME` entry.
- `docs/DEVELOPMENT.md` — note per-device subdir for O2Ring path.
- `CLAUDE.md` (at `CPAP_data_uploader/CLAUDE.md`) — add invariant to BLE/O2Ring paragraph.

---

## Task 1: Create MockWiFi and verify existing tests still build

**Why first:** `Config::loadFromSD` will soon call `WiFi.macAddress()`. Any test that touches `loadFromSD` needs the mock. Introducing the mock now (before changing Config) keeps every intermediate commit green.

**Files:**
- Create: `test/mocks/MockWiFi.h`
- Modify: `test/test_config/test_main.cpp` (header include)
- Modify: `test/test_credential_migration/test_credential_migration.cpp` (header include — verify actual filename first with `ls test/test_credential_migration/`)
- Modify: `test/test_o2ring_sync/test_o2ring_sync.cpp` (header include)

### Steps

- [ ] **Step 1: Create `test/mocks/MockWiFi.h`**

```cpp
#ifndef MOCK_WIFI_H
#define MOCK_WIFI_H

#include <Arduino.h>

class MockWiFi {
public:
    String _macAddress = "AC:0B:FB:6F:A1:94";  // deterministic default for tests
    String macAddress() { return _macAddress; }
    void setMockMacAddress(const String& mac) { _macAddress = mac; }
};

extern MockWiFi WiFi;

#endif // MOCK_WIFI_H
```

- [ ] **Step 2: Add the singleton definition to `test/mocks/Arduino.cpp`**

Open `test/mocks/Arduino.cpp`. At the bottom of the file, add:

```cpp
#include "MockWiFi.h"
MockWiFi WiFi;
```

- [ ] **Step 3: Add the include in each test file that loads Config**

Add a single line alongside the existing mock includes in:
- `test/test_config/test_main.cpp`
- `test/test_credential_migration/<testfile>.cpp`
- `test/test_o2ring_sync/test_o2ring_sync.cpp`

```cpp
#include "../mocks/MockWiFi.h"
```

Place it after the other `../mocks/*.h` includes.

- [ ] **Step 4: Run existing native suites to confirm nothing broke**

Run: `source venv/bin/activate && pio test -e native`
Expected: all currently-passing tests still pass. New mock is unused but compiles cleanly.

- [ ] **Step 5: Commit**

```bash
git add test/mocks/MockWiFi.h test/mocks/Arduino.cpp \
        test/test_config test/test_credential_migration test/test_o2ring_sync
git commit -m "test(mocks): add MockWiFi with deterministic macAddress"
```

---

## Task 2: Implement `sanitizeDeviceSegment` with unit tests

**Files:**
- Create: `test/test_device_name/test_main.cpp`
- Modify: `include/Config.h`
- Modify: `src/Config.cpp`

### Steps

- [ ] **Step 1: Create `test/test_device_name/test_main.cpp` with failing sanitizer tests**

```cpp
#include <unity.h>
#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H
#include "../mocks/MockPreferences.h"
#include "../mocks/MockWiFi.h"

#include "Config.h"
#include "../../src/Config.cpp"

void setUp(void) {}
void tearDown(void) {}

// ---------- sanitizeDeviceSegment ----------

void test_sanitize_valid_identifier_passes_through() {
    TEST_ASSERT_EQUAL_STRING("neil-bedroom",
        Config::sanitizeDeviceSegment("neil-bedroom").c_str());
}

void test_sanitize_spaces_become_hyphens() {
    TEST_ASSERT_EQUAL_STRING("my-device",
        Config::sanitizeDeviceSegment("my device").c_str());
}

void test_sanitize_path_chars_replaced() {
    TEST_ASSERT_EQUAL_STRING("a-b-c-d",
        Config::sanitizeDeviceSegment("a/b\\c:d").c_str());
}

void test_sanitize_consecutive_invalid_collapse() {
    TEST_ASSERT_EQUAL_STRING("a-b",
        Config::sanitizeDeviceSegment("a   b").c_str());
}

void test_sanitize_leading_trailing_hyphens_trimmed() {
    TEST_ASSERT_EQUAL_STRING("foo",
        Config::sanitizeDeviceSegment("---foo---").c_str());
}

void test_sanitize_empty_input_returns_empty() {
    TEST_ASSERT_EQUAL_STRING("",
        Config::sanitizeDeviceSegment("").c_str());
}

void test_sanitize_all_invalid_returns_empty() {
    TEST_ASSERT_EQUAL_STRING("",
        Config::sanitizeDeviceSegment("///").c_str());
}

void test_sanitize_whitespace_only_returns_empty() {
    TEST_ASSERT_EQUAL_STRING("",
        Config::sanitizeDeviceSegment("   ").c_str());
}

void test_sanitize_caps_at_32_chars() {
    String forty_a(40, 'a');  // Arduino String has no fill ctor — see below
    // Workaround: build 40-char string manually
    String input;
    for (int i = 0; i < 40; ++i) input += 'a';
    String out = Config::sanitizeDeviceSegment(input);
    TEST_ASSERT_EQUAL_size_t(32, out.length());
    for (size_t i = 0; i < out.length(); ++i) {
        TEST_ASSERT_EQUAL_CHAR('a', out.charAt(i));
    }
}

void test_sanitize_trailing_hyphen_after_cap_is_trimmed() {
    // 31 a's then a dash — after 32-char cap the dash should be trimmed
    String input;
    for (int i = 0; i < 31; ++i) input += 'a';
    input += "-extra";  // cap will cut at "aaaa...aaaa-" (32nd char is '-')
    String out = Config::sanitizeDeviceSegment(input);
    TEST_ASSERT_EQUAL_size_t(31, out.length());
    TEST_ASSERT_NOT_EQUAL('-', out.charAt(out.length() - 1));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_sanitize_valid_identifier_passes_through);
    RUN_TEST(test_sanitize_spaces_become_hyphens);
    RUN_TEST(test_sanitize_path_chars_replaced);
    RUN_TEST(test_sanitize_consecutive_invalid_collapse);
    RUN_TEST(test_sanitize_leading_trailing_hyphens_trimmed);
    RUN_TEST(test_sanitize_empty_input_returns_empty);
    RUN_TEST(test_sanitize_all_invalid_returns_empty);
    RUN_TEST(test_sanitize_whitespace_only_returns_empty);
    RUN_TEST(test_sanitize_caps_at_32_chars);
    RUN_TEST(test_sanitize_trailing_hyphen_after_cap_is_trimmed);
    return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `source venv/bin/activate && pio test -e native -f test_device_name`
Expected: compile error — `Config::sanitizeDeviceSegment` is not declared.

- [ ] **Step 3: Declare the static helper in `include/Config.h`**

In the `public:` section (near line 170 where other `private:` static helpers live), add:

```cpp
    // Device segment helpers (also used for testing)
    static String sanitizeDeviceSegment(const String& raw);
    static String resolveDeviceSegment(const String& deviceName, const String& macAddress);
```

Place these inside the existing `public:` section before the closing `};`. Static helpers can be public even while member logic is encapsulated — they take no implicit `this`.

- [ ] **Step 4: Implement the sanitizer in `src/Config.cpp`**

Near the bottom of `src/Config.cpp` (after the existing getter definitions, before `#ifdef UNIT_TEST` blocks if any), add:

```cpp
String Config::sanitizeDeviceSegment(const String& raw) {
    String out;
    out.reserve(raw.length());
    bool lastWasHyphen = false;
    for (size_t i = 0; i < raw.length(); ++i) {
        char c = raw.charAt(i);
        bool keep = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
        if (keep) {
            out += c;
            lastWasHyphen = (c == '-');
        } else {
            if (!lastWasHyphen) {
                out += '-';
                lastWasHyphen = true;
            }
            // Else: collapse consecutive non-keep chars into a single hyphen
        }
    }
    // Trim leading hyphens
    while (out.length() > 0 && out.charAt(0) == '-') {
        out = out.substring(1);
    }
    // Trim trailing hyphens
    while (out.length() > 0 && out.charAt(out.length() - 1) == '-') {
        out = out.substring(0, out.length() - 1);
    }
    // Cap at 32 chars
    if (out.length() > 32) {
        out = out.substring(0, 32);
    }
    // Re-trim trailing hyphen in case truncation left one
    while (out.length() > 0 && out.charAt(out.length() - 1) == '-') {
        out = out.substring(0, out.length() - 1);
    }
    return out;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `source venv/bin/activate && pio test -e native -f test_device_name`
Expected: 10 tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/Config.h src/Config.cpp test/test_device_name/test_main.cpp
git commit -m "feat(config): add sanitizeDeviceSegment helper with unit tests"
```

---

## Task 3: Implement `resolveDeviceSegment` with unit tests

**Files:**
- Modify: `test/test_device_name/test_main.cpp` (add tests)
- Modify: `src/Config.cpp` (add resolver)

### Steps

- [ ] **Step 1: Append resolver tests to `test/test_device_name/test_main.cpp`**

Add these test functions before `main()`:

```cpp
// ---------- resolveDeviceSegment ----------

void test_resolve_name_unset_returns_sanitized_mac() {
    TEST_ASSERT_EQUAL_STRING("ac0bfb6fa194",
        Config::resolveDeviceSegment("", "AC:0B:FB:6F:A1:94").c_str());
}

void test_resolve_name_empty_string_falls_back_to_mac() {
    TEST_ASSERT_EQUAL_STRING("ac0bfb6fa194",
        Config::resolveDeviceSegment("", "AC:0B:FB:6F:A1:94").c_str());
}

void test_resolve_name_whitespace_only_falls_back_to_mac() {
    TEST_ASSERT_EQUAL_STRING("ac0bfb6fa194",
        Config::resolveDeviceSegment("   ", "AC:0B:FB:6F:A1:94").c_str());
}

void test_resolve_name_valid_returns_sanitized_name() {
    TEST_ASSERT_EQUAL_STRING("home-upload",
        Config::resolveDeviceSegment("home-upload", "AC:0B:FB:6F:A1:94").c_str());
}

void test_resolve_name_with_spaces_returns_sanitized_name() {
    TEST_ASSERT_EQUAL_STRING("home-upload",
        Config::resolveDeviceSegment("home upload!", "AC:0B:FB:6F:A1:94").c_str());
}

void test_resolve_mac_lowercased_and_stripped() {
    TEST_ASSERT_EQUAL_STRING("deadbeefcafe",
        Config::resolveDeviceSegment("", "DE:AD:BE:EF:CA:FE").c_str());
}
```

Then register them in `main()`:

```cpp
    RUN_TEST(test_resolve_name_unset_returns_sanitized_mac);
    RUN_TEST(test_resolve_name_empty_string_falls_back_to_mac);
    RUN_TEST(test_resolve_name_whitespace_only_falls_back_to_mac);
    RUN_TEST(test_resolve_name_valid_returns_sanitized_name);
    RUN_TEST(test_resolve_name_with_spaces_returns_sanitized_name);
    RUN_TEST(test_resolve_mac_lowercased_and_stripped);
```

- [ ] **Step 2: Run to verify failure**

Run: `source venv/bin/activate && pio test -e native -f test_device_name`
Expected: compile error — `Config::resolveDeviceSegment` is not declared.

- [ ] **Step 3: Implement the resolver in `src/Config.cpp`**

Add after `sanitizeDeviceSegment`:

```cpp
String Config::resolveDeviceSegment(const String& deviceName,
                                    const String& macAddress) {
    String sanitized = sanitizeDeviceSegment(deviceName);
    if (sanitized.length() > 0) {
        return sanitized;
    }
    // Fallback: WiFi MAC, colons stripped, lowercased, then sanitized
    String mac = macAddress;
    mac.replace(":", "");
    mac.toLowerCase();
    return sanitizeDeviceSegment(mac);
}
```

- [ ] **Step 4: Run to verify pass**

Run: `source venv/bin/activate && pio test -e native -f test_device_name`
Expected: 16 tests pass (10 sanitizer + 6 resolver).

- [ ] **Step 5: Commit**

```bash
git add include/Config.h src/Config.cpp test/test_device_name/test_main.cpp
git commit -m "feat(config): add resolveDeviceSegment with MAC fallback"
```

---

## Task 4: Wire `DEVICE_NAME` into Config load flow

**Files:**
- Modify: `include/Config.h` (fields + getters)
- Modify: `src/Config.cpp` (WiFi include, constructor, parser, loadFromSD tail, getters)
- Modify: `test/test_device_name/test_main.cpp` (add end-to-end loadFromSD test)

### Steps

- [ ] **Step 1: Append an end-to-end test to `test/test_device_name/test_main.cpp`**

Add a mock FS helper reset to `setUp`/`tearDown`, then these tests. Also add `#include "FS.h"` near the top if not already included via Arduino.cpp.

At the top of the file, after the existing mock includes:

```cpp
#include "FS.h"

fs::FS mockSD;

void setUp(void) {
    mockSD.clear();
    Preferences::clearAll();
    WiFi.setMockMacAddress("AC:0B:FB:6F:A1:94");
}

void tearDown(void) {
    Preferences::clearAll();
}
```

(If `setUp`/`tearDown` already exist at stub `{}`, replace them.)

Then append these test functions:

```cpp
// ---------- Config::loadFromSD integration ----------

static const char* MINIMAL_CONFIG =
    "WIFI_SSID = Net\n"
    "WIFI_PASSWORD = pass\n"
    "ENDPOINT = //192.168.0.108/share\n"
    "ENDPOINT_TYPE = SMB\n"
    "ENDPOINT_USER = u\n"
    "ENDPOINT_PASSWORD = p\n";

void test_config_device_segment_falls_back_to_mac_when_unset() {
    mockSD.addFile("/config.txt", MINIMAL_CONFIG);
    Config cfg;
    TEST_ASSERT_TRUE(cfg.loadFromSD(mockSD));
    TEST_ASSERT_EQUAL_STRING("ac0bfb6fa194", cfg.getDeviceSegment().c_str());
}

void test_config_device_segment_uses_sanitized_name_when_set() {
    std::string body = MINIMAL_CONFIG;
    body += "DEVICE_NAME = home upload!\n";
    mockSD.addFile("/config.txt", body);
    Config cfg;
    TEST_ASSERT_TRUE(cfg.loadFromSD(mockSD));
    TEST_ASSERT_EQUAL_STRING("home-upload", cfg.getDeviceSegment().c_str());
}

void test_config_device_segment_empty_name_falls_back_to_mac() {
    std::string body = MINIMAL_CONFIG;
    body += "DEVICE_NAME = \n";
    mockSD.addFile("/config.txt", body);
    Config cfg;
    TEST_ASSERT_TRUE(cfg.loadFromSD(mockSD));
    TEST_ASSERT_EQUAL_STRING("ac0bfb6fa194", cfg.getDeviceSegment().c_str());
}

void test_config_device_name_getter_returns_raw_value() {
    std::string body = MINIMAL_CONFIG;
    body += "DEVICE_NAME = home upload!\n";
    mockSD.addFile("/config.txt", body);
    Config cfg;
    TEST_ASSERT_TRUE(cfg.loadFromSD(mockSD));
    // Raw (pre-sanitization) value preserved for display/logging
    TEST_ASSERT_EQUAL_STRING("home upload!", cfg.getDeviceName().c_str());
}
```

Register in `main()`:

```cpp
    RUN_TEST(test_config_device_segment_falls_back_to_mac_when_unset);
    RUN_TEST(test_config_device_segment_uses_sanitized_name_when_set);
    RUN_TEST(test_config_device_segment_empty_name_falls_back_to_mac);
    RUN_TEST(test_config_device_name_getter_returns_raw_value);
```

- [ ] **Step 2: Run to confirm failure**

Run: `source venv/bin/activate && pio test -e native -f test_device_name`
Expected: compile error — `getDeviceSegment` / `getDeviceName` not declared.

- [ ] **Step 3: Add fields and getter declarations to `include/Config.h`**

In the `private:` block, alongside the other O2Ring/config fields (around line 81), add:

```cpp
    // Device identity (used for SMB path disambiguation)
    String deviceName;     // Raw user-supplied value (may be empty)
    String deviceSegment;  // Computed once during loadFromSD
```

In the `public:` block, alongside the other getters (near line 168), add:

```cpp
    // Device identity getters
    const String& getDeviceName() const;
    const String& getDeviceSegment() const;
```

- [ ] **Step 4: Add WiFi include with UNIT_TEST guard in `src/Config.cpp`**

Near the top of `src/Config.cpp`, after the existing includes, add:

```cpp
#ifdef UNIT_TEST
    #include "MockWiFi.h"
#else
    #include <WiFi.h>
#endif
```

- [ ] **Step 5: Initialize fields in the `Config::Config()` constructor**

Find the constructor initializer list in `src/Config.cpp` (around line 30-50 where `o2ringPath("oximetry/raw")` appears). Add `deviceName`/`deviceSegment` initialization alongside the other string defaults:

```cpp
    deviceName(""),
    deviceSegment(""),
```

Place these in the initializer list in declaration order relative to `Config.h` (compilers warn otherwise). Since we added the fields right after the o2ring block, place the initializers right after `o2ringScanSeconds(30)`.

- [ ] **Step 6: Add the `DEVICE_NAME` case to `setConfigValue`**

Find `setConfigValue` in `src/Config.cpp` (around line 238 where O2Ring keys are parsed). Add another branch alongside them:

```cpp
    } else if (key == "DEVICE_NAME") {
        deviceName = value;
```

Place it near other general-purpose keys or near the O2Ring block — either is fine. Match the surrounding `else if` chain style.

- [ ] **Step 7: Compute `deviceSegment` at end of `loadFromSD`**

Find the end of `Config::loadFromSD` in `src/Config.cpp` where `isValid = true;` is set (search for `isValid = true`). Just before `return isValid;` / `return true;`, add:

```cpp
    // Compute device segment for SMB path disambiguation
    deviceSegment = resolveDeviceSegment(deviceName, WiFi.macAddress());
    LOG_DEBUGF("[Config] Device segment resolved to: %s", deviceSegment.c_str());
```

- [ ] **Step 8: Implement the getters in `src/Config.cpp`**

Near the existing getter block (where `getO2RingPath` lives, around line 660), add:

```cpp
const String& Config::getDeviceName() const { return deviceName; }
const String& Config::getDeviceSegment() const { return deviceSegment; }
```

- [ ] **Step 9: Run the full native suite**

Run: `source venv/bin/activate && pio test -e native`
Expected: all suites pass, including the new 4 integration tests in `test_device_name` (20 total), the existing `test_config`, `test_credential_migration`, and `test_o2ring_sync` (which now compile against the WiFi mock added in Task 1).

- [ ] **Step 10: Commit**

```bash
git add include/Config.h src/Config.cpp test/test_device_name/test_main.cpp
git commit -m "feat(config): parse DEVICE_NAME and compute deviceSegment at load"
```

---

## Task 5: Compose nested path in O2RingSync

**Files:**
- Modify: `src/O2RingSync.cpp:108-109`

### Steps

- [ ] **Step 1: Read current path composition**

Open `src/O2RingSync.cpp` at lines 105-115. Current code:

```cpp
    String remotePath = "/" + config->getO2RingPath() + "/" + filename;
    smb.createDirectory("/" + config->getO2RingPath());
    bool uploaded = smb.uploadRawBuffer(remotePath, fileData.data(), fileData.size());
```

- [ ] **Step 2: Replace with device-segment-aware composition**

```cpp
    String dir = "/" + config->getO2RingPath() + "/" + config->getDeviceSegment();
    smb.createDirectory(dir);
    String remotePath = dir + "/" + filename;
    bool uploaded = smb.uploadRawBuffer(remotePath, fileData.data(), fileData.size());
```

`SMBUploader::createDirectory` walks parents recursively (see `src/SMBUploader.cpp:353-361`), so the single call creates both `<base>` and `<base>/<device>` if needed.

- [ ] **Step 3: Build the firmware to verify no breakage**

Run: `source venv/bin/activate && pio run -e pico32`
Expected: SUCCESS, flash size slightly up, under 3 MB huge_app.

- [ ] **Step 4: Run the full native suite again**

Run: `source venv/bin/activate && pio test -e native`
Expected: all pass. `test_o2ring_sync` may need a Config DEVICE_NAME field for its mock config to make paths predictable — only update it if a test fails. If it passes (device segment falls back to MAC under the MockWiFi default), leave alone.

- [ ] **Step 5: Commit**

```bash
git add src/O2RingSync.cpp
git commit -m "feat(o2ring): upload into /<path>/<device>/ subdirectory"
```

---

## Task 6: Documentation

**Files:**
- Modify: `docs/CONFIG_REFERENCE.md`
- Modify: `docs/DEVELOPMENT.md`
- Modify: `CLAUDE.md` (the one at `CPAP_data_uploader/CLAUDE.md`)

### Steps

- [ ] **Step 1: Add `DEVICE_NAME` to `docs/CONFIG_REFERENCE.md`**

Find the first config-key table (around the top, after WIFI_* keys). Add a new row. Exact placement: after general connectivity keys (HOSTNAME, etc.) and before endpoint-specific keys. If a general-system table doesn't exist, add the row to the system/general section:

```markdown
| `DEVICE_NAME` | *(empty)* | Human-readable identifier for this uploader, used as the `<device>` segment in the O2Ring SMB path (`<O2RING_PATH>/<device>/...`). Sanitized to `[a-zA-Z0-9_-]`, capped at 32 chars. If empty, the WiFi MAC (colons stripped, lowercased) is used. |
```

- [ ] **Step 2: Update O2Ring section of `docs/DEVELOPMENT.md`**

Search for an O2Ring or BLE section. Add a paragraph (or update an existing path description):

```markdown
**O2Ring SMB layout.** Uploaded `.vld` files land at:

    <ENDPOINT>/<O2RING_PATH>/<deviceSegment>/<filename>.vld

The server-side oximetry staging pipeline requires the per-device subdirectory. `deviceSegment` is resolved once at config load: sanitized `DEVICE_NAME` from `config.txt` if set, else the WiFi MAC (colons stripped, lowercased). Don't write directly into `<O2RING_PATH>` — those uploads will be rejected.
```

If no O2Ring section exists in DEVELOPMENT.md, append a new H3 section to the file.

- [ ] **Step 3: Update `CPAP_data_uploader/CLAUDE.md`**

Find the BLE / O2Ring layer paragraph (mentions `O2RingSync`, `IBleClient`, NVS dedup). Append a sentence:

```markdown
O2Ring `.vld` uploads land at `<O2RING_PATH>/<deviceSegment>/<filename>.vld` — the server-side staging pipeline rejects uploads that skip the `<deviceSegment>` subdir. `deviceSegment` comes from `DEVICE_NAME` in `config.txt`, falling back to the WiFi MAC.
```

- [ ] **Step 4: Commit**

```bash
git add docs/CONFIG_REFERENCE.md docs/DEVELOPMENT.md CLAUDE.md
git commit -m "docs(o2ring): document DEVICE_NAME and per-device SMB subdir"
```

---

## Self-Review

- **Spec coverage:**
  - Config surface (`DEVICE_NAME` key): Task 4 ✓
  - Default unchanged for `O2RING_PATH`: Task 4 does not modify it ✓
  - Resolution rule (sanitized name → MAC fallback): Task 3 ✓
  - Sanitizer rules (all 5 steps): Task 2 tests enforce each ✓
  - Path composition: Task 5 ✓
  - WiFi MAC availability: Task 4 Step 4 guards production include, Task 1 provides mock ✓
  - Testing (sanitizer + resolver + load integration): Tasks 2-4 ✓
  - Docs (CONFIG_REFERENCE + DEVELOPMENT + CLAUDE): Task 6 ✓
- **Placeholder scan:** all code blocks contain real code; no TBD/TODO/"add appropriate X".
- **Type consistency:** `sanitizeDeviceSegment(const String&) -> String` and `resolveDeviceSegment(const String&, const String&) -> String` match across Tasks 2-4. `getDeviceSegment() -> const String&` and `getDeviceName() -> const String&` match across Tasks 4 and 5. `deviceName` and `deviceSegment` field names match across Tasks 4 and 5.
