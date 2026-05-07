#include <unity.h>
#include "Arduino.h"
#include "MockTime.h"
#include "MockFS.h"
#include "MockLogger.h"

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
    return UNITY_END();
}
