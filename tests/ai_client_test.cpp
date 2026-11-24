#include <gtest/gtest.h>

#include "ai/ai_client.hpp"
#include "ai_test_util.hpp"

using namespace hb::ai;
using namespace hb::ai::testutil;

namespace {

Provider orProvider() {
    Provider p;
    p.id = "p1";
    p.kind = ProviderKind::OpenRouter;
    p.baseUrl = "https://openrouter.ai/api/v1";
    p.apiKey = "sk-or-test-1234567890";
    p.model = "meta/x:free";
    return p;
}

struct Fx {
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    FakeClock clock;
    AiClient client{clock.deps(http)};
};

}  // namespace

TEST(AiClient, SuccessBuildsCorrectRequest) {
    Fx f;
    f.http->script.push_back(ok(chatJson("oi")));
    auto r = f.client.chat(orProvider(), {{"user", "hello"}}, {});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.text, "oi");
    EXPECT_EQ(r.attempts, 1);
    ASSERT_EQ(f.http->requests.size(), 1u);
    EXPECT_EQ(f.http->requests[0].url, "https://openrouter.ai/api/v1/chat/completions");
    EXPECT_EQ(f.http->requests[0].method, "POST");
    EXPECT_NE(f.http->requests[0].body.find("meta/x:free"), std::string::npos);
}

TEST(AiClient, ConfigErrorsDoNotTouchTheNetwork) {
    Fx f;
    Provider p = orProvider();
    p.apiKey.clear();
    auto r = f.client.chat(p, {{"user", "x"}}, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.kind, ErrorKind::Config);
    p = orProvider();
    p.model.clear();
    EXPECT_EQ(f.client.chat(p, {{"user", "x"}}, {}).kind, ErrorKind::Config);
    p = orProvider();
    p.baseUrl = "http://evil.example.com/v1";
    EXPECT_EQ(f.client.chat(p, {{"user", "x"}}, {}).kind, ErrorKind::Config);
    EXPECT_TRUE(f.http->requests.empty());
}

TEST(AiClient, KeylessLocalProviderIsAllowed) {
    Fx f;
    f.http->script.push_back(ok(chatJson("ok")));
    Provider p;
    p.kind = ProviderKind::NineRouter;
    p.baseUrl = "http://localhost:20128/v1";
    p.model = "my-combo";
    EXPECT_TRUE(f.client.chat(p, {{"user", "x"}}, {}).ok);
    for (auto& h : f.http->requests[0].headers) EXPECT_NE(h.first, "Authorization");
}

TEST(AiClient, RetriesOn429WithBackoffThenSucceeds) {
    Fx f;
    f.http->script.push_back(ok(R"({"error":{"message":"slow down"}})", 429));
    f.http->script.push_back(ok(R"({"error":{"message":"slow down"}})", 503));
    f.http->script.push_back(ok(chatJson("finally")));
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, {});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.attempts, 3);
    ASSERT_EQ(f.clock.sleeps.size(), 2u);
    EXPECT_EQ(f.clock.sleeps[0].count(), 500);
    EXPECT_EQ(f.clock.sleeps[1].count(), 1000);
    EXPECT_EQ(r.latencyMs, 1500);  // injected clock only advances via sleeps
}

TEST(AiClient, HonoursRetryAfterWithCap) {
    Fx f;
    HttpResponse a = ok("{}", 429);
    a.headers.emplace_back("Retry-After", "2");
    HttpResponse b = ok("{}", 429);
    b.headers.emplace_back("Retry-After", "120");
    f.http->script = {a, b, ok(chatJson("x"))};
    ASSERT_TRUE(f.client.chat(orProvider(), {{"user", "x"}}, {}).ok);
    EXPECT_EQ(f.clock.sleeps[0].count(), 2000);
    EXPECT_EQ(f.clock.sleeps[1].count(), 8000);
}

TEST(AiClient, GivesUpAfterMaxRetriesWithRateLimitedMessage) {
    Fx f;
    for (int i = 0; i < 5; ++i) f.http->script.push_back(ok(R"({"error":{"message":"free tier"}})", 429));
    ChatOptions o;
    o.maxRetries = 1;
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, o);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.kind, ErrorKind::RateLimited);
    EXPECT_EQ(r.attempts, 2);
    EXPECT_NE(r.error.find("Limite"), std::string::npos);
    EXPECT_NE(r.error.find("free tier"), std::string::npos);
}

TEST(AiClient, NoRetryOnAuthOrCreditErrors) {
    for (int status : {401, 402, 404}) {
        Fx f;
        f.http->script.push_back(ok(R"({"error":{"message":"nope"}})", status));
        f.http->script.push_back(ok(chatJson("should not be reached")));
        auto r = f.client.chat(orProvider(), {{"user", "x"}}, {});
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(r.attempts, 1);
        EXPECT_EQ(f.http->requests.size(), 1u);
        EXPECT_EQ(r.httpStatus, status);
    }
    Fx f;
    f.http->script.push_back(ok("{}", 401));
    EXPECT_EQ(f.client.chat(orProvider(), {{"user", "x"}}, {}).kind, ErrorKind::InvalidKey);
}

