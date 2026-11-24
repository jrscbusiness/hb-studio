#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "chat/hub.hpp"
#include "chat/settings.hpp"
#include "core/json.hpp"

using namespace hb::chat;

namespace {
ChatMessage msg(Platform p, const std::string& id, const std::string& author, const std::string& text, const std::string& aid = "") {
    ChatMessage m;
    m.platform = p;
    m.id = id;
    m.author = author;
    m.authorId = aid;
    m.text = text;
    m.tsMs = 1000;
    return m;
}
struct Capture {
    std::vector<hb::core::Event> events;
    explicit Capture(hb::core::EventBus& bus) {
        bus.subscribe("*", [this](const hb::core::Event& e) { events.push_back(e); });
    }
};
std::string tempPath(const char* name) {
    const char* t = std::getenv("TEMP");
    if (!t) t = std::getenv("TMPDIR");
    return std::string(t ? t : ".") + "/hb_chat_test_" + name;
}
}  // namespace

TEST(ChatHub, DedupesByPlatformAndId) {
    ChatHub hub(nullptr);
    EXPECT_EQ(hub.ingest({msg(Platform::Twitch, "1", "a", "x"), msg(Platform::Twitch, "1", "a", "x"),
                          msg(Platform::YouTube, "1", "a", "x")}), 2);   // same id on another platform is distinct
    EXPECT_EQ(hub.ingest({msg(Platform::Twitch, "1", "a", "x")}), 0);
    EXPECT_EQ(hub.size(), 2u);
}

TEST(ChatHub, AssignsSequenceAndPublishesChatMessage) {
    hb::core::EventBus bus;
    Capture cap(bus);
    ChatHub hub(&bus);
    hub.ingest({msg(Platform::YouTube, "id1", "Ana", "olá", "UC1")});
    ASSERT_EQ(cap.events.size(), 1u);
    const auto& e = cap.events[0];
    EXPECT_EQ(e.topic, "chat.message");
    EXPECT_EQ(e.fields.at("platform"), "youtube");
    EXPECT_EQ(e.fields.at("author"), "Ana");
    EXPECT_EQ(e.fields.at("text"), "olá");
    EXPECT_EQ(e.fields.at("id"), "id1");
    EXPECT_EQ(e.fields.at("ts"), "1000");
    EXPECT_EQ(hub.snapshot()[0].seq, 1u);
}

TEST(ChatHub, SynthesizesIdWhenMissing) {
    ChatHub hub(nullptr);
    EXPECT_EQ(hub.ingest({msg(Platform::Instagram, "", "u", "same"), msg(Platform::Instagram, "", "u", "same")}), 1);
    EXPECT_FALSE(hub.snapshot()[0].id.empty());
}

TEST(ChatHub, RingBufferKeepsLastN) {
    ChatHub hub(nullptr, 10);
    for (int i = 0; i < 25; ++i) hub.ingest({msg(Platform::Twitch, std::to_string(i), "a", "m" + std::to_string(i))});
    const auto s = hub.snapshot();
    ASSERT_EQ(s.size(), 10u);
    EXPECT_EQ(s.front().text, "m15");
    EXPECT_EQ(s.back().text, "m24");
}

TEST(ChatHub, MuteDropsFutureMessagesAndIsNotPublished) {
    hb::core::EventBus bus;
    Capture cap(bus);
    ChatHub hub(&bus);
    hub.ingest({msg(Platform::Twitch, "1", "Troll", "spam", "99")});
    ASSERT_TRUE(hub.muteUserOf(1));
    EXPECT_EQ(hub.ingest({msg(Platform::Twitch, "2", "Troll", "spam2", "99"), msg(Platform::Twitch, "3", "Ok", "hi", "5")}), 1);
    EXPECT_EQ(cap.events.size(), 2u);   // first + "Ok"
    EXPECT_EQ(hub.muted(), std::vector<std::string>{"twitch:99"});
    hub.unmuteUser("twitch:99");
    EXPECT_EQ(hub.ingest({msg(Platform::Twitch, "4", "Troll", "again", "99")}), 1);
    EXPECT_FALSE(hub.muteUserOf(12345));
}

TEST(ChatHub, PinPublishesFrozenOverlayContract) {
    hb::core::EventBus bus;
    Capture cap(bus);
    ChatHub hub(&bus);
    hub.ingest({msg(Platform::Facebook, "f1", "Carlos", "Parabéns!")});
    cap.events.clear();
    ASSERT_TRUE(hub.pin(1));
    ASSERT_EQ(cap.events.size(), 1u);
    EXPECT_EQ(cap.events[0].topic, "overlay.chatPin");
    EXPECT_EQ(cap.events[0].fields.at("author"), "Carlos");
    EXPECT_EQ(cap.events[0].fields.at("text"), "Parabéns!");
    EXPECT_EQ(cap.events[0].fields.at("platform"), "facebook");
    EXPECT_FALSE(hub.pin(999));
    cap.events.clear();
    hub.clearPin();
    ASSERT_EQ(cap.events.size(), 1u);
    EXPECT_EQ(cap.events[0].fields.at("text"), "");
}

