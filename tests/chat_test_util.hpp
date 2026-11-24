#pragma once
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "chat/net.hpp"
#include "chat/oauth.hpp"

namespace hb::chat::test {

// Scripted HTTP transport: responses are consumed in order; every request is recorded.
class FakeHttp : public IChatHttp {
public:
    HttpResponse send(const HttpRequest& r) override {
        std::lock_guard lock(m);
        requests.push_back(r);
        if (!script.empty()) {
            HttpResponse x = script.front();
            script.pop_front();
            return x;
        }
        HttpResponse e;
        e.error = "script exhausted";
        return e;
    }
    void ok(int status, const std::string& body) {
        HttpResponse r;
        r.transportOk = true;
        r.status = status;
        r.body = body;
        script.push_back(r);
    }
    void netFail(const std::string& why) {
        HttpResponse r;
        r.error = why;
        script.push_back(r);
    }
    std::mutex m;
    std::deque<HttpResponse> script;
    std::vector<HttpRequest> requests;
};

class FakeSocket : public IChatSocket {
public:
    bool connect(const std::string& url, std::string& error) override {
        connectedUrl = url;
        ++connects;
        if (failConnect) { error = "refused"; return false; }
        open = true;
        return true;
    }
    bool sendText(const std::string& t) override { sent.push_back(t); return open; }
    Recv receive(std::string& out, int) override {
        if (!inbox.empty()) { out = inbox.front(); inbox.pop_front(); return Recv::Message; }
        if (closeWhenEmpty) { open = false; return Recv::Closed; }
        return Recv::Timeout;
    }
    void close() override { open = false; ++closes; }
    bool open = false, failConnect = false, closeWhenEmpty = false;
    int connects = 0, closes = 0;
    std::string connectedUrl;
    std::vector<std::string> sent;
    std::deque<std::string> inbox;
};

class FakeTokens : public IAccessTokenProvider {
public:
    bool accessToken(TimePoint, std::string& token, std::string& error, bool& needsAuth) override {
        needsAuth = authNeeded;
        if (fail) { error = "no token"; return false; }
        token = "tok";
        return true;
    }
    void invalidate() override { ++invalidations; }
    bool fail = false, authNeeded = false;
    int invalidations = 0;
};

inline TimePoint t0() { return TimePoint{} + std::chrono::hours(1); }
inline TimePoint at(int ms) { return t0() + Millis(ms); }

}  // namespace hb::chat::test
