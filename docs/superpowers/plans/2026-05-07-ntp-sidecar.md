# NTP Sidecar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Firmware writes a `<filename>.ntp.json` sidecar next to every per-session EDF copied to SMB, capturing NTP witness time + EDF header times so Sleep ingest can populate `Sessions.ClockOffsetSec`.

**Architecture:** New static helper class `NtpSidecarWriter` (header parse, JSON serialize, witness capture, sink-abstracted write). Called from `FileUploader::uploadSingleFileSmb` for paths matching `/DATALOG/yyyymmdd/*.edf`. Pre-copy capture (witness time + FAT mtime + EDF header), post-copy write (JSON via `SMBUploader::uploadRawBuffer`). SMB-only, DATALOG-only.

**Tech Stack:** C++11, PlatformIO + Arduino framework on ESP32 (env `pico32`), Unity test framework on `native` env, libsmb2 component for SMB writes, existing mocks `MockFS`/`MockTime`/`MockSMBUploader`.

**Spec:** `docs/superpowers/specs/2026-05-07-ntp-sidecar-design.md`

---

## File Structure

| File | Purpose | Status |
|---|---|---|
| `include/NtpSidecarWriter.h` | Public types (`SidecarPayload`, `EdfHeader`, `SkipReason`), `SidecarSinkFn` typedef, class declaration | Create |
| `src/NtpSidecarWriter.cpp` | Implementation: `isDatalogEdf`, `parseEdfHeader`, `isNtpSynced`, `serializeJson`, `captureWitness`, `write` | Create |
| `src/FileUploader.cpp` | Wire `NtpSidecarWriter` calls into `uploadSingleFileSmb` (~10 lines) | Modify |
| `test/test_ntp_sidecar/test_ntp_sidecar.cpp` | Native unit tests (Unity) | Create |
| `test/mocks/MockFS.h` | Add `MockFile::getLastWrite()` + `MockFS::setLastWrite(path, time_t)` | Modify |

No changes to `platformio.ini` (existing `pico32` and `native` envs work for new sources). No changes to `config.txt` schema.

---

## Conventions for All Tasks

- **Working directory:** `/opt/homelab/sleep/CPAP_data_uploader/.worktrees/ntp-sidecar` (worktree on branch `feat/ntp-sidecar`).
- **Source layout:** Per-test-suite binaries — each `test/test_X/test_X.cpp` directly `#include`s the source files it depends on (see `test/test_upload_state_manager/test_upload_state_manager.cpp` for the pattern). Tests compile via `pio test -e native -f test_<name>`.
- **Run a single test suite:** `source venv/bin/activate && pio test -e native -f test_ntp_sidecar`
- **Compile-firmware sanity:** `source venv/bin/activate && pio run -e pico32` (full build, slow). Used as a final sanity check, not per-task.
- **Commit cadence:** one commit per task, after the test passes. Conventional commit format `feat(ntp-sidecar): ...` / `test(ntp-sidecar): ...` / `refactor(...)` / `docs(...)`.
- **Co-author trailer:** every commit ends with `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 1: Test scaffolding + `isDatalogEdf` predicate

**Files:**
- Create: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`
- Create: `include/NtpSidecarWriter.h`
- Create: `src/NtpSidecarWriter.cpp`

- [ ] **Step 1: Create test file with failing tests for `isDatalogEdf`**

Write `test/test_ntp_sidecar/test_ntp_sidecar.cpp`:

```cpp
#include <unity.h>
#include "Arduino.h"
#include "MockTime.h"
#include "MockFS.h"
#include "MockLogger.h"

// Include mock implementations
#include "../mocks/Arduino.cpp"

// Mock ArduinoJson (not used yet, but stays available for later)
#include "../mocks/ArduinoJson.h"

// Stub out FIRMWARE_VERSION before including NtpSidecarWriter
#define FIRMWARE_VERSION "test-v0.0.0"

// Prevent real Logger.h
#define LOGGER_H

#include "NtpSidecarWriter.h"
#include "../../src/NtpSidecarWriter.cpp"

void setUp(void) { MockTimeState::reset(); }
void tearDown(void) {}

// ── isDatalogEdf ────────────────────────────────────────────────────────

void test_isDatalogEdf_matches_session_pld(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/20260507_223015_PLD.edf")));
}

void test_isDatalogEdf_matches_session_brp(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/20260507_223015_BRP.edf")));
}

void test_isDatalogEdf_matches_session_eve(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/20260507_223015_EVE.edf")));
}

void test_isDatalogEdf_matches_session_csl(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/20260507_223015_CSL.edf")));
}

void test_isDatalogEdf_case_insensitive_extension(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/x.EDF")));
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/x.Edf")));
}

void test_isDatalogEdf_rejects_root_str(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(String("/STR.edf")));
}

void test_isDatalogEdf_rejects_state_files(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/.upload_state.v2.smb")));
}

void test_isDatalogEdf_rejects_non_8digit_folder(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/foo/bar.edf")));
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/2026050/bar.edf")));   // 7 digits
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/202605070/bar.edf"))); // 9 digits
}

void test_isDatalogEdf_rejects_extra_path_segments(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/sub/x.edf")));
}

void test_isDatalogEdf_rejects_non_edf_extension(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/x.txt")));
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/x")));
}

void test_isDatalogEdf_rejects_empty_filename(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/.edf")));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_isDatalogEdf_matches_session_pld);
    RUN_TEST(test_isDatalogEdf_matches_session_brp);
    RUN_TEST(test_isDatalogEdf_matches_session_eve);
    RUN_TEST(test_isDatalogEdf_matches_session_csl);
    RUN_TEST(test_isDatalogEdf_case_insensitive_extension);
    RUN_TEST(test_isDatalogEdf_rejects_root_str);
    RUN_TEST(test_isDatalogEdf_rejects_state_files);
    RUN_TEST(test_isDatalogEdf_rejects_non_8digit_folder);
    RUN_TEST(test_isDatalogEdf_rejects_extra_path_segments);
    RUN_TEST(test_isDatalogEdf_rejects_non_edf_extension);
    RUN_TEST(test_isDatalogEdf_rejects_empty_filename);
    return UNITY_END();
}
```

Create `include/NtpSidecarWriter.h` with the empty class shell (so the include resolves):

```cpp
#ifndef NTP_SIDECAR_WRITER_H
#define NTP_SIDECAR_WRITER_H

#ifdef UNIT_TEST
#include "Arduino.h"
#else
#include <Arduino.h>
#endif

class NtpSidecarWriter {
public:
    static bool isDatalogEdf(const String& path);
};

#endif // NTP_SIDECAR_WRITER_H
```

Create `src/NtpSidecarWriter.cpp` with an intentionally-wrong stub so the test fails:

```cpp
#include "NtpSidecarWriter.h"

bool NtpSidecarWriter::isDatalogEdf(const String& /*path*/) {
    return false;  // intentional: tests must drive impl
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: 11 tests run, the 5 "matches" tests FAIL (stub returns false), the 6 "rejects" tests pass trivially.

- [ ] **Step 3: Implement `isDatalogEdf` properly**

Replace the body in `src/NtpSidecarWriter.cpp`:

```cpp
#include "NtpSidecarWriter.h"

#include <cctype>

namespace {

bool isAllDigits(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!isdigit(static_cast<unsigned char>(p[i]))) return false;
    }
    return true;
}

bool endsWithEdfCaseInsensitive(const char* s, size_t len) {
    if (len < 4) return false;
    return (s[len-4] == '.') &&
           (tolower(static_cast<unsigned char>(s[len-3])) == 'e') &&
           (tolower(static_cast<unsigned char>(s[len-2])) == 'd') &&
           (tolower(static_cast<unsigned char>(s[len-1])) == 'f');
}

}  // namespace