TEST(ChatHub, BatchesAreSequencedAndBoundedUnderFlood) {
    ChatHub hub(nullptr, 500);
    EXPECT_EQ(hub.takeBatchJson(), "");
    for (int i = 0; i < 120; ++i) hub.ingest({msg(Platform::Twitch, std::to_string(i), "a", "m")});
    const std::string b1 = hub.takeBatchJson(50);
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(b1, v));
    EXPECT_EQ(v.get("t")->s, "chat_msgs");
    EXPECT_EQ(v.get("seq")->n, 1);
    EXPECT_EQ(v.get("msgs")->a.size(), 50u);
    EXPECT_EQ(v.get("dropped")->n, 70);                       // oldest 70 dropped from the UI feed, newest 50 kept
    EXPECT_EQ(v.get("msgs")->a.back().get("n")->n, 120);
    EXPECT_EQ(hub.takeBatchJson(), "");
    hub.ingest({msg(Platform::Twitch, "x", "a", "m")});
    hb::core::json::Value v2;
    ASSERT_TRUE(hb::core::json::parse(hub.takeBatchJson(), v2));
    EXPECT_EQ(v2.get("seq")->n, 2);
    EXPECT_EQ(hub.size(), 121u);                              // the ring buffer still has everything
}

TEST(ChatHub, MessageJsonFlagsAndEscaping) {
    ChatMessage m = msg(Platform::Twitch, "1", "NightBot", "!hello \"q\"\\", "1");
    m.avatarUrl = "https://x/y.png";
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(ChatHub::messageJson(m), v));
    EXPECT_EQ(v.get("x")->s, "!hello \"q\"\\");
    EXPECT_EQ(v.get("cmd")->n, 1);
    EXPECT_EQ(v.get("bot")->n, 1);
    EXPECT_EQ(v.get("uk")->s, "twitch:1");
    EXPECT_EQ(v.get("av")->s, "https://x/y.png");
}

TEST(ChatHub, HistoryResetsPending) {
    ChatHub hub(nullptr);
    hub.ingest({msg(Platform::Twitch, "1", "a", "x")});
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(hub.historyJson(), v));
    EXPECT_TRUE(v.get("reset")->b);
    EXPECT_EQ(v.get("msgs")->a.size(), 1u);
    EXPECT_EQ(hub.takeBatchJson(), "");
}

TEST(ChatHub, FileLogOffByDefaultAndWritesWhenAttached) {
    const std::string path = tempPath("log.jsonl");
    std::remove(path.c_str());
    ChatHub hub(nullptr);
    hub.ingest({msg(Platform::Twitch, "1", "a", "before")});
    hub.setLog(std::make_unique<FileMessageLog>(path));
    hub.ingest({msg(Platform::Twitch, "2", "a", "after")});
    hub.setLog(nullptr);
    hub.ingest({msg(Platform::Twitch, "3", "a", "later")});
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    EXPECT_EQ(text.find("before"), std::string::npos);
    EXPECT_NE(text.find("after"), std::string::npos);
    EXPECT_EQ(text.find("later"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ChatSettings, RoundTripValidationAndNoSecrets) {
    ChatSettings s;
    s.youtubeVideoId = "abc_123-XYZ";
    s.youtubeClientId = "123-abc.apps.googleusercontent.com";
    s.facebookLiveVideoId = "1234_5678";
    s.twitchChannel = "meucanal";
    s.persistMessages = true;
    s.muted = {"twitch:1", "youtube:UC2"};
    ChatSettings r;
    ASSERT_TRUE(ChatSettings::fromJson(s.toJson(), r));
    EXPECT_EQ(r.youtubeVideoId, s.youtubeVideoId);
    EXPECT_EQ(r.youtubeClientId, s.youtubeClientId);
    EXPECT_EQ(r.facebookLiveVideoId, s.facebookLiveVideoId);
    EXPECT_EQ(r.twitchChannel, "meucanal");
    EXPECT_TRUE(r.persistMessages);
    EXPECT_EQ(r.muted.size(), 2u);
    EXPECT_FALSE(ChatSettings::fromJson("nope", r));
    ChatSettings d;
    EXPECT_FALSE(d.persistMessages);   // privacy: off by default
    // hostile content is sanitized away
    ASSERT_TRUE(ChatSettings::fromJson(R"({"youtubeVideoId":"a b;rm","twitchChannel":"UPPER CASE","facebookLiveVideoId":"x"})", r));
    EXPECT_EQ(r.youtubeVideoId, "");
    EXPECT_EQ(r.twitchChannel, "");
    EXPECT_EQ(r.facebookLiveVideoId, "");
}

TEST(ChatSettings, CleanInputs) {
    EXPECT_EQ(cleanVideoId("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=5"), "dQw4w9WgXcQ");
    EXPECT_EQ(cleanVideoId("https://youtu.be/dQw4w9WgXcQ?si=1"), "dQw4w9WgXcQ");
    EXPECT_EQ(cleanVideoId("https://www.youtube.com/live/abcDEF12345"), "abcDEF12345");
    EXPECT_EQ(cleanVideoId("  dQw4w9WgXcQ "), "dQw4w9WgXcQ");
    EXPECT_EQ(cleanVideoId("bad id"), "");
    EXPECT_EQ(cleanTwitchChannel("#MeuCanal"), "meucanal");
    EXPECT_EQ(cleanTwitchChannel("https://twitch.tv/Fulano/"), "fulano");
    EXPECT_EQ(cleanTwitchChannel("a-b"), "");
    EXPECT_EQ(cleanGraphId("123_456"), "123_456");
    EXPECT_EQ(cleanGraphId("12 3;"), "");
    EXPECT_EQ(cleanClientId("x y"), "xy");
}

TEST(ChatSettings, SaveLoadFile) {
    const std::string path = tempPath("settings.json");
    ChatSettings s;
    s.twitchChannel = "canal";
    ASSERT_TRUE(s.save(path));
    ChatSettings r;
    ASSERT_TRUE(r.load(path));
    EXPECT_EQ(r.twitchChannel, "canal");
    std::remove(path.c_str());
    EXPECT_FALSE(r.load(path));
}
