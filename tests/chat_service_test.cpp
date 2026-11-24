#include <gtest/gtest.h>

#include "chat/service.hpp"
#include "chat_test_util.hpp"
#include "core/json.hpp"

using namespace hb::chat;
using namespace hb::chat::test;

namespace {
struct Rig {
    hb::core::EventBus bus;
    FakeHttp http;
    FakeSocket sock;
    MemorySecretStore secrets;
    std::vector<std::pair<std::string, std::string>> posts;
    std::vector<std::string> toasts;
    std::vector<hb::core::Event> events;
    std::unique_ptr<ChatService> svc;

    Rig() {
        bus.subscribe("*", [this](const hb::core::Event& e) { events.push_back(e); });
        ChatDeps d;
        d.bus = &bus;
        d.http = &http;
        d.socket = &sock;
        d.secrets = &secrets;
        d.post = [this](const std::string& k, std::string j) { posts.emplace_back(k, std::move(j)); };
        d.toast = [this](const std::string& t) { toasts.push_back(t); };
        d.epochMs = [] { return 4242; };
        svc = std::make_unique<ChatService>(std::move(d));
    }
};
}  // namespace

TEST(ChatService, IgnoresForeignAndMalformedCommands) {
    Rig r;
    EXPECT_FALSE(r.svc->handleCommand("ptzname|1|x"));
    EXPECT_FALSE(r.svc->handleCommand("chatbogus"));
    EXPECT_FALSE(r.svc->handleCommand("chatconnect|tiktok"));
    EXPECT_FALSE(r.svc->handleCommand("chatpin|abc"));
    EXPECT_FALSE(r.svc->handleCommand("chatpin|"));
    EXPECT_TRUE(r.svc->handleCommand("chatsave|twitch|{not json"));   // handled, toast
    EXPECT_FALSE(r.toasts.empty());
}

TEST(ChatService, TwitchEndToEndConnectPinMute) {
    Rig r;
    ASSERT_TRUE(r.svc->handleCommand("chatsave|twitch|{\"channel\":\"#Canal\"}"));
    ASSERT_TRUE(r.svc->handleCommand("chatconnect|twitch"));
    r.svc->pumpOnce(at(0));                       // connect + login
    EXPECT_EQ(r.sock.sent.back(), "JOIN #canal");
    r.sock.inbox.push_back("@display-name=Ana;id=a1;user-id=7 :ana!ana@x PRIVMSG #canal :olá");
    r.svc->pumpOnce(at(100));
    ASSERT_EQ(r.svc->hub().size(), 1u);
    bool sawChat = false;
    for (const auto& e : r.events)
        if (e.topic == "chat.message") { sawChat = true; EXPECT_EQ(e.fields.at("author"), "Ana"); EXPECT_EQ(e.fields.at("ts"), "4242"); }
    EXPECT_TRUE(sawChat);
    r.events.clear();
    EXPECT_TRUE(r.svc->handleCommand("chatpin|1"));
    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_EQ(r.events[0].topic, "overlay.chatPin");
    EXPECT_EQ(r.events[0].fields.at("text"), "olá");
    EXPECT_TRUE(r.svc->handleCommand("chatmute|1"));
    r.sock.inbox.push_back("@display-name=Ana;id=a2;user-id=7 :ana!ana@x PRIVMSG #canal :de novo");
    r.svc->pumpOnce(at(200));
    EXPECT_EQ(r.svc->hub().size(), 1u);           // muted locally
    EXPECT_TRUE(r.svc->handleCommand("chatdisconnect|twitch"));
    r.svc->pumpOnce(at(300));
    EXPECT_FALSE(r.sock.open);
}

