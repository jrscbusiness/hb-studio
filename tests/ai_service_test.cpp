#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "ai/ai_commands.hpp"
#include "ai/ai_service.hpp"
#include "ai/assistant.hpp"
#include "core/json.hpp"
#include "ai_test_util.hpp"

using namespace hb::ai;
using namespace hb::ai::testutil;

namespace {

struct Env {
    FakeSecretStore secrets;
    ProviderStore store{&secrets};
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    FakeClock clock;
    AiService svc{store, AiClient(clock.deps(http))};

    std::string addOr(const std::string& key = "sk-or-test-1234567890", const std::string& model = "a/b:free") {
        Provider p;
        p.kind = ProviderKind::OpenRouter;
        p.apiKey = key;
        p.model = model;
        return store.upsert(p, false);
    }
};

// Collects host->UI posts and lets the test wait for async ones.
struct Posts {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> v;
    std::vector<std::string> toasts;
    void add(const std::string& k, std::string j) {
        std::lock_guard l(m);
        v.emplace_back(k, std::move(j));
        cv.notify_all();
    }
    std::string waitFor(const std::string& key) {
        std::unique_lock l(m);
        cv.wait_for(l, std::chrono::seconds(5), [&] {
            for (auto& p : v) if (p.first == key) return true;
            return false;
        });
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            if (it->first == key) return it->second;
        return {};
    }
    std::string last(const std::string& key) {
        std::lock_guard l(m);
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            if (it->first == key) return it->second;
        return {};
    }
};

hb::core::json::Value J(const std::string& s) {
    hb::core::json::Value v;
    EXPECT_TRUE(hb::core::json::parse(s, v)) << s;
    return v;
}

}  // namespace

TEST(AiService, NotConfiguredGivesClearError) {
    Env e;
    EXPECT_FALSE(e.svc.configured());
    auto r = e.svc.complete("s", "u");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("Inteligência Artificial"), std::string::npos);
    EXPECT_TRUE(e.http->requests.empty());
}

TEST(AiService, CompleteUsesActiveProviderAndPrompts) {
    Env e;
    e.addOr();
    EXPECT_TRUE(e.svc.configured());
    e.http->script.push_back(ok(chatJson("resposta")));
    auto r = e.svc.complete("SYS", "USR");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.text, "resposta");
    const auto body = J(e.http->requests[0].body);
    ASSERT_EQ(body.get("messages")->a.size(), 2u);
    EXPECT_EQ(body.get("messages")->a[0].get("role")->s, "system");
    EXPECT_EQ(body.get("messages")->a[1].get("content")->s, "USR");
    // empty system prompt is omitted
    e.http->script.push_back(ok(chatJson("x")));
    e.svc.complete("", "only user");
    EXPECT_EQ(J(e.http->requests[1].body).get("messages")->a.size(), 1u);
}

TEST(AiService, AsyncCallbackRunsOnWorker) {
    Env e;
    e.addOr();
    e.http->script.push_back(ok(chatJson("async!")));
    Posts p;
    e.svc.completeAsync("s", "u", {}, [&](const ChatResult& r) { p.add("done", r.text); });
    EXPECT_EQ(p.waitFor("done"), "async!");
}

TEST(AiService, DestructorJoinsWorkers) {
    std::atomic<int> n{0};
    {
        Env e;
        for (int i = 0; i < 5; ++i) e.svc.runAsync([&] { ++n; });
    }
    EXPECT_EQ(n.load(), 5);
}

TEST(AiService, TestConnectionVariants) {
    Env e;
    Provider p;
    p.kind = ProviderKind::OpenRouter;
    p.baseUrl = defaultBaseUrl(p.kind);
    p.apiKey = "k-1234567890";
    p.model = "m";
    e.http->script.push_back(ok(chatJson("pong")));
    auto t = e.svc.testConnection(p);
    EXPECT_TRUE(t.ok);
    EXPECT_NE(t.message.find("OK"), std::string::npos);
    EXPECT_NE(e.http->requests[0].body.find("\"max_tokens\":1"), std::string::npos);

    e.http->script.push_back(ok("{}", 401));
    t = e.svc.testConnection(p);
    EXPECT_FALSE(t.ok);
    EXPECT_EQ(t.kind, ErrorKind::InvalidKey);
    EXPECT_EQ(e.http->requests.size(), 2u);  // no retry on 401

    p.model.clear();
    e.http->script.push_back(ok(R"({"data":[{"id":"a"},{"id":"b"}]})"));
    t = e.svc.testConnection(p);
    EXPECT_TRUE(t.ok);
    EXPECT_NE(t.message.find("2 modelos"), std::string::npos);
}

