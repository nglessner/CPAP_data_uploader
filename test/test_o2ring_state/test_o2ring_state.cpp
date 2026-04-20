#include <unity.h>
#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H
#include "../mocks/MockPreferences.h"

#include "O2RingState.h"
#include "../../src/O2RingState.cpp"

void setUp(void) { Preferences::clearAll(); }
void tearDown(void) { Preferences::clearAll(); }

void test_new_file_not_seen() {
    O2RingState state;
    state.load();
    TEST_ASSERT_FALSE(state.hasSeen("20260116233312.vld"));
}

void test_mark_seen_persists() {
    {
        O2RingState state;
        state.load();
        state.markSeen("20260116233312.vld");
        state.save();
    }
    {
        O2RingState state;
        state.load();
        TEST_ASSERT_TRUE(state.hasSeen("20260116233312.vld"));
    }
}

void test_multiple_files_tracked() {
    O2RingState state;
    state.load();
    state.markSeen("20260116233312.vld");
    state.markSeen("20260115221045.vld");
    state.save();

    O2RingState state2;
    state2.load();
    TEST_ASSERT_TRUE(state2.hasSeen("20260116233312.vld"));
    TEST_ASSERT_TRUE(state2.hasSeen("20260115221045.vld"));
    TEST_ASSERT_FALSE(state2.hasSeen("20260114101010.vld"));
}

void test_mark_seen_idempotent() {
    O2RingState state;
    state.load();
    state.markSeen("20260116233312.vld");
    state.markSeen("20260116233312.vld");  // duplicate
    state.save();

    O2RingState state2;
    state2.load();
    TEST_ASSERT_TRUE(state2.hasSeen("20260116233312.vld"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_new_file_not_seen);
    RUN_TEST(test_mark_seen_persists);
    RUN_TEST(test_multiple_files_tracked);
    RUN_TEST(test_mark_seen_idempotent);
    return UNITY_END();
}
