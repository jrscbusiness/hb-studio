#include <gtest/gtest.h>

#include "ai/protocol.hpp"
#include "core/json.hpp"

using namespace hb::ai;

TEST(AiProtocol, BaseUrlAcceptsHttpsAndLoopbackHttp) {
    std::string out, err;
    EXPECT_TRUE(normalizeBaseUrl("https://openrouter.ai/api/v1/", out, err));
    EXPECT_EQ(out, "https://openrouter.ai/api/v1");
    EXPECT_TRUE(normalizeBaseUrl("  http://localhost:20128/v1  ", out, err));
    EXPECT_EQ(out, "http://localhost:20128/v1");
    EXPECT_TRUE(normalizeBaseUrl("http://127.0.0.1:11434/v1", out, err));
    EXPECT_TRUE(normalizeBaseUrl("http://[::1]:1234/v1", out, err));
}

TEST(AiProtocol, BaseUrlRejectsBadInput) {
    std::string out, err;
    EXPECT_FALSE(normalizeBaseUrl("http://example.com/v1", out, err));  // cleartext to a remote host
    EXPECT_NE(err.find("localhost"), std::string::npos);
    EXPECT_FALSE(normalizeBaseUrl("ftp://x/v1", out, err));
    EXPECT_FALSE(normalizeBaseUrl("https://user:pw@host/v1", out, err));
    EXPECT_FALSE(normalizeBaseUrl("https://host/v1?x=1", out, err));
    EXPECT_FALSE(normalizeBaseUrl("https://", out, err));
    EXPECT_FALSE(normalizeBaseUrl("https://host:99999/v1", out, err));
    EXPECT_FALSE(normalizeBaseUrl("https://ho st/v1", out, err));
    EXPECT_FALSE(normalizeBaseUrl("", out, err));
}

TEST(AiProtocol, JoinUrl) {
    EXPECT_EQ(joinUrl("https://a/v1/", "/models"), "https://a/v1/models");
    EXPECT_EQ(joinUrl("https://a/v1", "models"), "https://a/v1/models");
}

TEST(AiProtocol, HeadersCarryKeyOnlyWhenPresent) {
    Provider p;
    p.kind = ProviderKind::OpenRouter;
    p.apiKey = "sk-or-secret";
    auto h = buildHeaders(p, true);
    bool auth = false, sse = false, title = false;
    for (auto& kv : h) {
        if (kv.first == "Authorization" && kv.second == "Bearer sk-or-secret") auth = true;
        if (kv.first == "Accept" && kv.second == "text/event-stream") sse = true;
        if (kv.first == "X-Title") title = true;
    }
    EXPECT_TRUE(auth && sse && title);
    p.apiKey.clear();
    p.kind = ProviderKind::NineRouter;
    for (auto& kv : buildHeaders(p, false)) EXPECT_NE(kv.first, "Authorization");
}

TEST(AiProtocol, ChatBodyEscapesAndClamps) {
    ChatOptions o;
    o.temperature = 9;
    o.maxTokens = 0;
    const std::string b = buildChatBody("m/x:free", {{"system", "s"}, {"user", "a \"quoted\"\nline"}}, o, true);
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(b, v));
    EXPECT_EQ(v.get("model")->s, "m/x:free");
    EXPECT_EQ(v.get("messages")->a.size(), 2u);
    EXPECT_EQ(v.get("messages")->a[1].get("content")->s, "a \"quoted\"\nline");
    EXPECT_DOUBLE_EQ(v.get("temperature")->n, 2.0);
    EXPECT_DOUBLE_EQ(v.get("max_tokens")->n, 1.0);
    EXPECT_TRUE(v.get("stream")->b);
    hb::core::json::Value v2;
    ASSERT_TRUE(hb::core::json::parse(buildChatBody("m", {}, ChatOptions{}, false), v2));
    EXPECT_EQ(v2.get("stream"), nullptr);
}

TEST(AiProtocol, ParseChatResponse) {
    auto r = parseChatResponse(R"({"choices":[{"message":{"content":"Olá"}}]})");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.text, "Olá");
    r = parseChatResponse(R"({"choices":[{"message":{"content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]}}]})");
    EXPECT_EQ(r.text, "ab");
    r = parseChatResponse(R"({"error":{"message":"upstream down","code":502}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.providerError, "upstream down");
    EXPECT_FALSE(parseChatResponse("not json").ok);
    EXPECT_FALSE(parseChatResponse(R"({"choices":[]})").ok);
    EXPECT_FALSE(parseChatResponse(R"({"choices":[{"message":{}}]})").ok);
}

TEST(AiProtocol, SseAcrossChunkBoundariesAndComments) {
    const std::string s =
        ": OPENROUTER PROCESSING\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Ol\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"á!\"}}]}\n\n"
        "data: [DONE]\n\n";
    SseParser p;
    std::vector<std::string> ds;
    for (std::size_t i = 0; i < s.size(); i += 5) p.feed(s.substr(i, 5), ds);
    p.finish(ds);
    std::string all;
    for (auto& d : ds) all += d;
    EXPECT_EQ(all, "Olá!");
    EXPECT_TRUE(p.done());
    EXPECT_TRUE(p.sawEvents());
}