bool NtpSidecarWriter::isDatalogEdf(const String& path) {
    const char* s = path.c_str();
    const size_t n = path.length();

    // Must start with "/DATALOG/"
    static const char kPrefix[] = "/DATALOG/";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;  // 9
    if (n < kPrefixLen) return false;
    for (size_t i = 0; i < kPrefixLen; ++i) {
        if (s[i] != kPrefix[i]) return false;
    }

    // Next 8 chars must be digits (the yyyymmdd folder)
    if (n < kPrefixLen + 8) return false;
    if (!isAllDigits(s + kPrefixLen, 8)) return false;

    // Then exactly one '/'
    if (s[kPrefixLen + 8] != '/') return false;

    const char* fname = s + kPrefixLen + 8 + 1;
    const size_t fnameLen = n - (kPrefixLen + 8 + 1);

    // Filename must not be empty before .edf
    if (fnameLen <= 4) return false;  // ".edf" alone is not a filename

    // Must end in .edf (case-insensitive)
    if (!endsWithEdfCaseInsensitive(fname, fnameLen)) return false;

    // No further path separators (rejects /DATALOG/20260507/sub/x.edf)
    for (size_t i = 0; i < fnameLen; ++i) {
        if (fname[i] == '/') return false;
    }

    return true;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `11 Tests 0 Failures 0 Ignored` and overall PASSED.

- [ ] **Step 5: Commit**

```bash
git add include/NtpSidecarWriter.h \
        src/NtpSidecarWriter.cpp \
        test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): isDatalogEdf path predicate

Matches /DATALOG/<yyyymmdd>/*.edf only (case-insensitive on .edf).
Rejects /STR.edf, state files, non-8-digit folders, nested paths,
non-.edf extensions, and empty filenames.

Part of admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `parseEdfHeader` (EDF byte parse → `EdfHeader` struct)

**Files:**
- Modify: `include/NtpSidecarWriter.h`
- Modify: `src/NtpSidecarWriter.cpp`
- Modify: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`

**Background:** EDF fixed header is 256 bytes. Field layout:
- Offset 168: 8 bytes ASCII `dd.MM.yy` (start date)
- Offset 176: 8 bytes ASCII `HH.mm.ss` (start time)
- Offset 184: 8 bytes ASCII (reserved — number of bytes in header record; not used here)
- Offset 192: 44 bytes reserved
- Offset 236: 8 bytes ASCII (number of data records, integer or "-1")
- Offset 244: 8 bytes ASCII (duration of a data record, in seconds, may be float)

The two-digit year `yy` in `dd.MM.yy` is interpreted as `2000+yy` (EDF spec: years 1985–2084, with `00–84` → `2000–2084`, `85–99` → `1985–1999`). For the AirSense 11 we only need `2000+yy` (no negative dates anywhere near 1985 are physically possible).

`durationSeconds = round(num_records * record_duration)`. We round to integer because the sidecar schema requires integer seconds.

- [ ] **Step 1: Add types to header and write failing tests**

Update `include/NtpSidecarWriter.h`:

```cpp
#ifndef NTP_SIDECAR_WRITER_H
#define NTP_SIDECAR_WRITER_H

#ifdef UNIT_TEST
#include "Arduino.h"
#include "MockFS.h"
#else
#include <Arduino.h>
#include <FS.h>
#endif

struct EdfHeader {
    char startNaiveStr[20];   // "YYYY-MM-DDTHH:MM:SS" + null
    int  durationSeconds;
};

class NtpSidecarWriter {
public:
    static bool isDatalogEdf(const String& path);

    // Reads the first 256 bytes of an open File and parses the EDF header
    // start time (offset 168/176) and duration (offsets 236+244).
    // Returns false on short read or malformed fields.
#ifdef UNIT_TEST
    static bool parseEdfHeader(MockFile& f, EdfHeader& out);
#else
    static bool parseEdfHeader(fs::File& f, EdfHeader& out);
#endif
};

#endif // NTP_SIDECAR_WRITER_H
```

Append tests to `test/test_ntp_sidecar/test_ntp_sidecar.cpp` (add `RUN_TEST` lines in `main` for each):

```cpp
// ── parseEdfHeader ──────────────────────────────────────────────────────

namespace {
// Build a 256-byte EDF header buffer in-memory. Returns vector<uint8_t>
// suitable for MockFS::addFile(path, content).
std::vector<uint8_t> makeEdfHeader(const char* dateDdMmYy,
                                   const char* timeHhMmSs,
                                   const char* numRecords,
                                   const char* recordDuration) {
    std::vector<uint8_t> buf(256, ' ');  // ASCII spaces fill reserved fields
    auto put = [&](size_t off, const char* s, size_t fieldLen) {
        size_t n = std::min(strlen(s), fieldLen);
        memcpy(buf.data() + off, s, n);
        // pad remainder with spaces (already done by initialiser)
    };
    // version (offset 0, 8 bytes): "0       "
    put(0, "0", 8);
    // patient id, recording id, etc. left as spaces
    put(168, dateDdMmYy, 8);
    put(176, timeHhMmSs, 8);
    put(236, numRecords, 8);
    put(244, recordDuration, 8);
    return buf;
}
}  // namespace

MockFS edfFS;

void test_parseEdfHeader_valid_full_pld(void) {
    edfFS.clear();
    auto buf = makeEdfHeader("04.05.26", "22.29.52", "13650", "2");
    edfFS.addFile(String("/sample.edf"), buf);
    MockFile f = edfFS.open(String("/sample.edf"));
    EdfHeader out{};
    TEST_ASSERT_TRUE(NtpSidecarWriter::parseEdfHeader(f, out));
    TEST_ASSERT_EQUAL_STRING("2026-05-04T22:29:52", out.startNaiveStr);
    TEST_ASSERT_EQUAL_INT(27300, out.durationSeconds);
}

void test_parseEdfHeader_short_read(void) {
    edfFS.clear();
    std::vector<uint8_t> tiny(100, ' ');
    edfFS.addFile(String("/short.edf"), tiny);
    MockFile f = edfFS.open(String("/short.edf"));
    EdfHeader out{};
    TEST_ASSERT_FALSE(NtpSidecarWriter::parseEdfHeader(f, out));
}

void test_parseEdfHeader_bad_date_format(void) {
    edfFS.clear();
    auto buf = makeEdfHeader("xx.yy.zz", "22.29.52", "100", "2");
    edfFS.addFile(String("/bad.edf"), buf);
    MockFile f = edfFS.open(String("/bad.edf"));
    EdfHeader out{};
    TEST_ASSERT_FALSE(NtpSidecarWriter::parseEdfHeader(f, out));
}

void test_parseEdfHeader_bad_time_format(void) {
    edfFS.clear();
    auto buf = makeEdfHeader("04.05.26", "ab.29.52", "100", "2");
    edfFS.addFile(String("/badt.edf"), buf);
    MockFile f = edfFS.open(String("/badt.edf"));
    EdfHeader out{};
    TEST_ASSERT_FALSE(NtpSidecarWriter::parseEdfHeader(f, out));
}

void test_parseEdfHeader_fractional_record_duration(void) {
    edfFS.clear();
    auto buf = makeEdfHeader("04.05.26", "22.29.52", "100", "2.5");
    edfFS.addFile(String("/frac.edf"), buf);
    MockFile f = edfFS.open(String("/frac.edf"));
    EdfHeader out{};
    TEST_ASSERT_TRUE(NtpSidecarWriter::parseEdfHeader(f, out));
    TEST_ASSERT_EQUAL_INT(250, out.durationSeconds);
}

void test_parseEdfHeader_zero_records(void) {
    edfFS.clear();
    auto buf = makeEdfHeader("04.05.26", "22.29.52", "0", "2");
    edfFS.addFile(String("/zero.edf"), buf);
    MockFile f = edfFS.open(String("/zero.edf"));
    EdfHeader out{};
    TEST_ASSERT_TRUE(NtpSidecarWriter::parseEdfHeader(f, out));
    TEST_ASSERT_EQUAL_INT(0, out.durationSeconds);
}

void test_parseEdfHeader_negative_records_treated_as_invalid(void) {
    // EDF allows num_records="-1" (unknown). We treat that as invalid for
    // our purposes — without a known duration the sidecar is useless.
    edfFS.clear();
    auto buf = makeEdfHeader("04.05.26", "22.29.52", "-1", "2");
    edfFS.addFile(String("/neg.edf"), buf);
    MockFile f = edfFS.open(String("/neg.edf"));
    EdfHeader out{};
    TEST_ASSERT_FALSE(NtpSidecarWriter::parseEdfHeader(f, out));
}
```

Add to `main()`:

```cpp
    RUN_TEST(test_parseEdfHeader_valid_full_pld);
    RUN_TEST(test_parseEdfHeader_short_read);
    RUN_TEST(test_parseEdfHeader_bad_date_format);
    RUN_TEST(test_parseEdfHeader_bad_time_format);
    RUN_TEST(test_parseEdfHeader_fractional_record_duration);
    RUN_TEST(test_parseEdfHeader_zero_records);
    RUN_TEST(test_parseEdfHeader_negative_records_treated_as_invalid);
```

Add a stub to `src/NtpSidecarWriter.cpp` so it compiles:

```cpp
#ifdef UNIT_TEST
bool NtpSidecarWriter::parseEdfHeader(MockFile& /*f*/, EdfHeader& /*out*/) {
    return false;
}
#else
bool NtpSidecarWriter::parseEdfHeader(fs::File& /*f*/, EdfHeader& /*out*/) {
    return false;
}
#endif
```

- [ ] **Step 2: Run tests to verify failure**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: 4 of the 7 new parseEdfHeader tests FAIL (the ones expecting `true`); the 3 "invalid input" tests pass trivially.

- [ ] **Step 3: Implement `parseEdfHeader`**

Replace the stub with the real implementation. Add the `<cmath>` and `<cstdlib>` includes near the top of `src/NtpSidecarWriter.cpp`:

```cpp
#include <cmath>
#include <cstdlib>
#include <cstring>
```

Helpers (file-static, anonymous namespace) and impl:

```cpp
namespace {

// Parse exactly nDigits ASCII decimal digits. Returns false if any non-digit.
bool parseUInt(const char* p, size_t nDigits, int& out) {
    int v = 0;
    for (size_t i = 0; i < nDigits; ++i) {
        if (!isdigit(static_cast<unsigned char>(p[i]))) return false;
        v = v * 10 + (p[i] - '0');
    }
    out = v;
    return true;
}

// Trim leading/trailing ASCII spaces and return a null-terminated copy in dst.
void trimToCStr(const char* src, size_t srcLen, char* dst, size_t dstCap) {
    size_t lo = 0, hi = srcLen;
    while (lo < hi && src[lo] == ' ') ++lo;
    while (hi > lo && src[hi - 1] == ' ') --hi;
    size_t n = hi - lo;
    if (n + 1 > dstCap) n = dstCap - 1;
    memcpy(dst, src + lo, n);
    dst[n] = '\0';
}

}  // namespace

#ifdef UNIT_TEST
static bool readHeaderBytes(MockFile& f, uint8_t* hdr) {
    return f.read(hdr, 256) == 256;
}
#else
static bool readHeaderBytes(fs::File& f, uint8_t* hdr) {
    return f.read(hdr, 256) == 256;
}
#endif

#ifdef UNIT_TEST
bool NtpSidecarWriter::parseEdfHeader(MockFile& f, EdfHeader& out)
#else
bool NtpSidecarWriter::parseEdfHeader(fs::File& f, EdfHeader& out)
#endif
{
    uint8_t hdr[256];
    if (!readHeaderBytes(f, hdr)) return false;

    // Date dd.MM.yy at offset 168 (8 bytes), Time HH.mm.ss at offset 176.
    const char* date = reinterpret_cast<const char*>(hdr + 168);
    const char* time = reinterpret_cast<const char*>(hdr + 176);
    if (date[2] != '.' || date[5] != '.') return false;
    if (time[2] != '.' || time[5] != '.') return false;

    int dd = 0, mm = 0, yy = 0, HH = 0, MM = 0, SS = 0;
    if (!parseUInt(date + 0, 2, dd)) return false;
    if (!parseUInt(date + 3, 2, mm)) return false;
    if (!parseUInt(date + 6, 2, yy)) return false;
    if (!parseUInt(time + 0, 2, HH)) return false;
    if (!parseUInt(time + 3, 2, MM)) return false;
    if (!parseUInt(time + 6, 2, SS)) return false;

    // EDF spec: yy in 00..84 → 2000+yy, 85..99 → 1900+yy. AirSense 11 only
    // produces 20xx, but we honour the spec for forensic robustness.
    int year = (yy <= 84) ? (2000 + yy) : (1900 + yy);

    if (mm < 1 || mm > 12) return false;
    if (dd < 1 || dd > 31) return false;
    if (HH > 23 || MM > 59 || SS > 60) return false;  // 60 = leap second

    snprintf(out.startNaiveStr, sizeof(out.startNaiveStr),
             "%04d-%02d-%02dT%02d:%02d:%02d", year, mm, dd, HH, MM, SS);

    // num_records at offset 236 (8 bytes ASCII), record_duration at 244.
    char numRecStr[16] = {0};
    char recDurStr[16] = {0};
    trimToCStr(reinterpret_cast<const char*>(hdr + 236), 8,
               numRecStr, sizeof(numRecStr));
    trimToCStr(reinterpret_cast<const char*>(hdr + 244), 8,
               recDurStr, sizeof(recDurStr));

    char* end = nullptr;
    long numRec = strtol(numRecStr, &end, 10);
    if (end == numRecStr || *end != '\0') return false;
    if (numRec < 0) return false;  // -1 means "unknown" — useless for us

    double recDur = strtod(recDurStr, &end);
    if (end == recDurStr || *end != '\0') return false;
    if (recDur < 0.0) return false;

    double total = static_cast<double>(numRec) * recDur;
    out.durationSeconds = static_cast<int>(std::lround(total));
    return true;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `18 Tests 0 Failures 0 Ignored` (11 prior + 7 new) PASSED.

- [ ] **Step 5: Commit**

```bash
git add include/NtpSidecarWriter.h \
        src/NtpSidecarWriter.cpp \
        test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): parseEdfHeader extracts start + duration

