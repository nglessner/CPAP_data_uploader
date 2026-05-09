// Host-side test for HaWebhook. Production code calls Arduino's
// HTTPClient, which we do not have on native. The test focuses on:
//   - JSON payload shape
//   - empty URL == disabled (no-op, returns false but does not crash)
//   - non-2xx status is logged, not fatal
// HaWebhook takes an IHttpSender interface so tests can substitute a
// capturing mock.

#include <unity.h>

#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H

#include "HaWebhook.h"
#include "../../src/HaWebhook.cpp"

void setUp(void) {}
void tearDown(void) {}

namespace {
struct CapturingSender : public IHttpSender {
    String lastUrl;
    String lastBody;
    int    timeoutMsSeen = 0;
    int    returnStatus  = 200;

    int post(const String& url, const String& body, int timeoutMs) override {
        lastUrl = url;
        lastBody = body;
        timeoutMsSeen = timeoutMs;
        return returnStatus;
    }
};
}

static void test_empty_url_is_disabled(void) {
    CapturingSender s;
    HaWebhook hook(&s);
    bool fired = hook.fire("ring_sync_miss", "abc123", 1715000000UL,
                           /*url=*/"", /*timeoutMs=*/3000);
    TEST_ASSERT_FALSE(fired);
    TEST_ASSERT_EQUAL_STRING("", s.lastUrl.c_str());  // sender not called
}

static void test_payload_shape(void) {
    CapturingSender s;
    HaWebhook hook(&s);
    hook.fire("ring_sync_miss", "abc123", 1715000000UL,
              "http://ha.local/api/webhook/x", 3000);
    TEST_ASSERT_EQUAL_STRING("http://ha.local/api/webhook/x", s.lastUrl.c_str());
    TEST_ASSERT_EQUAL(3000, s.timeoutMsSeen);
    // Compact JSON, key order matches our writer
    TEST_ASSERT_EQUAL_STRING(
        "{\"event\":\"ring_sync_miss\",\"device\":\"abc123\",\"ts\":1715000000}",
        s.lastBody.c_str());
}

static void test_non_2xx_is_not_fatal(void) {
    CapturingSender s;
    s.returnStatus = 500;
    HaWebhook hook(&s);
    bool fired = hook.fire("ring_sync_miss", "abc", 0UL,
                           "http://ha.local/api/webhook/x", 3000);
    TEST_ASSERT_FALSE(fired);   // not a success
    // No crash, sender was called
    TEST_ASSERT_EQUAL_STRING("http://ha.local/api/webhook/x", s.lastUrl.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_url_is_disabled);
    RUN_TEST(test_payload_shape);
    RUN_TEST(test_non_2xx_is_not_fatal);
    return UNITY_END();
}
