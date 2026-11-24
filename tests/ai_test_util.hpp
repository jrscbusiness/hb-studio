#pragma once
#include <chrono>
#include <deque>
#include <mutex>

#include "ai/ai_client.hpp"

namespace hb::ai::testutil {

// Scripted transport: pops one response per send(); records requests. Streaming: if the request has
// onData and the response is 2xx, the body is delivered in `chunkSize` pieces.
class FakeHttp : public IAiHttp {
public:
    std::deque<HttpResponse> script;
    std::vector<HttpRequest> requests;
    std::size_t chunkSize = 7;
    std::mutex m;

    HttpResponse send(const HttpRequest& r) override {
        std::lock_guard lock(m);
        requests.push_back(r);
        HttpResponse resp;
        if (!script.empty()) { resp = script.front(); script.pop_front(); }
        else resp.transportError = "script exhausted";
        if (r.onData && resp.status >= 200 && resp.status < 300 && resp.transportError.empty()) {
            const std::string body = resp.body;
            resp.body.clear();
            for (std::size_t i = 0; i < body.size(); i += chunkSize)
                if (!r.onData(body.substr(i, chunkSize))) break;
        }
        return resp;
    }
};

inline HttpResponse ok(const std::string& body, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = body;
    return r;
}
inline HttpResponse transportFail(bool timeout = false) {
    HttpResponse r;
    r.transportError = "boom";
    r.timedOut = timeout;
    return r;
}

struct FakeClock {
    std::chrono::steady_clock::time_point t{};
    std::vector<std::chrono::milliseconds> sleeps;
    AiClientDeps deps(std::shared_ptr<IAiHttp> http) {
        AiClientDeps d;
        d.http = std::move(http);
        d.now = [this] { return t; };
        d.sleep = [this](std::chrono::milliseconds ms) { sleeps.push_back(ms); t += ms; };
        return d;
    }
};

inline std::string chatJson(const std::string& text) {
    return R"({"choices":[{"message":{"role":"assistant","content":")" + text + R"("}}]})";
}

}  // namespace hb::ai::testutil
