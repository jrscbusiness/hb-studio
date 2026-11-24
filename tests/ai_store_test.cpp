#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "ai/provider_store.hpp"
#include "core/json.hpp"

using namespace hb::ai;

namespace {
Provider mk(const std::string& name, ProviderKind k, const std::string& key = "", const std::string& model = "m") {
    Provider p;
    p.name = name;
    p.kind = k;
    p.apiKey = key;
    p.model = model;
    return p;
}
}  // namespace

TEST(AiStore, FirstProviderBecomesActiveAndDefaultsApply) {
    FakeSecretStore ss;
    ProviderStore s(&ss);
    const std::string id = s.upsert(mk("", ProviderKind::OpenRouter, "sk-or-abc"), false);
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(s.activeId(), id);
    const Provider p = s.get(id);
    EXPECT_EQ(p.name, "OpenRouter");
    EXPECT_EQ(p.baseUrl, "https://openrouter.ai/api/v1");
    const std::string id2 = s.upsert(mk("9r", ProviderKind::NineRouter), false);
    EXPECT_EQ(s.get(id2).baseUrl, "http://localhost:20128/v1");
    EXPECT_EQ(s.activeId(), id);  // adding does not steal active
    EXPECT_TRUE(s.activate(id2));
    EXPECT_EQ(s.active().id, id2);
    EXPECT_FALSE(s.activate("nope"));
}

TEST(AiStore, KeyIsTrimmedAndKeptWhenBlank) {
    FakeSecretStore ss;
    ProviderStore s(&ss);
    const std::string id = s.upsert(mk("a", ProviderKind::OpenRouter, "  sk-or-abc\r\n"), false);
    EXPECT_EQ(s.get(id).apiKey, "sk-or-abc");
    Provider edit = s.get(id);
    edit.apiKey = "";
    edit.model = "other";
    ASSERT_EQ(s.upsert(edit, true), id);
    EXPECT_EQ(s.get(id).apiKey, "sk-or-abc");
    EXPECT_EQ(s.get(id).model, "other");
    edit.apiKey = "new-key-value";
    s.upsert(edit, false);
    EXPECT_EQ(s.get(id).apiKey, "new-key-value");
}

TEST(AiStore, RejectsBadUrlUnknownIdAndLimit) {
    ProviderStore s(nullptr);
    std::string err;
    Provider p = mk("x", ProviderKind::Custom);
    p.baseUrl = "http://remote.example.com/v1";
    EXPECT_EQ(s.upsert(p, false, &err), "");
    EXPECT_FALSE(err.empty());
    p.baseUrl = "https://api.groq.com/openai/v1";
    p.id = "ghost";
    EXPECT_EQ(s.upsert(p, false, &err), "");
    p.id.clear();
    for (int i = 0; i < ProviderStore::kMax; ++i) EXPECT_FALSE(s.upsert(p, false).empty());
    EXPECT_EQ(s.upsert(p, false, &err), "");
}

TEST(AiStore, RemoveActiveFallsBack) {
    ProviderStore s(nullptr);
    Provider p = mk("x", ProviderKind::NineRouter);
    const auto a = s.upsert(p, false), b = s.upsert(p, false);
    EXPECT_TRUE(s.remove(a));
    EXPECT_EQ(s.activeId(), b);
    EXPECT_TRUE(s.remove(b));
    EXPECT_EQ(s.activeId(), "");
    EXPECT_FALSE(s.remove(b));
}

TEST(AiStore, PersistedFileNeverContainsPlaintextKey) {
    FakeSecretStore ss;
    ProviderStore s(&ss);
    s.upsert(mk("a", ProviderKind::OpenRouter, "sk-or-SUPERSECRET"), false);
    const std::string json = s.toJson();
    EXPECT_EQ(json.find("SUPERSECRET"), std::string::npos);
    EXPECT_EQ(json.find("apiKey"), std::string::npos);
    EXPECT_NE(json.find("keyBlob"), std::string::npos);
    // The reversed fake blob must not leak it in hex either.
    EXPECT_EQ(json.find("TERCES"), std::string::npos);

    ProviderStore s2(&ss);
    ASSERT_TRUE(s2.fromJson(json));
    ASSERT_EQ(s2.list().size(), 1u);
    EXPECT_EQ(s2.list()[0].apiKey, "sk-or-SUPERSECRET");
    EXPECT_EQ(s2.activeId(), s.activeId());
    EXPECT_FALSE(s2.lastLoadLostKeys());
}