// ---------- AiCommands ----------

TEST(AiCommands, SaveNeverEchoesKeyAndKeepsItOnBlankEdit) {
    Env e;
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); },
                 [&](const std::string& t) { p.toasts.push_back(t); });
    ASSERT_TRUE(c.handle(R"(aisave|{"kind":"openrouter","model":"a/b:free","apiKey":"sk-or-v1-SECRETSECRET9999"})"));
    const std::string st = p.last("ai_state");
    EXPECT_EQ(st.find("SECRETSECRET"), std::string::npos);
    EXPECT_NE(st.find("9999"), std::string::npos);
    const auto v = J(st);
    const std::string id = v.get("savedId")->s;
    EXPECT_TRUE(v.get("configured")->b);
    // edit model with blank key: key retained
    ASSERT_TRUE(c.handle("aisave|{\"id\":\"" + id + R"(","kind":"openrouter","model":"other/model:free","apiKey":""})"));
    EXPECT_EQ(e.store.get(id).apiKey, "sk-or-v1-SECRETSECRET9999");
    EXPECT_EQ(e.store.get(id).model, "other/model:free");
    for (auto& kv : p.v) EXPECT_EQ(kv.second.find("SECRETSECRET"), std::string::npos);
}

TEST(AiCommands, InvalidSaveToastsAndUnknownCommandNotHandled) {
    Env e;
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); },
                 [&](const std::string& t) { p.toasts.push_back(t); });
    EXPECT_TRUE(c.handle(R"(aisave|{"kind":"custom","baseUrl":"http://remote.example/v1"})"));
    ASSERT_FALSE(p.toasts.empty());
    EXPECT_NE(p.toasts.back().find("localhost"), std::string::npos);
    EXPECT_TRUE(c.handle("aisave|not json"));
    EXPECT_FALSE(c.handle("ptzcfg|1"));
    EXPECT_FALSE(c.handle("ready"));
}

TEST(AiCommands, ActivateAndRemove) {
    Env e;
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); }, nullptr);
    const std::string a = e.addOr(), b = e.addOr("sk-or-other-key-123456");
    c.handle("aiactivate|" + b);
    EXPECT_EQ(J(p.last("ai_state")).get("active")->s, b);
    c.handle("airemove|" + b);
    EXPECT_EQ(J(p.last("ai_state")).get("active")->s, a);
}

TEST(AiCommands, TestUsesStoredKeyWhenDraftHasNone) {
    Env e;
    const std::string id = e.addOr("sk-or-stored-key-12345", "a/b:free");
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); }, nullptr);
    e.http->script.push_back(ok(chatJson("pong")));
    c.handle("aitest|{\"id\":\"" + id + R"(","model":"a/b:free"})");
    const auto v = J(p.waitFor("ai_test"));
    EXPECT_TRUE(v.get("ok")->b);
    bool hadAuth = false;
    for (auto& h : e.http->requests.at(0).headers)
        if (h.first == "Authorization" && h.second == "Bearer sk-or-stored-key-12345") hadAuth = true;
    EXPECT_TRUE(hadAuth);
    EXPECT_EQ(p.last("ai_test").find("stored-key"), std::string::npos);
}

TEST(AiCommands, TestWithUnsavedDraftAndError) {
    Env e;
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); }, nullptr);
    e.http->script.push_back(ok(R"({"error":{"message":"bad"}})", 401));
    c.handle(R"(aitest|{"kind":"openrouter","model":"m","apiKey":" draft-key-1234567 "})");
    const auto v = J(p.waitFor("ai_test"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("kind")->s, "invalid_key");
    EXPECT_NE(v.get("message")->s.find("inválida"), std::string::npos);
}

