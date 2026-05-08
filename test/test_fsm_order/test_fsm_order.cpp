// Asserts the documented FSM state surface. The FSM lives in main.cpp
// and depends on globals (sdManager, config, uploader). Rather than
// wire all that in for a host test, we just check the enum values and
// their string names exist as expected. This catches anyone silently
// renaming or removing a state.

#include <unity.h>
#include "UploadFSM.h"

void setUp(void) {}
void tearDown(void) {}

static void test_fsm_state_names_present(void) {
    TEST_ASSERT_EQUAL_STRING("IDLE",        getStateName(UploadState::IDLE));
    TEST_ASSERT_EQUAL_STRING("LISTENING",   getStateName(UploadState::LISTENING));
    TEST_ASSERT_EQUAL_STRING("ACQUIRING",   getStateName(UploadState::ACQUIRING));
    TEST_ASSERT_EQUAL_STRING("UPLOADING",   getStateName(UploadState::UPLOADING));
    TEST_ASSERT_EQUAL_STRING("RELEASING",   getStateName(UploadState::RELEASING));
    TEST_ASSERT_EQUAL_STRING("COOLDOWN",    getStateName(UploadState::COOLDOWN));
    TEST_ASSERT_EQUAL_STRING("COMPLETE",    getStateName(UploadState::COMPLETE));
    TEST_ASSERT_EQUAL_STRING("MONITORING",  getStateName(UploadState::MONITORING));
    TEST_ASSERT_EQUAL_STRING("O2RING_SYNC",  getStateName(UploadState::O2RING_SYNC));
    TEST_ASSERT_EQUAL_STRING("O2RING_RETRY", getStateName(UploadState::O2RING_RETRY));
}

static void test_fsm_unknown_returns_unknown(void) {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", getStateName((UploadState)99));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fsm_state_names_present);
    RUN_TEST(test_fsm_unknown_returns_unknown);
    return UNITY_END();
}