TEST(AiStore, PlaintextKeyFieldInFileIsIgnored) {
    FakeSecretStore ss;
    ProviderStore s(&ss);
    ASSERT_TRUE(s.fromJson(R"({"providers":[{"id":"p1","kind":"openrouter","baseUrl":"https://openrouter.ai/api/v1","apiKey":"planted"}]})"));
    EXPECT_EQ(s.list()[0].apiKey, "");
}

TEST(AiStore, UndecryptableKeyIsFlaggedNotFatal) {
    FakeSecretStore ss;
    ProviderStore s(&ss);
    s.upsert(mk("a", ProviderKind::OpenRouter, "k-1234567890"), false);
    const std::string json = s.toJson();
    ss.failUnprotect = true;
    ProviderStore s2(&ss);
    ASSERT_TRUE(s2.fromJson(json));
    EXPECT_EQ(s2.list().size(), 1u);
    EXPECT_EQ(s2.list()[0].apiKey, "");
    EXPECT_TRUE(s2.lastLoadLostKeys());
}

TEST(AiStore, WithoutSecretStoreKeysAreNotWritten) {
    ProviderStore s(nullptr);
    s.upsert(mk("a", ProviderKind::OpenRouter, "k-1234567890"), false);
    EXPECT_EQ(s.toJson().find("keyBlob"), std::string::npos);
    EXPECT_NE(s.uiJson().find("\"canPersistKeys\":false"), std::string::npos);
}

TEST(AiStore, UiJsonIsMaskedAndValid) {
    FakeSecretStore ss;
    ProviderStore s(&ss);
    s.upsert(mk("a", ProviderKind::OpenRouter, "sk-or-v1-abcdefgh7f2a"), false);
    const std::string j = s.uiJson();
    EXPECT_EQ(j.find("abcdefgh"), std::string::npos);
    EXPECT_NE(j.find("7f2a"), std::string::npos);
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(j, v));
    EXPECT_TRUE(v.get("configured")->b);
    EXPECT_TRUE(v.get("list")->a[0].get("keySet")->b);
    EXPECT_EQ(v.get("list")->a[0].get("apiKey"), nullptr);
}

TEST(AiStore, ConfiguredFlagRules) {
    ProviderStore s(nullptr);
    s.upsert(mk("or", ProviderKind::OpenRouter, "", "m"), false);
    EXPECT_NE(s.uiJson().find("\"configured\":false"), std::string::npos);  // key required, missing
    ProviderStore t(nullptr);
    t.upsert(mk("local", ProviderKind::NineRouter, "", "m"), false);
    EXPECT_NE(t.uiJson().find("\"configured\":true"), std::string::npos);
    ProviderStore u(nullptr);
    u.upsert(mk("local", ProviderKind::NineRouter, "", ""), false);
    EXPECT_NE(u.uiJson().find("\"configured\":false"), std::string::npos);  // no model
}

TEST(AiStore, SaveLoadRoundTripOnDisk) {
    FakeSecretStore ss;
    const std::string path = ::testing::TempDir() + "hb_ai_store_test.json";
    {
        ProviderStore s(&ss);
        s.upsert(mk("a", ProviderKind::OpenRouter, "sk-or-disk-key-123"), false);
        ASSERT_TRUE(s.save(path));
        ASSERT_TRUE(s.save(path));  // overwrite path
    }
    ProviderStore s2(&ss);
    ASSERT_TRUE(s2.load(path));
    EXPECT_EQ(s2.active().apiKey, "sk-or-disk-key-123");
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream buf;
        buf << f.rdbuf();
        EXPECT_EQ(buf.str().find("disk-key"), std::string::npos);
    }   // closed before remove(): Windows cannot delete a file that is still open
    std::remove(path.c_str());
    EXPECT_FALSE(s2.load(path));
}

TEST(AiStore, HostileFilesDoNotCrash) {
    ProviderStore s(nullptr);
    EXPECT_FALSE(s.fromJson("garbage"));
    EXPECT_FALSE(s.fromJson("[]"));
    EXPECT_FALSE(s.fromJson("{}"));
    EXPECT_TRUE(s.fromJson(R"({"providers":[5,{"id":""},{"id":"x","baseUrl":"http://remote.com"},{"id":"ok","kind":"custom","baseUrl":"https://a.b/v1"}]})"));
    EXPECT_EQ(s.list().size(), 1u);
}

TEST(AiStore, HexRoundTrip) {
    std::string raw("\x00\x01\xfe\xff", 4), back;
    EXPECT_TRUE(fromHex(toHex(raw), back));
    EXPECT_EQ(back, raw);
    EXPECT_FALSE(fromHex("abc", back));
    EXPECT_FALSE(fromHex("zz", back));
}

TEST(AiStore, ParseProviderFromUiJson) {
    Provider p;
    bool hasKey = true;
    ASSERT_TRUE(ProviderStore::parseProvider(R"({"id":"p3","kind":"9router","baseUrl":"http://localhost:20128/v1","model":"combo","apiKey":""})", p, hasKey));
    EXPECT_EQ(p.id, "p3");
    EXPECT_EQ(p.kind, ProviderKind::NineRouter);
    EXPECT_FALSE(hasKey);
    ASSERT_TRUE(ProviderStore::parseProvider(R"({"apiKey":"abc"})", p, hasKey));
    EXPECT_TRUE(hasKey);
    EXPECT_FALSE(ProviderStore::parseProvider("nope", p, hasKey));
}
