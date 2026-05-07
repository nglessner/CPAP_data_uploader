#include <unity.h>
#include "Arduino.h"
#include "MockTime.h"
#include "MockFS.h"
#include "MockLogger.h"
#include <vector>
#include <cstring>

// Include mock implementations
#include "../mocks/Arduino.cpp"

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

void test_isDatalogEdf_matches_session_sad(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/20260507_223015_SAD.edf")));
}

void test_isDatalogEdf_matches_session_sa2(void) {
    TEST_ASSERT_TRUE(NtpSidecarWriter::isDatalogEdf(
        String("/DATALOG/20260507/20260507_223015_SA2.edf")));
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
        size_t slen = strlen(s);
        size_t n = (slen < fieldLen) ? slen : fieldLen;
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

// ── serializeJson ───────────────────────────────────────────────────────

namespace {
SidecarPayload makeSamplePayload(time_t fatMtime = 1778117965) {
    SidecarPayload p{};
    p.valid = true;
    p.skipReason = SkipReason::NONE;
    p.ntp_observed_at_unix = 1778117967;  // 2026-05-07T01:39:27Z
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
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ntp_observed_at\":\"2026-05-07T01:39:27Z\""));
}

void test_serializeJson_iso_utc_format(void) {
    char buf[512] = {0};
    auto p = makeSamplePayload();
    NtpSidecarWriter::serializeJson(p, buf, sizeof(buf));
    const char* ntp = strstr(buf, "\"ntp_observed_at\":\"");
    TEST_ASSERT_NOT_NULL(ntp);
    TEST_ASSERT_EQUAL_CHAR('Z', ntp[strlen("\"ntp_observed_at\":\"") + 19]);
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

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_isDatalogEdf_matches_session_pld);
    RUN_TEST(test_isDatalogEdf_matches_session_brp);
    RUN_TEST(test_isDatalogEdf_matches_session_eve);
    RUN_TEST(test_isDatalogEdf_matches_session_csl);
    RUN_TEST(test_isDatalogEdf_matches_session_sad);
    RUN_TEST(test_isDatalogEdf_matches_session_sa2);
    RUN_TEST(test_isDatalogEdf_case_insensitive_extension);
    RUN_TEST(test_isDatalogEdf_rejects_root_str);
    RUN_TEST(test_isDatalogEdf_rejects_state_files);
    RUN_TEST(test_isDatalogEdf_rejects_non_8digit_folder);
    RUN_TEST(test_isDatalogEdf_rejects_extra_path_segments);
    RUN_TEST(test_isDatalogEdf_rejects_non_edf_extension);
    RUN_TEST(test_isDatalogEdf_rejects_empty_filename);
    RUN_TEST(test_parseEdfHeader_valid_full_pld);
    RUN_TEST(test_parseEdfHeader_short_read);
    RUN_TEST(test_parseEdfHeader_bad_date_format);
    RUN_TEST(test_parseEdfHeader_bad_time_format);
    RUN_TEST(test_parseEdfHeader_fractional_record_duration);
    RUN_TEST(test_parseEdfHeader_zero_records);
    RUN_TEST(test_parseEdfHeader_negative_records_treated_as_invalid);
    RUN_TEST(test_isNtpSynced_zero_is_unsynced);
    RUN_TEST(test_isNtpSynced_pre_2024_is_unsynced);
    RUN_TEST(test_isNtpSynced_2024_boundary_is_synced);
    RUN_TEST(test_isNtpSynced_modern_time_is_synced);
    RUN_TEST(test_serializeJson_includes_all_fields);
    RUN_TEST(test_serializeJson_omits_fat_mtime_when_zero);
    RUN_TEST(test_serializeJson_iso_utc_format);
    RUN_TEST(test_serializeJson_buffer_too_small_returns_zero);
    RUN_TEST(test_mockfs_getlastwrite_default_zero);
    RUN_TEST(test_mockfs_setlastwrite_round_trips);
    RUN_TEST(test_captureWitness_skips_when_ntp_unsynced);
    RUN_TEST(test_captureWitness_skips_when_header_parse_fails);
    RUN_TEST(test_captureWitness_skips_when_file_missing);
    RUN_TEST(test_captureWitness_full_path);
    RUN_TEST(test_write_invokes_sink_with_correct_path);
    RUN_TEST(test_write_payload_is_valid_json);
    RUN_TEST(test_write_skips_sink_when_payload_invalid);
    RUN_TEST(test_write_returns_false_when_sink_fails);
    return UNITY_END();
}
