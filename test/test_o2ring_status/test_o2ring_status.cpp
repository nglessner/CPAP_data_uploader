#include <unity.h>
#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H
#include "../mocks/MockPreferences.h"
#include "../mocks/MockTime.h"

#include "O2RingStatus.h"
#include "../../src/O2RingStatus.cpp"

void setUp(void) {
    Preferences::clearAll();
    MockTimeState::reset();
}
void tearDown(void) { Preferences::clearAll(); }

void test_default_record_has_no_data() {
    O2RingStatus s;
    s.load();
    TEST_ASSERT_FALSE(s.hasData());
    TEST_ASSERT_EQUAL_UINT32(0, s.getLastUnix());
    TEST_ASSERT_EQUAL_INT(-1, s.getLastResult());
    TEST_ASSERT_EQUAL_UINT(0, s.getFilesSynced());
    TEST_ASSERT_EQUAL_STRING("", s.getLastFilename().c_str());
}

void test_record_ok_roundtrip() {
    MockTimeState::setTime(1777035170);
    {
        O2RingStatus s;
        s.load();
        s.record(0 /*OK*/, 3, "20260124013312.vld");
        s.save();
    }
    {
        O2RingStatus reloaded;
        reloaded.load();
        TEST_ASSERT_TRUE(reloaded.hasData());
        TEST_ASSERT_EQUAL_UINT32(1777035170, reloaded.getLastUnix());
        TEST_ASSERT_EQUAL_INT(0, reloaded.getLastResult());
        TEST_ASSERT_EQUAL_UINT(3, reloaded.getFilesSynced());
        TEST_ASSERT_EQUAL_STRING("20260124013312.vld",
                                 reloaded.getLastFilename().c_str());
    }
}

void test_record_failure_preserves_previous_filename() {
    // First run: successful INFO observation of a file
    MockTimeState::setTime(1777000000);
    {
        O2RingStatus s;
        s.load();
        s.record(4 /*NOTHING_TO_SYNC*/, 0, "20260123223312.vld");
        s.save();
    }
    // Second run: DEVICE_NOT_FOUND before INFO, no filename passed
    MockTimeState::setTime(1777035170);
    {
        O2RingStatus s;
        s.load();
        s.recordPreservingFilename(1 /*DEVICE_NOT_FOUND*/);
        s.save();
    }
    O2RingStatus reloaded;
    reloaded.load();
    TEST_ASSERT_EQUAL_UINT32(1777035170, reloaded.getLastUnix());
    TEST_ASSERT_EQUAL_INT(1, reloaded.getLastResult());
    TEST_ASSERT_EQUAL_UINT(0, reloaded.getFilesSynced());
    TEST_ASSERT_EQUAL_STRING("20260123223312.vld",
                             reloaded.getLastFilename().c_str());
}

void test_empty_filename_accepted() {
    MockTimeState::setTime(1777035170);
    O2RingStatus s;
    s.load();
    s.record(1 /*DEVICE_NOT_FOUND*/, 0, "");
    s.save();

    O2RingStatus reloaded;
    reloaded.load();
    TEST_ASSERT_TRUE(reloaded.hasData());
    TEST_ASSERT_EQUAL_STRING("", reloaded.getLastFilename().c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_default_record_has_no_data);
    RUN_TEST(test_record_ok_roundtrip);
    RUN_TEST(test_record_failure_preserves_previous_filename);
    RUN_TEST(test_empty_filename_accepted);
    return UNITY_END();
}
