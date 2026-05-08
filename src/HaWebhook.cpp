#include "HaWebhook.h"
#ifndef UNIT_TEST
#include <HTTPClient.h>
#include <WiFi.h>
#endif
#include "Logger.h"

bool HaWebhook::fire(const char* eventName,
                     const String& deviceSegment,
                     uint32_t epochSec,
                     const String& url,
                     int timeoutMs) {
    if (url.isEmpty()) {
        LOG_DEBUG("[HaWebhook] url empty — cue disabled");
        return false;
    }
    if (!_sender) {
        LOG_WARN("[HaWebhook] no sender wired");
        return false;
    }
    String body;
    body  = "{\"event\":\"";
    body += eventName;
    body += "\",\"device\":\"";
    body += deviceSegment;
    body += "\",\"ts\":";
    body += String((unsigned long)epochSec);
    body += "}";

    int status = _sender->post(url, body, timeoutMs);
    if (status >= 200 && status < 300) {
        LOGF("[HaWebhook] %s -> %d", eventName, status);
        return true;
    }
    LOGF("[HaWebhook] %s -> %d (non-2xx or transport error)", eventName, status);
    return false;
}

#ifndef UNIT_TEST
class HttpClientSender : public IHttpSender {
public:
    int post(const String& url, const String& body, int timeoutMs) override {
        if (WiFi.status() != WL_CONNECTED) return -1;
        HTTPClient http;
        if (!http.begin(url)) return -2;
        http.setTimeout(timeoutMs);
        http.addHeader("Content-Type", "application/json");
        int status = http.POST(body);
        http.end();
        return status;
    }
};

IHttpSender& haHttpClientSender() {
    static HttpClientSender s;
    return s;
}
#endif