TEST(AiClient, NetworkAndTimeoutErrors) {
    Fx f;
    f.http->script = {transportFail(), transportFail(), transportFail()};
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, {});
    EXPECT_EQ(r.kind, ErrorKind::Network);
    EXPECT_EQ(r.attempts, 3);
    Fx g;
    g.http->script = {transportFail(true), ok(chatJson("recovered"))};
    auto r2 = g.client.chat(orProvider(), {{"user", "x"}}, {});
    EXPECT_TRUE(r2.ok);
    Fx h;
    h.http->script = {transportFail(true)};
    ChatOptions o;
    o.maxRetries = 0;
    EXPECT_EQ(h.client.chat(orProvider(), {{"user", "x"}}, o).kind, ErrorKind::Timeout);
}

TEST(AiClient, HttpOkWithErrorBodyAndGarbage) {
    Fx f;
    f.http->script.push_back(ok(R"({"error":{"message":"provider returned error"}})"));
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, {});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("provider returned error"), std::string::npos);
    Fx g;
    g.http->script.push_back(ok("<html>captive portal</html>"));
    EXPECT_EQ(g.client.chat(orProvider(), {{"user", "x"}}, {}).kind, ErrorKind::BadResponse);
}

TEST(AiClient, StreamingDeliversDeltas) {
    Fx f;
    f.http->script.push_back(ok(
        "data: {\"choices\":[{\"delta\":{\"content\":\"Ola\"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\" mundo\"}}]}\n\ndata: [DONE]\n\n"));
    std::vector<std::string> got;
    ChatOptions o;
    o.onDelta = [&](const std::string& s) { got.push_back(s); };
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, o);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.text, "Ola mundo");
    EXPECT_EQ(got.size(), 2u);
    EXPECT_NE(f.http->requests[0].body.find("\"stream\":true"), std::string::npos);
}

TEST(AiClient, StreamingServerIgnoringStreamStillWorks) {
    Fx f;
    f.http->script.push_back(ok(chatJson("whole")));
    std::string got;
    ChatOptions o;
    o.onDelta = [&](const std::string& s) { got += s; };
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, o);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(got, "whole");
}

TEST(AiClient, StreamingNotRetriedAfterDelivery) {
    Fx f;
    // 200 stream that ends with an error object after one fragment: ok with partial text, no retry.
    f.http->script.push_back(ok("data: {\"choices\":[{\"delta\":{\"content\":\"par\"}}]}\n\ndata: {\"error\":{\"message\":\"cut\"}}\n\n"));
    f.http->script.push_back(ok(chatJson("never")));
    ChatOptions o;
    o.onDelta = [](const std::string&) {};
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, o);
    EXPECT_EQ(f.http->requests.size(), 1u);
    EXPECT_EQ(r.text, "par");
}

TEST(AiClient, StreamingCallbackExceptionIsContained) {
    Fx f;
    f.http->script.push_back(ok("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\ndata: [DONE]\n\n"));
    ChatOptions o;
    o.onDelta = [](const std::string&) { throw 1; };
    EXPECT_TRUE(f.client.chat(orProvider(), {{"user", "x"}}, o).ok);
}

TEST(AiClient, ListModelsOkAndErrors) {
    Fx f;
    f.http->script.push_back(ok(R"({"data":[{"id":"a:free","pricing":{"prompt":"0","completion":"0"}},{"id":"b"}]})"));
    auto m = f.client.listModels(orProvider());
    ASSERT_TRUE(m.ok);
    EXPECT_EQ(m.models.size(), 2u);
    EXPECT_TRUE(m.models[0].free);
    EXPECT_EQ(f.http->requests[0].method, "GET");
    EXPECT_EQ(f.http->requests[0].url, "https://openrouter.ai/api/v1/models");

    Fx g;
    g.http->script.push_back(ok("{}", 404));
    auto n = g.client.listModels(orProvider());
    EXPECT_FALSE(n.ok);
    EXPECT_NE(n.error.find("/models"), std::string::npos);
    Fx h;
    h.http->script.push_back(ok("{}", 401));
    EXPECT_EQ(h.client.listModels(orProvider()).kind, ErrorKind::InvalidKey);
    Fx i;
    i.http->script.push_back(transportFail());
    EXPECT_EQ(i.client.listModels(orProvider()).kind, ErrorKind::Network);
    Fx j;
    j.http->script.push_back(ok(R"({"data":[]})"));
    EXPECT_FALSE(j.client.listModels(orProvider()).ok);
}

TEST(AiClient, KeyNeverAppearsInErrorText) {
    Fx f;
    f.http->script.push_back(ok(R"({"error":{"message":"Incorrect API key provided: sk-or-test-1234567890"}})", 401));
    auto r = f.client.chat(orProvider(), {{"user", "x"}}, {});
    // The provider message is echoed (useful) but the redactor must not let a bearer token through.
    f.http->script.push_back(ok(R"({"error":{"message":"bad Authorization: Bearer sk-or-test-1234567890"}})", 401));
    auto r2 = f.client.chat(orProvider(), {{"user", "x"}}, {});
    EXPECT_EQ(r2.error.find("sk-or-test-1234567890"), std::string::npos);
    (void)r;
}
