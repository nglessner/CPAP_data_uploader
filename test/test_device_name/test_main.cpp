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
    String input;
    for (int i = 0; i < 40; ++i) input += String("a");
    String out = Config::sanitizeDeviceSegment(input);
    TEST_ASSERT_EQUAL_size_t(32, out.length());
    for (size_t i = 0; i < out.length(); ++i) {
        TEST_ASSERT_EQUAL_CHAR('a', out.charAt(i));
    }
}

void test_sanitize_trailing_hyphen_after_cap_is_trimmed() {
    // 31 a's then a dash — after 32-char cap the dash should be trimmed
    String input;
    for (int i = 0; i < 31; ++i) input += String("a");
    input += "-extra";  // cap will cut at "aaaa...aaaa-" (32nd char is '-')
    String out = Config::sanitizeDeviceSegment(input);
    TEST_ASSERT_EQUAL_size_t(31, out.length());
    TEST_ASSERT_NOT_EQUAL('-', out.charAt(out.length() - 1));
}

// ---------- resolveDeviceSegment ----------

void test_resolve_name_unset_returns_sanitized_mac() {
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
    RUN_TEST(test_resolve_name_unset_returns_sanitized_mac);
    RUN_TEST(test_resolve_name_whitespace_only_falls_back_to_mac);
    RUN_TEST(test_resolve_name_valid_returns_sanitized_name);
    RUN_TEST(test_resolve_name_with_spaces_returns_sanitized_name);
    RUN_TEST(test_resolve_mac_lowercased_and_stripped);
    return UNITY_END();
}
