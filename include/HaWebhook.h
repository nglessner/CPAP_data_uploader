#ifndef HA_WEBHOOK_H
#define HA_WEBHOOK_H

#include <stdint.h>

#ifdef UNIT_TEST
#include "Arduino.h"   // mock String — provided by test/mocks/Arduino.h
#else
#include <Arduino.h>
#endif

// Pluggable HTTP transport so unit tests don't need WiFi+HTTPClient.
// Production wires this to an HTTPClient-backed implementation; tests
// inject a capturing mock.
class IHttpSender {
public:
    virtual ~IHttpSender() = default;
    // Returns HTTP status code on completion, or a negative value on
    // connection/timeout failure. Implementations must not throw.
    virtual int post(const String& url, const String& body, int timeoutMs) = 0;
};

// Tiny fire-and-forget cue. fire() builds a compact JSON payload, calls
// the sender, logs the result, and returns true on 2xx. Empty url is
// "disabled" — fire() returns false without calling the sender.
class HaWebhook {
public:
    explicit HaWebhook(IHttpSender* sender) : _sender(sender) {}
    bool fire(const char* eventName,
              const String& deviceSegment,
              uint32_t epochSec,
              const String& url,
              int timeoutMs);
private:
    IHttpSender* _sender;
};

#ifndef UNIT_TEST
class HttpClientSender;
HttpClientSender& haHttpClientSender();
#endif

#endif // HA_WEBHOOK_H