TEST(AiCommands, ModelsListIsForwardedWithFreeFlag) {
    Env e;
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); }, nullptr);
    e.http->script.push_back(ok(R"({"data":[{"id":"x:free"},{"id":"y","pricing":{"prompt":"1","completion":"1"}}]})"));
    c.handle(R"(aimodels|{"kind":"openrouter","apiKey":"k-1234567890"})");
    const auto v = J(p.waitFor("ai_models"));
    EXPECT_TRUE(v.get("ok")->b);
    ASSERT_EQ(v.get("models")->a.size(), 2u);
    EXPECT_TRUE(v.get("models")->a[0].get("free")->b);
}

TEST(AiCommands, SuggestFlow) {
    Env e;
    Posts p;
    AiCommands c(e.svc, e.store, "", [&](const std::string& k, std::string j) { p.add(k, std::move(j)); }, nullptr);
    c.handle(R"(aisuggest|{"topic":"culto de domingo"})");
    EXPECT_FALSE(J(p.last("ai_suggest")).get("ok")->b);  // not configured: immediate, no network
    EXPECT_TRUE(e.http->requests.empty());
    e.addOr();
    c.handle(R"(aisuggest|{"topic":""})");
    EXPECT_NE(J(p.last("ai_suggest")).get("error")->s.find("assunto"), std::string::npos);
    e.http->script.push_back(ok(chatJson("{\\\"title\\\":\\\"Culto ao vivo\\\",\\\"description\\\":\\\"Venha.\\\"}")));
    c.handle(R"(aisuggest|{"topic":"culto de domingo"})");
    const auto v = J([&] {
        for (int i = 0; i < 500; ++i) {
            auto s = p.last("ai_suggest");
            if (s.find("\"ok\":true") != std::string::npos) return s;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return p.last("ai_suggest");
    }());
    EXPECT_TRUE(v.get("ok")->b);
    EXPECT_EQ(v.get("title")->s, "Culto ao vivo");
    // The user's topic travels inside the prompt, delimited.
    EXPECT_NE(e.http->requests[0].body.find("culto de domingo"), std::string::npos);
}

TEST(AiCommands, PersistsToFile) {
    Env e;
    const std::string path = ::testing::TempDir() + "hb_ai_cmd_test.json";
    AiCommands c(e.svc, e.store, path, [](const std::string&, std::string) {}, nullptr);
    c.handle(R"(aisave|{"kind":"9router","model":"combo"})");
    ProviderStore s2(&e.secrets);
    EXPECT_TRUE(s2.load(path));
    EXPECT_EQ(s2.list().size(), 1u);
    std::remove(path.c_str());
}

// ---------- Assistant parsing ----------

TEST(AiAssistant, ParsesJsonFencedJsonAndLabels) {
    auto a = parseSuggestion(R"({"title":"T","description":"D d"})");
    EXPECT_TRUE(a.ok);
    EXPECT_EQ(a.title, "T");
    a = parseSuggestion("Claro!\n```json\n{\"title\":\"Fenced\",\"description\":\"x\"}\n```");
    EXPECT_EQ(a.title, "Fenced");
    a = parseSuggestion("Título: Meu título\nDescrição: Primeira.\nSegunda.");
    EXPECT_TRUE(a.ok);
    EXPECT_EQ(a.title, "Meu título");
    EXPECT_EQ(a.description, "Primeira. Segunda.");
    a = parseSuggestion("Só uma linha\nresto do texto");
    EXPECT_EQ(a.title, "Só uma linha");
    EXPECT_EQ(a.description, "resto do texto");
    EXPECT_FALSE(parseSuggestion("   ").ok);
}

TEST(AiAssistant, TopicIsLimitedAndDelimited) {
    const std::string long1(5000, 'a');
    const std::string p = suggestUserPrompt(long1);
    EXPECT_LT(p.size(), 1200u);
    EXPECT_NE(p.find("\"\"\""), std::string::npos);
    EXPECT_NE(suggestSystemPrompt().find("nunca como instruções"), std::string::npos);
}