Reads the 256-byte EDF fixed header and pulls dd.MM.yy / HH.mm.ss
from offsets 168/176 and num_records / record_duration from
offsets 236/244. Output is ISO 8601 naive and integer seconds.
Rejects short reads, malformed numerics, and num_records < 0.

Part of admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `isNtpSynced` epoch threshold

**Files:**
- Modify: `include/NtpSidecarWriter.h`
- Modify: `src/NtpSidecarWriter.cpp`
- Modify: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`

- [ ] **Step 1: Declare + write failing tests**

Add to `include/NtpSidecarWriter.h` inside the class:

```cpp
    // Returns true if `now` looks like a real NTP-synced time
    // (after 2024-01-01 00:00:00 UTC = 1704067200).
    static bool isNtpSynced(time_t now);
```

Append tests to `test/test_ntp_sidecar/test_ntp_sidecar.cpp`:

```cpp
// ── isNtpSynced ─────────────────────────────────────────────────────────

void test_isNtpSynced_zero_is_unsynced(void) {
    TEST_ASSERT_FALSE(NtpSidecarWriter::isNtpSynced(0));
}

void test_isNtpSynced_pre_2024_is_unsynced(void) {
    // 2023-12-31 23:59:59 UTC = 1704067199
    TEST_ASSERT_FALSE(NtpSidecarWriter::isNtpSynced(1704067199));
}

void test_isNtpSynced_2024_boundary_is_synced(void) {
    // 2024-01-01 00:00:00 UTC = 1704067200
    TEST_ASSERT_TRUE(NtpSidecarWriter::isNtpSynced(1704067200));
}