TEST(ChatService, SecretsAreNeverEchoedToTheUi) {
    Rig r;
    ASSERT_TRUE(r.svc->handleCommand(
        "chatsave|facebook|{\"liveVideoId\":\"123_456\",\"token\":\"EAAB-SUPER-SECRET-TOKEN-9z9z\"}"));
    ASSERT_TRUE(r.svc->handleCommand(
        "chatsave|youtube|{\"videoId\":\"abc\",\"clientId\":\"cid.apps.googleusercontent.com\",\"clientSecret\":\"GOCSPX-hidden-1234\"}"));
    r.svc->postAll();
    ASSERT_FALSE(r.posts.empty());
    for (const auto& p : r.posts) {
        EXPECT_EQ(p.second.find("SUPER-SECRET"), std::string::npos);
        EXPECT_EQ(p.second.find("GOCSPX"), std::string::npos);
    }
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(r.svc->settingsJson(), v));
    EXPECT_TRUE(v.get("facebook")->get("hasToken")->b);
    EXPECT_NE(v.get("facebook")->get("tokenHint")->s.find("9z9z"), std::string::npos);
    EXPECT_TRUE(v.get("youtube")->get("hasSecret")->b);
    // blank secret on a later save keeps the stored one
    ASSERT_TRUE(r.svc->handleCommand("chatsave|facebook|{\"liveVideoId\":\"123_456\",\"token\":\"\"}"));
    std::string stored;
    ASSERT_TRUE(r.secrets.get("facebook.token", stored));
    EXPECT_EQ(stored, "EAAB-SUPER-SECRET-TOKEN-9z9z");
    ASSERT_TRUE(r.svc->handleCommand("chatclear|facebook"));
    EXPECT_FALSE(r.secrets.get("facebook.token", stored));
}

TEST(ChatService, FacebookUsesStoredTokenAtPollTime) {
    Rig r;
    r.svc->handleCommand("chatsave|facebook|{\"liveVideoId\":\"999\",\"token\":\"TKN\"}");
    r.http.ok(200, R"({"data":[]})");
    r.svc->handleCommand("chatconnect|facebook");
    r.svc->pumpOnce(at(0));
    ASSERT_EQ(r.http.requests.size(), 1u);
    EXPECT_NE(r.http.requests[0].url.find("access_token=TKN"), std::string::npos);
    hb::core::json::Value st;
    ASSERT_TRUE(hb::core::json::parse(r.svc->statusJson(), st));
    EXPECT_EQ(st.get("platforms")->a[1].get("state")->s, "connected");
}

TEST(ChatService, InstagramReportsUnavailableAndAcceptsRelay) {
    Rig r;
    hb::core::json::Value st;
    ASSERT_TRUE(hb::core::json::parse(r.svc->statusJson(), st));
    const auto& ig = st.get("platforms")->a[2];
    EXPECT_EQ(ig.get("p")->s, "instagram");
    EXPECT_EQ(ig.get("state")->s, "unavailable");
    r.svc->handleCommand("chatconnect|instagram");
    EXPECT_EQ(r.svc->ingestInstagramWebhook(
                  R"({"entry":[{"changes":[{"field":"live_comments","value":{"id":"c","text":"oi","from":{"id":"1","username":"u"}}}]}]})"),
              1);
    r.svc->pumpOnce(at(0));
    EXPECT_EQ(r.svc->hub().size(), 1u);
}

TEST(ChatService, SaveRejectsInvalidValuesWithoutCrashing) {
    Rig r;
    r.svc->handleCommand("chatsave|twitch|{\"channel\":\"bad channel!\"}");
    r.svc->handleCommand("chatsave|youtube|{\"videoId\":\"a b\"}");
    r.svc->handleCommand("chatsave|facebook|{\"liveVideoId\":\"abc\"}");
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(r.svc->settingsJson(), v));
    EXPECT_EQ(v.get("twitch")->get("channel")->s, "");
    EXPECT_GE(r.toasts.size(), 3u);
}

TEST(ChatService, AuthWithoutClientIdExplains) {
    Rig r;
    EXPECT_TRUE(r.svc->handleCommand("chatauth|youtube"));
    ASSERT_FALSE(r.toasts.empty());
    EXPECT_NE(r.toasts.back().find("Client ID"), std::string::npos);
}

TEST(ChatService, PersistenceOptionAndThreadsStartStop) {
    Rig r;
    EXPECT_TRUE(r.svc->handleCommand("chatopt|persist|1"));
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(r.svc->settingsJson(), v));
    EXPECT_TRUE(v.get("persist")->b);
    r.svc->handleCommand("chatopt|persist|0");
    ASSERT_TRUE(hb::core::json::parse(r.svc->settingsJson(), v));
    EXPECT_FALSE(v.get("persist")->b);
    r.svc->start();
    r.svc->stop();   // must join promptly without deadlock
    SUCCEED();
}