TEST(AiProtocol, SseFallsBackToPlainJsonAndReportsErrors) {
    SseParser p;
    std::vector<std::string> ds;
    p.feed(R"({"choices":[{"message":{"content":"inteiro"}}]})", ds);
    p.finish(ds);
    ASSERT_EQ(ds.size(), 1u);
    EXPECT_EQ(ds[0], "inteiro");

    SseParser e;
    std::vector<std::string> ds2;
    e.feed("data: {\"error\":{\"message\":\"cota\"}}\n\n", ds2);
    EXPECT_EQ(e.providerError(), "cota");
}

TEST(AiProtocol, ErrorClassification) {
    EXPECT_EQ(classifyHttpError(401, ""), ErrorKind::InvalidKey);
    EXPECT_EQ(classifyHttpError(403, ""), ErrorKind::InvalidKey);
    EXPECT_EQ(classifyHttpError(402, ""), ErrorKind::NoCredit);
    EXPECT_EQ(classifyHttpError(404, ""), ErrorKind::ModelUnavailable);
    EXPECT_EQ(classifyHttpError(400, "Unknown model foo"), ErrorKind::ModelUnavailable);
    EXPECT_EQ(classifyHttpError(400, "bad json"), ErrorKind::Other);
    EXPECT_EQ(classifyHttpError(429, ""), ErrorKind::RateLimited);
    EXPECT_EQ(classifyHttpError(503, ""), ErrorKind::Server);
    EXPECT_TRUE(isRetryable(429));
    EXPECT_TRUE(isRetryable(502));
    EXPECT_FALSE(isRetryable(401));
    EXPECT_FALSE(isRetryable(402));
}

TEST(AiProtocol, ErrorMessagesArePortugueseAndRedacted) {
    EXPECT_NE(errorMessagePt(ErrorKind::InvalidKey, 401, "").find("Chave de API inválida"), std::string::npos);
    EXPECT_NE(errorMessagePt(ErrorKind::NoCredit, 402, "").find("créditos"), std::string::npos);
    EXPECT_NE(errorMessagePt(ErrorKind::RateLimited, 429, "").find("Limite"), std::string::npos);
    EXPECT_NE(errorMessagePt(ErrorKind::Network, 0, "").find("conectar"), std::string::npos);
    const std::string m = errorMessagePt(ErrorKind::Other, 400, "bad Authorization: Bearer sk-live-123456789 here");
    EXPECT_EQ(m.find("sk-live-123456789"), std::string::npos);
    EXPECT_EQ(extractErrorMessage(R"({"error":{"message":"Authorization: Bearer abcdefghijk"}})").find("abcdefghijk"), std::string::npos);
    EXPECT_EQ(extractErrorMessage(R"({"error":"plain"})"), "plain");
    EXPECT_EQ(extractErrorMessage("garbage"), "");
}

TEST(AiProtocol, RetryAfter) {
    HttpResponse r;
    r.headers.emplace_back("retry-after", " 3 ");
    EXPECT_EQ(parseRetryAfterSeconds(r), 3);
    r.headers[0].second = "Wed, 21 Oct 2026 07:28:00 GMT";
    EXPECT_EQ(parseRetryAfterSeconds(r), -1);
    EXPECT_EQ(parseRetryAfterSeconds(HttpResponse{}), -1);
}

TEST(AiProtocol, FreeModelDetection) {
    EXPECT_TRUE(isFreeModel("meta/llama:free", "0.1", "0.1", true));
    EXPECT_TRUE(isFreeModel("x/y", "0", "0", true));
    EXPECT_TRUE(isFreeModel("x/y", "0.0", "0", true));
    EXPECT_FALSE(isFreeModel("x/y", "0", "0.000002", true));
    EXPECT_FALSE(isFreeModel("openrouter/auto", "-1", "-1", true));
    EXPECT_FALSE(isFreeModel("x/y", "", "", false));
    EXPECT_FALSE(isFreeModel(":free", "1", "1", true) && false);
}

TEST(AiProtocol, ParseModelsOpenRouterShapeFreeFirst) {
    const std::string body = R"({"data":[
      {"id":"z/paid","name":"Paid","context_length":8000,"pricing":{"prompt":"0.000001","completion":"0.000002"}},
      {"id":"b/free-a:free","name":"Free A","context_length":32000,"pricing":{"prompt":"0","completion":"0"}},
      {"id":"a/zero","name":"Zero","pricing":{"prompt":"0","completion":"0"}},
      {"id":"openrouter/auto","pricing":{"prompt":"-1","completion":"-1"}},
      {"nope":1}, 5]})";
    auto m = parseModels(body);
    ASSERT_EQ(m.size(), 4u);
    EXPECT_EQ(m[0].id, "a/zero");
    EXPECT_TRUE(m[0].free);
    EXPECT_EQ(m[1].id, "b/free-a:free");
    EXPECT_EQ(m[1].contextLength, 32000);
    EXPECT_FALSE(m[2].free);
    EXPECT_FALSE(m[3].free);
}

TEST(AiProtocol, ParseModelsPlainOpenAiShape) {
    auto m = parseModels(R"({"object":"list","data":[{"id":"gpt-4o-mini"},{"id":"my-combo"}]})");
    ASSERT_EQ(m.size(), 2u);
    EXPECT_FALSE(m[0].free);
    EXPECT_TRUE(parseModels("{}").empty());
    EXPECT_TRUE(parseModels("xx").empty());
}

TEST(AiProtocol, MaskKeyNeverRevealsShortKeys) {
    EXPECT_EQ(maskKey(""), "");
    EXPECT_EQ(maskKey("abcd1234"), "••••");
    EXPECT_EQ(maskKey("sk-or-v1-abcdefgh7f2a"), "••••7f2a");
}