void test_isNtpSynced_modern_time_is_synced(void) {
    // 2026-05-07 ≈ 1778500000
    TEST_ASSERT_TRUE(NtpSidecarWriter::isNtpSynced(1778500000));
}
```

Add to `main()`:

```cpp
    RUN_TEST(test_isNtpSynced_zero_is_unsynced);
    RUN_TEST(test_isNtpSynced_pre_2024_is_unsynced);
    RUN_TEST(test_isNtpSynced_2024_boundary_is_synced);
    RUN_TEST(test_isNtpSynced_modern_time_is_synced);
```

Add a stub to `src/NtpSidecarWriter.cpp`:

```cpp
bool NtpSidecarWriter::isNtpSynced(time_t /*now*/) {
    return false;
}
```

- [ ] **Step 2: Run tests to verify failure**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: 2 of the 4 new tests FAIL (the synced-true cases).

- [ ] **Step 3: Implement**

Replace the stub:

```cpp
bool NtpSidecarWriter::isNtpSynced(time_t now) {
    static const time_t kEpoch2024Utc = 1704067200;  // 2024-01-01T00:00:00Z
    return now >= kEpoch2024Utc;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `22 Tests 0 Failures 0 Ignored` PASSED.

- [ ] **Step 5: Commit**

```bash
git add include/NtpSidecarWriter.h \
        src/NtpSidecarWriter.cpp \
        test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): isNtpSynced epoch threshold check

Anything before 2024-01-01 UTC counts as unsynced (firmware
default-clock or boot-before-NTP-fetch). This gate is what
captureWitness uses to decide whether to skip the sidecar.

Part of admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `serializeJson` (build sidecar JSON to a stack buffer)

**Files:**
- Modify: `include/NtpSidecarWriter.h`
- Modify: `src/NtpSidecarWriter.cpp`
- Modify: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`

**Background:** Sidecar JSON is fixed-format. We build it with `snprintf` to a 512-byte stack buffer. The `fat_mtime` field is omitted when its source value is 0 (FAT mtime unset). Format example:

```json
{"schema_version":1,"ntp_observed_at":"2026-05-07T01:39:27Z","fat_mtime":"2026-05-07T01:39:25Z","edf_header_start":"2026-05-06T22:29:52","edf_header_duration_seconds":27300,"uploader_poll_interval_seconds":30,"uploader_firmware_version":"v1.4.2-dev+7"}
```

UTC timestamps use `gmtime_r` for portability (works on both ESP-IDF and native). Worst-case length: 7 keys + value formatting + commas + braces ≈ 350 bytes; 512-byte buffer leaves ample margin.

- [ ] **Step 1: Declare types + write failing tests**

Add to `include/NtpSidecarWriter.h` (alongside the existing types):

```cpp
enum class SkipReason {
    NONE,
    NTP_UNSYNCED,
    EDF_PARSE_FAILED
};

struct SidecarPayload {
    bool       valid;
    SkipReason skipReason;
    time_t     ntp_observed_at_unix;
    time_t     fat_mtime_unix;          // 0 ⇒ omit field from JSON
    EdfHeader  header;
    int        pollIntervalSeconds;
    const char* firmwareVersion;        // e.g. FIRMWARE_VERSION
};
```

Add inside the class:

```cpp
    // Serializes the payload as compact JSON into `buf` (capacity `cap`).
    // Returns the number of bytes written (excluding the null terminator),
    // or 0 if the buffer is too small.
    static size_t serializeJson(const SidecarPayload& p, char* buf, size_t cap);
```

Append tests:

```cpp
// ── serializeJson ───────────────────────────────────────────────────────

namespace {
SidecarPayload makeSamplePayload(time_t fatMtime = 1746603565) {
    SidecarPayload p{};
    p.valid = true;
    p.skipReason = SkipReason::NONE;
    p.ntp_observed_at_unix = 1746603567;  // 2026-05-07T01:39:27Z
    p.fat_mtime_unix       = fatMtime;    // 2026-05-07T01:39:25Z
    snprintf(p.header.startNaiveStr, sizeof(p.header.startNaiveStr),
             "%s", "2026-05-06T22:29:52");
    p.header.durationSeconds = 27300;
    p.pollIntervalSeconds = 30;
    p.firmwareVersion = "v1.4.2-dev+7";
    return p;
}
}  // namespace

void test_serializeJson_includes_all_fields(void) {
    char buf[512] = {0};
    auto p = makeSamplePayload();
    size_t n = NtpSidecarWriter::serializeJson(p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"schema_version\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ntp_observed_at\":\"2026-05-07T01:39:27Z\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fat_mtime\":\"2026-05-07T01:39:25Z\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"edf_header_start\":\"2026-05-06T22:29:52\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"edf_header_duration_seconds\":27300"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uploader_poll_interval_seconds\":30"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uploader_firmware_version\":\"v1.4.2-dev+7\""));
}

void test_serializeJson_omits_fat_mtime_when_zero(void) {
    char buf[512] = {0};
    auto p = makeSamplePayload(0);  // mtime unset
    size_t n = NtpSidecarWriter::serializeJson(p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NULL(strstr(buf, "fat_mtime"));
    // Other fields still present
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ntp_observed_at\":\"2026-05-07T01:39:27Z\""));
}

void test_serializeJson_iso_utc_format(void) {
    char buf[512] = {0};
    auto p = makeSamplePayload();
    NtpSidecarWriter::serializeJson(p, buf, sizeof(buf));
    // ntp_observed_at and fat_mtime end with Z
    const char* ntp = strstr(buf, "\"ntp_observed_at\":\"");
    TEST_ASSERT_NOT_NULL(ntp);
    TEST_ASSERT_EQUAL_CHAR('Z', ntp[strlen("\"ntp_observed_at\":\"") + 19]);
    // edf_header_start has no Z
    const char* hdr = strstr(buf, "\"edf_header_start\":\"");
    TEST_ASSERT_NOT_NULL(hdr);
    TEST_ASSERT_EQUAL_CHAR('"', hdr[strlen("\"edf_header_start\":\"") + 19]);
}

void test_serializeJson_buffer_too_small_returns_zero(void) {
    char small[16] = {0};
    auto p = makeSamplePayload();
    size_t n = NtpSidecarWriter::serializeJson(p, small, sizeof(small));
    TEST_ASSERT_EQUAL_UINT(0, n);
}
```

Add to `main()`:

```cpp
    RUN_TEST(test_serializeJson_includes_all_fields);
    RUN_TEST(test_serializeJson_omits_fat_mtime_when_zero);
    RUN_TEST(test_serializeJson_iso_utc_format);
    RUN_TEST(test_serializeJson_buffer_too_small_returns_zero);
```

Stub in `src/NtpSidecarWriter.cpp`:

```cpp
size_t NtpSidecarWriter::serializeJson(const SidecarPayload& /*p*/,
                                       char* /*buf*/, size_t /*cap*/) {
    return 0;
}
```

- [ ] **Step 2: Run tests to verify failure**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: 3 of the 4 new tests FAIL (the ones expecting non-zero output).

- [ ] **Step 3: Implement `serializeJson`**

Replace the stub:

```cpp
namespace {

// Format a tz-aware UTC ISO 8601 string into out (size must be >= 21).
// Output: "YYYY-MM-DDTHH:MM:SSZ"
void formatIsoUtcZ(time_t t, char out[21]) {
    struct tm tmv;
#ifdef UNIT_TEST
    gmtime_r(&t, &tmv);
#else
    gmtime_r(&t, &tmv);
#endif
    snprintf(out, 21, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

}  // namespace

size_t NtpSidecarWriter::serializeJson(const SidecarPayload& p,
                                       char* buf, size_t cap) {
    char ntpStr[21];
    formatIsoUtcZ(p.ntp_observed_at_unix, ntpStr);

    int written;
    if (p.fat_mtime_unix != 0) {
        char fatStr[21];
        formatIsoUtcZ(p.fat_mtime_unix, fatStr);
        written = snprintf(
            buf, cap,
            "{\"schema_version\":1,"
            "\"ntp_observed_at\":\"%s\","
            "\"fat_mtime\":\"%s\","
            "\"edf_header_start\":\"%s\","
            "\"edf_header_duration_seconds\":%d,"
            "\"uploader_poll_interval_seconds\":%d,"
            "\"uploader_firmware_version\":\"%s\"}",
            ntpStr, fatStr, p.header.startNaiveStr,
            p.header.durationSeconds, p.pollIntervalSeconds,
            p.firmwareVersion ? p.firmwareVersion : "");
    } else {
        written = snprintf(
            buf, cap,
            "{\"schema_version\":1,"
            "\"ntp_observed_at\":\"%s\","
            "\"edf_header_start\":\"%s\","
            "\"edf_header_duration_seconds\":%d,"
            "\"uploader_poll_interval_seconds\":%d,"
            "\"uploader_firmware_version\":\"%s\"}",
            ntpStr, p.header.startNaiveStr,
            p.header.durationSeconds, p.pollIntervalSeconds,
            p.firmwareVersion ? p.firmwareVersion : "");
    }

    if (written < 0) return 0;
    if (static_cast<size_t>(written) >= cap) return 0;  // truncated
    return static_cast<size_t>(written);
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `26 Tests 0 Failures 0 Ignored` PASSED.

- [ ] **Step 5: Commit**

```bash
git add include/NtpSidecarWriter.h \
        src/NtpSidecarWriter.cpp \
        test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): serializeJson builds compact sidecar JSON

snprintf-based formatter into a caller-provided buffer (target 512
bytes). Omits fat_mtime when 0. Returns 0 on truncation. Output
matches the schema in admin/sleep#176 and is bytewise-compatible
with Sleep.Ingest/cpap_clock.py read_sidecar().

Part of admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Extend MockFS with `getLastWrite` / `setLastWrite`

**Files:**
- Modify: `test/mocks/MockFS.h`
- Modify: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`

**Background:** Arduino's real `fs::File::getLastWrite()` returns a `time_t`. `MockFS` doesn't have it yet. We add it backward-compatibly: `MockFS::setLastWrite(path, t)` to set, `MockFile::getLastWrite()` to read. Default value is 0 (matches "FAT mtime unset" semantics).

- [ ] **Step 1: Write a failing test that uses `getLastWrite`**

Append to `test/test_ntp_sidecar/test_ntp_sidecar.cpp`:

```cpp
// ── MockFS extension sanity ────────────────────────────────────────────

void test_mockfs_getlastwrite_default_zero(void) {
    edfFS.clear();
    edfFS.addFile(String("/x.edf"), std::vector<uint8_t>(256, 0));
    MockFile f = edfFS.open(String("/x.edf"));
    TEST_ASSERT_EQUAL((time_t)0, f.getLastWrite());
}

void test_mockfs_setlastwrite_round_trips(void) {
    edfFS.clear();
    edfFS.addFile(String("/y.edf"), std::vector<uint8_t>(256, 0));
    edfFS.setLastWrite(String("/y.edf"), (time_t)1746603565);
    MockFile f = edfFS.open(String("/y.edf"));
    TEST_ASSERT_EQUAL((time_t)1746603565, f.getLastWrite());
}
```

Add to `main()`:

```cpp
    RUN_TEST(test_mockfs_getlastwrite_default_zero);
    RUN_TEST(test_mockfs_setlastwrite_round_trips);
```

- [ ] **Step 2: Run tests to verify they fail to compile**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: COMPILE FAIL — `MockFS::setLastWrite` and `MockFile::getLastWrite` not declared.

- [ ] **Step 3: Implement in `test/mocks/MockFS.h`**

Inside `class MockFS`, locate the `addFile` overloads (around line 189). Add a `std::map<std::string, time_t> mtimes;` private member, plus accessor methods. Also add `getLastWrite(path)` accessor.

Find the `private:` (or insert one if all members are public) section in `MockFS`, add:

```cpp
private:
    std::map<std::string, time_t> mtimes;
public:
    void setLastWrite(const String& path, time_t t) {
        mtimes[path.toStdString()] = t;
    }
    time_t getLastWrite(const String& path) const {
        auto it = mtimes.find(path.toStdString());
        return it == mtimes.end() ? (time_t)0 : it->second;
    }
```

(Place these next to the existing `addFile` overloads. Add `#include <map>` near the top of the file if not already present.)

In `class MockFile`, add a public method that reads through to the FS:

```cpp
    time_t getLastWrite() const {
        return fs ? fs->getLastWrite(path) : (time_t)0;
    }
```

The `clear()` method on `MockFS` should also clear `mtimes` — find it and add `mtimes.clear();`.

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `28 Tests 0 Failures 0 Ignored` PASSED. Also verify other test suites still build:

```bash
pio test -e native -f test_upload_state_manager
```

Expected: existing test suite still passes (this is the suite that uses MockFS most heavily).

- [ ] **Step 5: Commit**

```bash
git add test/mocks/MockFS.h test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
test(mocks): add getLastWrite/setLastWrite to MockFS/MockFile

Backwards-compatible additive change. Default mtime is 0 (matches
"FAT mtime unset" semantics from real Arduino fs::File). Needed
for NtpSidecarWriter::captureWitness to mock fat_mtime in tests.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `captureWitness` orchestrator

**Files:**
- Modify: `include/NtpSidecarWriter.h`
- Modify: `src/NtpSidecarWriter.cpp`
- Modify: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`

**Background:** `captureWitness` is the single entry point that produces a `SidecarPayload` from inputs. It must be testable without a real `Config` object, so we pass primitive args (poll interval seconds, firmware version string) instead of a `Config*`. The `FileUploader` call site looks them up itself.

Skip behavior:
- NTP unsynced → `{valid=false, skipReason=NTP_UNSYNCED}`
- Open file fails or header parse fails → `{valid=false, skipReason=EDF_PARSE_FAILED}`

- [ ] **Step 1: Declare + write failing tests**

Add to `include/NtpSidecarWriter.h` inside the class:

```cpp
    // Captures everything needed for a sidecar in one shot. Reads the EDF
    // header from disk, snapshots the current time, reads FAT mtime.
    // Returns a fully-populated SidecarPayload on success, or a
    // skipReason-tagged invalid payload if NTP is unsynced or the EDF
    // header is unparseable. Logs a warning via LOG()/LOGF() on skip paths.
#ifdef UNIT_TEST
    static SidecarPayload captureWitness(MockFS& fs, const String& localPath,
                                         int pollIntervalSeconds,
                                         const char* firmwareVersion);
#else
    static SidecarPayload captureWitness(fs::FS& fs, const String& localPath,
                                         int pollIntervalSeconds,
                                         const char* firmwareVersion);
#endif
```

Append tests:

```cpp
// ── captureWitness ──────────────────────────────────────────────────────

void test_captureWitness_skips_when_ntp_unsynced(void) {
    edfFS.clear();
    edfFS.addFile(String("/DATALOG/20260507/x_PLD.edf"),
                  makeEdfHeader("04.05.26", "22.29.52", "13650", "2"));
    MockTimeState::setTime(0);  // unsynced
    auto p = NtpSidecarWriter::captureWitness(
        edfFS, String("/DATALOG/20260507/x_PLD.edf"), 30, "test-fw");
    TEST_ASSERT_FALSE(p.valid);
    TEST_ASSERT_TRUE(p.skipReason == SkipReason::NTP_UNSYNCED);
}

void test_captureWitness_skips_when_header_parse_fails(void) {
    edfFS.clear();
    // 100 bytes — too short for an EDF header
    edfFS.addFile(String("/DATALOG/20260507/x_PLD.edf"),
                  std::vector<uint8_t>(100, 0));
    MockTimeState::setTime((time_t)1746603567);  // synced
    auto p = NtpSidecarWriter::captureWitness(
        edfFS, String("/DATALOG/20260507/x_PLD.edf"), 30, "test-fw");
    TEST_ASSERT_FALSE(p.valid);
    TEST_ASSERT_TRUE(p.skipReason == SkipReason::EDF_PARSE_FAILED);
}

void test_captureWitness_skips_when_file_missing(void) {
    edfFS.clear();
    MockTimeState::setTime((time_t)1746603567);
    auto p = NtpSidecarWriter::captureWitness(
        edfFS, String("/DATALOG/20260507/missing.edf"), 30, "test-fw");
    TEST_ASSERT_FALSE(p.valid);
    TEST_ASSERT_TRUE(p.skipReason == SkipReason::EDF_PARSE_FAILED);
}

void test_captureWitness_full_path(void) {
    edfFS.clear();
    edfFS.addFile(String("/DATALOG/20260507/x_PLD.edf"),
                  makeEdfHeader("04.05.26", "22.29.52", "13650", "2"));
    edfFS.setLastWrite(String("/DATALOG/20260507/x_PLD.edf"),
                       (time_t)1746603565);
    MockTimeState::setTime((time_t)1746603567);
    auto p = NtpSidecarWriter::captureWitness(
        edfFS, String("/DATALOG/20260507/x_PLD.edf"), 30, "test-fw");
    TEST_ASSERT_TRUE(p.valid);
    TEST_ASSERT_TRUE(p.skipReason == SkipReason::NONE);
    TEST_ASSERT_EQUAL((time_t)1746603567, p.ntp_observed_at_unix);
    TEST_ASSERT_EQUAL((time_t)1746603565, p.fat_mtime_unix);
    TEST_ASSERT_EQUAL_STRING("2026-05-04T22:29:52", p.header.startNaiveStr);
    TEST_ASSERT_EQUAL_INT(27300, p.header.durationSeconds);
    TEST_ASSERT_EQUAL_INT(30, p.pollIntervalSeconds);
    TEST_ASSERT_EQUAL_STRING("test-fw", p.firmwareVersion);
}
```

Add to `main()`:

```cpp
    RUN_TEST(test_captureWitness_skips_when_ntp_unsynced);
    RUN_TEST(test_captureWitness_skips_when_header_parse_fails);
    RUN_TEST(test_captureWitness_skips_when_file_missing);
    RUN_TEST(test_captureWitness_full_path);
```

Stub in `src/NtpSidecarWriter.cpp`:

```cpp
#ifdef UNIT_TEST
SidecarPayload NtpSidecarWriter::captureWitness(
    MockFS& /*fs*/, const String& /*localPath*/,
    int /*pollIntervalSeconds*/, const char* /*firmwareVersion*/)
#else
SidecarPayload NtpSidecarWriter::captureWitness(
    fs::FS& /*fs*/, const String& /*localPath*/,
    int /*pollIntervalSeconds*/, const char* /*firmwareVersion*/)
#endif
{
    SidecarPayload p{};
    p.valid = false;
    p.skipReason = SkipReason::EDF_PARSE_FAILED;
    return p;
}
```

- [ ] **Step 2: Run tests to verify failure**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: 1 of the 4 new tests FAILS (`test_captureWitness_full_path`); the others happen to pass against the stub.

- [ ] **Step 3: Implement `captureWitness`**

Replace the stub:

```cpp
#ifdef UNIT_TEST
SidecarPayload NtpSidecarWriter::captureWitness(
    MockFS& fs, const String& localPath,
    int pollIntervalSeconds, const char* firmwareVersion)
#else
SidecarPayload NtpSidecarWriter::captureWitness(
    fs::FS& fs, const String& localPath,
    int pollIntervalSeconds, const char* firmwareVersion)
#endif
{
    SidecarPayload p{};
    p.valid = false;
    p.pollIntervalSeconds = pollIntervalSeconds;
    p.firmwareVersion = firmwareVersion;

    time_t now = time(nullptr);
    if (!isNtpSynced(now)) {
        p.skipReason = SkipReason::NTP_UNSYNCED;
        return p;
    }
    p.ntp_observed_at_unix = now;

#ifdef UNIT_TEST
    MockFile f = fs.open(localPath);
#else
    fs::File f = fs.open(localPath);
#endif
    if (!f) {
        p.skipReason = SkipReason::EDF_PARSE_FAILED;
        return p;
    }

    // FAT mtime — captured before parse so we have it even if parse fails
    // (we don't actually return it on failure, but the read is cheap).
    p.fat_mtime_unix = f.getLastWrite();

    if (!parseEdfHeader(f, p.header)) {
        p.skipReason = SkipReason::EDF_PARSE_FAILED;
        return p;
    }

    p.valid = true;
    p.skipReason = SkipReason::NONE;
    return p;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `32 Tests 0 Failures 0 Ignored` PASSED.

- [ ] **Step 5: Commit**

```bash
git add include/NtpSidecarWriter.h \
        src/NtpSidecarWriter.cpp \
        test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): captureWitness orchestrator

Combines NTP-sync gate, file open, FAT mtime read, and EDF header
parse into a single SidecarPayload. Skip-reason-tagged failure
modes for NTP_UNSYNCED and EDF_PARSE_FAILED so the caller can log
the right diagnostic.

Part of admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: `write` (sink-abstracted SMB write)

**Files:**
- Modify: `include/NtpSidecarWriter.h`
- Modify: `src/NtpSidecarWriter.cpp`
- Modify: `test/test_ntp_sidecar/test_ntp_sidecar.cpp`

**Background:** `write` takes a sink callback (function pointer + context). The real call site wraps `SMBUploader::uploadRawBuffer`. Tests pass a recording stub. This avoids needing a `MockSMBUploader` class.

If the payload is invalid (`valid == false`), `write` returns `true` without calling the sink — "don't write a sidecar" is not an error. The caller decides whether to retry the EDF based on its own state. In the integration step we treat invalid-payload as a no-op success and only fail when the sink itself fails.

Wait — re-reading the spec: the asymmetry says invalid payloads (NTP-unsynced, parse-fail) → **mark uploaded, no retry**, while sink-write fail → **don't mark, retry**. So `write` returning `true` for invalid payloads is correct: the EDF should still be marked uploaded.

- [ ] **Step 1: Declare + write failing tests**

Add to `include/NtpSidecarWriter.h`:

```cpp
    // Sink callback signature. ctx is a caller-provided pointer (e.g.
    // SMBUploader*). Returns true on successful write.
    typedef bool (*SinkFn)(void* ctx, const String& remotePath,
                           const uint8_t* data, size_t len);

    // Writes the sidecar JSON to <edfRemotePath>.ntp.json via the sink.
    // - If payload is invalid, returns true without invoking the sink.
    // - If serialisation fails (buffer too small), returns false.
    // - Otherwise returns the sink's return value.
    static bool write(SinkFn sink, void* sinkCtx,
                      const String& edfRemotePath,
                      const SidecarPayload& payload);
```

Append tests:

```cpp
// ── write ──────────────────────────────────────────────────────────────

namespace {
struct SinkCall {
    String   path;
    std::vector<uint8_t> data;
    bool     called = false;
    bool     returnValue = true;
};

bool recordingSink(void* ctx, const String& remotePath,
                   const uint8_t* data, size_t len) {
    auto* s = static_cast<SinkCall*>(ctx);
    s->called = true;
    s->path = remotePath;
    s->data.assign(data, data + len);
    return s->returnValue;
}
}  // namespace

void test_write_invokes_sink_with_correct_path(void) {
    SinkCall call;
    auto p = makeSamplePayload();
    bool ok = NtpSidecarWriter::write(recordingSink, &call,
                                      String("/DATALOG/20260507/x_PLD.edf"),
                                      p);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(call.called);
    TEST_ASSERT_EQUAL_STRING("/DATALOG/20260507/x_PLD.edf.ntp.json",
                             call.path.c_str());
}

void test_write_payload_is_valid_json(void) {
    SinkCall call;
    auto p = makeSamplePayload();
    NtpSidecarWriter::write(recordingSink, &call,
                            String("/x.edf"), p);
    TEST_ASSERT_GREATER_THAN(0u, call.data.size());
    TEST_ASSERT_EQUAL_CHAR('{', (char)call.data.front());
    TEST_ASSERT_EQUAL_CHAR('}', (char)call.data.back());
}

void test_write_skips_sink_when_payload_invalid(void) {
    SinkCall call;
    SidecarPayload p{};
    p.valid = false;
    p.skipReason = SkipReason::NTP_UNSYNCED;
    bool ok = NtpSidecarWriter::write(recordingSink, &call,
                                      String("/x.edf"), p);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(call.called);
}

void test_write_returns_false_when_sink_fails(void) {
    SinkCall call;
    call.returnValue = false;
    auto p = makeSamplePayload();
    bool ok = NtpSidecarWriter::write(recordingSink, &call,
                                      String("/x.edf"), p);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(call.called);
}
```

Add to `main()`:

```cpp
    RUN_TEST(test_write_invokes_sink_with_correct_path);
    RUN_TEST(test_write_payload_is_valid_json);
    RUN_TEST(test_write_skips_sink_when_payload_invalid);
    RUN_TEST(test_write_returns_false_when_sink_fails);
```

Stub in `src/NtpSidecarWriter.cpp`:

```cpp
bool NtpSidecarWriter::write(SinkFn /*sink*/, void* /*sinkCtx*/,
                             const String& /*edfRemotePath*/,
                             const SidecarPayload& /*payload*/) {
    return false;
}
```

- [ ] **Step 2: Run tests to verify failure**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: 3 of the 4 new tests FAIL.

- [ ] **Step 3: Implement `write`**

Replace the stub:

```cpp
bool NtpSidecarWriter::write(SinkFn sink, void* sinkCtx,
                             const String& edfRemotePath,
                             const SidecarPayload& payload) {
    if (!payload.valid) {
        // No-op success — caller still marks the EDF uploaded.
        return true;
    }

    char buf[512];
    size_t n = serializeJson(payload, buf, sizeof(buf));
    if (n == 0) {
        return false;
    }

    String sidecarPath = edfRemotePath + ".ntp.json";
    return sink(sinkCtx, sidecarPath,
                reinterpret_cast<const uint8_t*>(buf), n);
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
source venv/bin/activate
pio test -e native -f test_ntp_sidecar
```

Expected: `36 Tests 0 Failures 0 Ignored` PASSED.

- [ ] **Step 5: Commit**

```bash
git add include/NtpSidecarWriter.h \
        src/NtpSidecarWriter.cpp \
        test/test_ntp_sidecar/test_ntp_sidecar.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): write via SinkFn callback abstraction

write() takes a function-pointer sink so unit tests can record
the call without linking SMBUploader.cpp. Invalid payloads
return true without invoking the sink (caller still marks the
EDF uploaded); only sink failure returns false (caller retries).

Part of admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Wire into `FileUploader::uploadSingleFileSmb`

**Files:**
- Modify: `src/FileUploader.cpp`
- Modify: `include/FileUploader.h` (only if a new include is needed)

- [ ] **Step 1: Inspect the current function**

Read `src/FileUploader.cpp:1053-1091` to confirm the structure has not drifted from what the spec assumed.

```bash
sed -n '1053,1095p' src/FileUploader.cpp
```

Expected: the function body matches the architecture pseudo-code in the spec (existing pre-flight, then `smbUploader->upload(...)`, then `markFileUploaded`).

- [ ] **Step 2: Add include + wire the calls**

Add the include near the existing includes at the top of `src/FileUploader.cpp`:

```cpp
#include "NtpSidecarWriter.h"
```

Define a sink adapter in an anonymous namespace at the top of the file (just after the includes):

```cpp
namespace {
bool smbSinkFn(void* ctx, const String& remotePath,
               const uint8_t* data, size_t len) {
    auto* up = static_cast<SMBUploader*>(ctx);
    return up->uploadRawBuffer(remotePath, data, len);
}
}  // namespace
```

Modify `uploadSingleFileSmb` (the function at line 1053). Insert the witness-capture and sidecar-write logic. Replace the existing body with:

```cpp
bool FileUploader::uploadSingleFileSmb(SDCardManager* sdManager, const String& filePath, bool force) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    if (!smbUploader || !smbStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    if (!sd.exists(filePath)) return true;  // file absent — not an error

    File f = sd.open(filePath);
    if (!f) { LOG_ERRORF("[FileUploader] [SMB] Cannot open: %s", filePath.c_str()); return false; }
    unsigned long fileSize = f.size();
    f.close();

    if (fileSize == 0) return true;

    if (!force && !smbStateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUGF("[FileUploader] [SMB] Unchanged, skipping: %s", filePath.c_str());
        return true;
    }

    LOGF("[FileUploader] Uploading single file: %s", filePath.c_str());

    // ── NTP sidecar: capture witness BEFORE the upload starts ──
    bool wantSidecar = NtpSidecarWriter::isDatalogEdf(filePath);
    SidecarPayload sidecar{};
    if (wantSidecar) {
        sidecar = NtpSidecarWriter::captureWitness(
            sd, filePath,
            config ? config->getInactivitySeconds() : 0,
            FIRMWARE_VERSION);
        if (!sidecar.valid) {
            switch (sidecar.skipReason) {
                case SkipReason::NTP_UNSYNCED:
                    LOG_WARNF("[NtpSidecar] Skipping %s — clock not NTP-synced",
                              filePath.c_str());
                    break;
                case SkipReason::EDF_PARSE_FAILED:
                    LOG_WARNF("[NtpSidecar] Skipping %s — EDF header unparseable",
                              filePath.c_str());
                    break;
                default:
                    break;
            }
        }
    }

    if (!smbUploader->isConnected() && !smbUploader->begin()) {
        LOG_ERROR("[FileUploader] [SMB] Connection failed");
        return false;
    }
    unsigned long smbBytes = 0;
    if (!smbUploader->upload(filePath, filePath, sd, smbBytes)) {
        LOG_ERRORF("[FileUploader] [SMB] Upload failed: %s", filePath.c_str());
        return false;
    }

    // ── NTP sidecar: write AFTER successful EDF upload ──
    if (wantSidecar) {
        if (!NtpSidecarWriter::write(smbSinkFn, smbUploader,
                                     filePath, sidecar)) {
            // Sink failed (sidecar payload was valid but SMB write erred).
            // Don't mark file uploaded — next pass retries the pair.
            LOG_ERRORF("[NtpSidecar] Failed to write sidecar for %s",
                       filePath.c_str());
            return false;
        }
    }

    String checksum = smbStateManager->calculateChecksum(sd, filePath);
    if (!checksum.isEmpty()) smbStateManager->markFileUploaded(filePath, checksum, fileSize);

    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), smbBytes);
    return true;
#endif
}
```

Verify there's a `LOG_WARNF` macro available (or the equivalent). Search:

```bash
grep -n "LOG_WARNF\|LOG_WARN\b" include/Logger.h src/Logger.cpp 2>/dev/null | head -5
```

If `LOG_WARNF` exists, use it. If not, substitute `LOGF` with a `[NtpSidecar] WARN:` prefix.

- [ ] **Step 3: Compile-only verification (firmware target)**

```bash
source venv/bin/activate
pio run -e pico32
```

Expected: build succeeds. The new helper adds <2KB to flash (no STL, no allocations).

If the build fails because `SidecarPayload` / `SkipReason` aren't visible in `FileUploader.cpp`, double-check the `#include "NtpSidecarWriter.h"` is above first use.

- [ ] **Step 4: Re-run all native tests to confirm nothing broke**

```bash
source venv/bin/activate
pio test -e native
```

Expected: all suites green. The new test_ntp_sidecar suite continues to pass; existing suites (test_upload_state_manager, test_oxyii_sync, etc.) still pass.

- [ ] **Step 5: Commit**

```bash
git add src/FileUploader.cpp
git commit -m "$(cat <<'EOF'
feat(ntp-sidecar): wire NtpSidecarWriter into uploadSingleFileSmb

Pre-copy witness capture for /DATALOG/<yyyymmdd>/*.edf, post-copy
sidecar write via smbSinkFn -> SMBUploader::uploadRawBuffer.
Sidecar-write failure returns false so the state manager does
NOT mark the EDF uploaded, ensuring the next pass retries the
EDF+sidecar as a unit.

Closes admin/sleep#176.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Full firmware build sanity check

**Files:** none (verification only)

- [ ] **Step 1: Build pico32**

```bash
source venv/bin/activate
pio run -e pico32
```

Expected: build succeeds. Take note of the `RAM:` and `Flash:` percentages reported on the last lines — compare to a prior build if needed.

- [ ] **Step 2: Build pico32-ota**

```bash
pio run -e pico32-ota
```

Expected: build succeeds. The OTA partition is tight (1.5MB per slot per CLAUDE.md); confirm we still fit. Sidecar code is small (<2KB), so it should.

- [ ] **Step 3: Run full native test suite**

```bash
pio test -e native
```

Expected: all suites pass. Total test count includes the 36 new ntp-sidecar tests + all prior counts unchanged.

No commit needed (verification step only).

---

## Task 10: Push branch and open PR

**Files:** none (git/gh operations)

- [ ] **Step 1: Push branch to fork**

```bash
git push -u origin feat/ntp-sidecar
```

- [ ] **Step 2: Open PR against the fork (NOT upstream)**

Per the project memory `feedback_cpap_uploader_pr_target_fork.md`: `gh pr create` defaults to upstream `amanuense/...`; pass `--repo nglessner/CPAP_data_uploader` explicitly.

```bash
gh pr create --repo nglessner/CPAP_data_uploader \
  --base main \
  --head feat/ntp-sidecar \
  --title "feat: write NTP sidecar manifest per copied EDF" \
  --body "$(cat <<'EOF'
## Summary

- Adds `NtpSidecarWriter` helper (header parse, JSON serialise, witness capture, sink-abstracted write).
- Wires it into `FileUploader::uploadSingleFileSmb` to write `<edf>.ntp.json` next to every `/DATALOG/yyyymmdd/*.edf` copied to SMB.
- 36 native unit tests; existing suites unchanged.

Part of [admin/sleep#174](http://192.168.0.108:3300/admin/sleep/issues/174) (closes [admin/sleep#176](http://192.168.0.108:3300/admin/sleep/issues/176)).

The sleep-side ingest already accepts these sidecars and populates `Sessions.ClockOffsetSec` from them ([admin/sleep#175](http://192.168.0.108:3300/admin/sleep/issues/175), [#177](http://192.168.0.108:3300/admin/sleep/issues/177)).

## Test plan

- [x] `pio test -e native -f test_ntp_sidecar` — 36 new tests
- [x] `pio test -e native` — all suites green
- [x] `pio run -e pico32` — builds clean
- [x] `pio run -e pico32-ota` — fits OTA partition
- [ ] Flash to F25 ESP32, run upload pass against AS11
- [ ] Verify `<edf>.ntp.json` appears next to each copied EDF on `/mnt/unraid/Misc`
- [ ] Confirm Sleep ingest populates `Sessions.ClockOffsetSec` for the night

Spec: `docs/superpowers/specs/2026-05-07-ntp-sidecar-design.md`

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Note the PR URL** for the integration test handoff in Task 11.

---

## Task 11: Hardware integration test (manual, post-merge or post-flash-of-branch)

**Files:** none (manual smoke test)

This task is intentionally not auto-checked by CI. Run it after merging the PR (or after flashing the branch directly, before merging).

- [ ] **Step 1: Flash to F25 ESP32**

Follow the project's standard flash procedure (`./build_upload.sh` or `pio run -e pico32 -t upload`). Per the project memory `feedback_serial_monitor_after_flash.md`, attach a serial monitor immediately after.

- [ ] **Step 2: Trigger an upload pass**

Either let the firmware run through its FSM naturally (ACQUIRING → UPLOADING) or hit the web UI's "Trigger Upload" button.

- [ ] **Step 3: Verify sidecars on the SMB share**

```bash
ls -la /mnt/unraid/Misc/DATALOG/$(date -d 'yesterday' +%Y%m%d)/*.ntp.json 2>/dev/null
```

Expected: one `.ntp.json` per `.edf` for the most recent session. Inspect one:

```bash
jq . /mnt/unraid/Misc/DATALOG/<yyyymmdd>/<file>_PLD.edf.ntp.json
```

Confirm:
- All required fields present (schema_version, ntp_observed_at, edf_header_start, edf_header_duration_seconds, uploader_poll_interval_seconds)
- Optional fields present (fat_mtime, uploader_firmware_version)
- `ntp_observed_at` is the actual upload time (within minutes of when the firmware ran)
- `edf_header_start` matches what the EDF actually says
- `uploader_firmware_version` matches the flashed firmware

- [ ] **Step 4: Trigger Sleep ingest re-run**

Per the existing self-heal hook, restart the Sleep API or wait for the next ingest cycle. Then query:

```bash
PGPASSWORD=$(grep -oP 'POSTGRES_PASSWORD=\K\S+' /opt/homelab/sleep/deploy/.env) \
  psql -h 127.0.0.1 -p 5433 -U sleep -d sleepdb -c \
  "SELECT \"Id\", \"SessionDate\", \"ClockOffsetSec\" \
     FROM \"Sessions\" \
     WHERE \"SessionDate\" >= CURRENT_DATE - INTERVAL '7 days' \
     ORDER BY \"SessionDate\" DESC LIMIT 10;"
```

Expected: the most recent sessions have `ClockOffsetSec` populated (around `-257` per the AS11's known +4-min skew).

- [ ] **Step 5: Mark `admin/sleep#176` closed**

The PR commit message includes `Closes admin/sleep#176`, but Gitea's autoclose works only for PRs cut on Gitea itself. Close manually via the Gitea web UI or:

```bash
curl -X PATCH \
  "http://admin:${GITEA_TOKEN}@192.168.0.108:3300/api/v1/repos/admin/sleep/issues/176" \
  -H "Content-Type: application/json" \
  -d '{"state":"closed"}'
```

Add a closing comment with the PR URL from Task 10 Step 3.

---

## Self-review

**Spec coverage:**

- ✅ DATALOG-EDF-only scope → Task 1 (`isDatalogEdf`)
- ✅ EDF header parse (offsets 168/176/236/244) → Task 2
- ✅ NTP-unsynced gate → Tasks 3 + 6
- ✅ JSON schema match (7 fields, fat_mtime omittable) → Task 4
- ✅ Witness capture orchestrator → Task 6
- ✅ Sink abstraction for testing → Task 7
- ✅ FileUploader wiring with retry semantics → Task 8
- ✅ Native test coverage for all branches → all tasks
- ✅ pico32 + pico32-ota build sanity → Task 9
- ✅ PR-target-fork preference honoured → Task 10
- ✅ Manual integration test → Task 11

**Placeholder scan:** none — every code step has full code; every command has full args.

**Type consistency:**
- `SidecarPayload`, `EdfHeader`, `SkipReason` defined in Task 2/4 and used unchanged in 6/7/8.
- `SinkFn` typedef in Task 7 used in Task 8 via `smbSinkFn`.
- `MockFile`/`MockFS` extension in Task 5 used in Task 6 (`fat_mtime_unix`).

No drift detected.
