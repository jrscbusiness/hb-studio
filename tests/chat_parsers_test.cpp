#include <gtest/gtest.h>

#include "chat/irc.hpp"
#include "chat/parsers.hpp"
#include "chat/types.hpp"

using namespace hb::chat;

TEST(ChatTypes, Iso8601) {
    std::int64_t t = 0;
    ASSERT_TRUE(parseIso8601Ms("1970-01-01T00:00:00Z", t));
    EXPECT_EQ(t, 0);
    ASSERT_TRUE(parseIso8601Ms("2024-05-01T12:34:56.789Z", t));
    EXPECT_EQ(t, 1714566896789LL);
    ASSERT_TRUE(parseIso8601Ms("2024-05-01T12:34:56+0000", t));
    EXPECT_EQ(t, 1714566896000LL);
    ASSERT_TRUE(parseIso8601Ms("2024-05-01T09:34:56-03:00", t));
    EXPECT_EQ(t, 1714566896000LL);
    ASSERT_TRUE(parseIso8601Ms("2024-05-01T12:34:56.5Z", t));
    EXPECT_EQ(t, 1714566896500LL);
    EXPECT_FALSE(parseIso8601Ms("garbage", t));
    EXPECT_FALSE(parseIso8601Ms("2024-13-01T00:00:00Z", t));
    EXPECT_FALSE(parseIso8601Ms("2024-05-01T12:34:56Zjunk", t));
}

TEST(ChatTypes, SanitizeAndFilters) {
    EXPECT_EQ(sanitizeText("a\nb\tc\x01" "d", 100), "a b cd");
    EXPECT_EQ(sanitizeText("ok\xff" "x", 100), "ok?x");
    EXPECT_EQ(sanitizeText("\xC3\xA9\xC3\xA9\xC3\xA9", 5), "\xC3\xA9\xC3\xA9");   // never cut inside a code point
    EXPECT_EQ(sanitizeText("\xE2\x82", 100), "??");                                // truncated sequence
    EXPECT_TRUE(looksLikeCommand("!sorteio"));
    EXPECT_TRUE(looksLikeCommand("  !uptime now"));
    EXPECT_FALSE(looksLikeCommand("! nao"));
    EXPECT_FALSE(looksLikeCommand("oi!"));
    EXPECT_TRUE(isKnownBot("NightBot"));
    EXPECT_FALSE(isKnownBot("maria"));
    EXPECT_EQ(maskTail(""), "");
    EXPECT_EQ(maskTail("abcdef1234").substr(12), "1234");
    Platform p;
    EXPECT_TRUE(parsePlatform("twitch", p));
    EXPECT_EQ(p, Platform::Twitch);
    EXPECT_FALSE(parsePlatform("tiktok", p));
}

TEST(ChatIrc, ParsesTagsPrefixCommandParams) {
    IrcMessage m;
    ASSERT_TRUE(parseIrcLine(
        "@badge-info=subscriber/8;badges=moderator/1,subscriber/6;display-name=Ana\\sLima;emotes=25:0-4,12-16/1902:6-10;"
        "id=abc-123;mod=1;tmi-sent-ts=1700000000123;user-id=42 :ana!ana@ana.tmi.twitch.tv PRIVMSG #canal :Kappa Keepo Kappa hi\r\n",
        m));
    EXPECT_EQ(m.command, "PRIVMSG");
    EXPECT_EQ(m.prefix, "ana!ana@ana.tmi.twitch.tv");
    ASSERT_EQ(m.params.size(), 2u);
    EXPECT_EQ(m.params[0], "#canal");
    EXPECT_EQ(m.params[1], "Kappa Keepo Kappa hi");
    EXPECT_EQ(m.tags["display-name"], "Ana Lima");
    auto c = twitchToChatMessage(m, 5);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->author, "Ana Lima");
    EXPECT_EQ(c->id, "abc-123");
    EXPECT_EQ(c->authorId, "42");
    EXPECT_EQ(c->tsMs, 1700000000123LL);
    EXPECT_TRUE(c->badges & kBadgeMod);
    EXPECT_TRUE(c->badges & kBadgeSub);
    EXPECT_FALSE(c->badges & kBadgeOwner);
    ASSERT_EQ(c->emotes.size(), 3u);
    EXPECT_EQ(c->emotes[0].id, "25");
    EXPECT_EQ(c->emotes[1].start, 12);
    EXPECT_EQ(c->emotes[2].id, "1902");
}

TEST(ChatIrc, SimpleLinesAndEdgeCases) {
    IrcMessage m;
    ASSERT_TRUE(parseIrcLine("PING :tmi.twitch.tv", m));
    EXPECT_EQ(m.command, "PING");
    EXPECT_EQ(m.params.back(), "tmi.twitch.tv");
    ASSERT_TRUE(parseIrcLine(":tmi.twitch.tv 001 justinfan1 :Welcome, GLHF!", m));
    EXPECT_EQ(m.command, "001");
    ASSERT_TRUE(parseIrcLine("privmsg #c :x", m));
    EXPECT_EQ(m.command, "PRIVMSG");
    EXPECT_FALSE(parseIrcLine("", m));
    EXPECT_FALSE(parseIrcLine("@only=tags", m));
    EXPECT_FALSE(parseIrcLine(std::string(10000, 'a'), m));
    // valueless tag and trailing backslash
    ASSERT_TRUE(parseIrcLine("@flag;x=a\\ PING :z", m));
    EXPECT_TRUE(m.tags.count("flag"));
    EXPECT_EQ(unescapeIrcTag("a\\:b\\\\c\\n"), "a;b\\c\n");
    auto lines = splitIrcLines("A\r\nB\r\n\r\nC");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[2], "C");
}

TEST(ChatIrc, ActionAndMissingDisplayName) {
    IrcMessage m;
    ASSERT_TRUE(parseIrcLine(":bob!bob@bob.tmi.twitch.tv PRIVMSG #c :\x01" "ACTION dances\x01", m));
    auto c = twitchToChatMessage(m, 77);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->text, "dances");
    EXPECT_EQ(c->author, "bob");
    EXPECT_EQ(c->tsMs, 77);
    IrcMessage j;
    ASSERT_TRUE(parseIrcLine(":tmi.twitch.tv JOIN #c", j));
    EXPECT_FALSE(twitchToChatMessage(j, 0).has_value());
}

TEST(ChatIrc, EmotesAndBadgesMalformed) {
    EXPECT_TRUE(parseTwitchEmotes("::,-/x:1-,/y:b-2/z:3-1").empty());
    EXPECT_EQ(parseTwitchBadges("broadcaster/1,verified/1,bogus/9"), kBadgeOwner | kBadgeVerified);
}

static const char* kYtPage = R"({
 "kind":"youtube#liveChatMessageListResponse","nextPageToken":"NEXT","pollingIntervalMillis":5200,
 "items":[
  {"id":"m1","snippet":{"type":"textMessageEvent","publishedAt":"2024-05-01T12:34:56.789Z","displayMessage":"Olá mundo",
    "textMessageDetails":{"messageText":"Olá mundo"}},
   "authorDetails":{"channelId":"UC1","displayName":"Joana","profileImageUrl":"https://yt3.ggpht.com/a","isChatOwner":false,"isChatModerator":true,"isChatSponsor":true,"isVerified":false}},
  {"id":"m2","snippet":{"type":"superChatEvent","publishedAt":"2024-05-01T12:35:00Z","superChatDetails":{"amountDisplayString":"R$ 10,00","userComment":"força!"}},
   "authorDetails":{"channelId":"UC2","displayName":"Rico","profileImageUrl":"http://insecure/x"}},
  {"id":"m3","snippet":{"type":"messageDeletedEvent","publishedAt":"2024-05-01T12:35:01Z"},"authorDetails":{"channelId":"UC3","displayName":"x"}},
  {"id":"m4","snippet":{"type":"newSponsorEvent","publishedAt":"2024-05-01T12:35:02Z"},"authorDetails":{"channelId":"UC4","displayName":"Novo"}},
  {"snippet":{"type":"textMessageEvent"}},
  {"id":"m5","snippet":{"type":"textMessageEvent","displayMessage":""},"authorDetails":{"channelId":"UC5","displayName":"Vazio"}}
 ]})";

TEST(ChatYouTube, ParsesPage) {
    YouTubeChatPage p;
    ASSERT_TRUE(parseYouTubeChatPage(kYtPage, 9, p));
    EXPECT_EQ(p.nextPageToken, "NEXT");
    EXPECT_EQ(p.pollingIntervalMillis, 5200);
    EXPECT_FALSE(p.offline);
    ASSERT_EQ(p.messages.size(), 3u);
    EXPECT_EQ(p.messages[0].author, "Joana");
    EXPECT_EQ(p.messages[0].text, "Olá mundo");
    EXPECT_EQ(p.messages[0].tsMs, 1714566896789LL);
    EXPECT_TRUE(p.messages[0].badges & kBadgeMod);
    EXPECT_TRUE(p.messages[0].badges & kBadgeMember);
    EXPECT_EQ(p.messages[0].avatarUrl, "https://yt3.ggpht.com/a");
    EXPECT_EQ(p.messages[1].text, "[Super Chat R$ 10,00] força!");
    EXPECT_EQ(p.messages[1].avatarUrl, "");   // non-https avatar is dropped
    EXPECT_EQ(p.messages[2].text, "[Novo membro do canal]");
    EXPECT_EQ(p.skippedEvents, 3);
}

TEST(ChatYouTube, OfflineAndBadInput) {
    YouTubeChatPage p;
    ASSERT_TRUE(parseYouTubeChatPage(R"({"items":[],"offlineAt":"2024-05-01T13:00:00Z"})", 0, p));
    EXPECT_TRUE(p.offline);
    EXPECT_FALSE(parseYouTubeChatPage("not json", 0, p));
    EXPECT_FALSE(parseYouTubeChatPage("{}", 0, p));
    EXPECT_FALSE(parseYouTubeChatPage("[]", 0, p));
}

TEST(ChatYouTube, ChatIdAndErrors) {
    std::string id;
    ASSERT_TRUE(parseYouTubeActiveChatId(R"({"items":[{"liveStreamingDetails":{"activeLiveChatId":"CHAT1"}}]})", id));
    EXPECT_EQ(id, "CHAT1");
    ASSERT_TRUE(parseYouTubeActiveChatId(R"({"items":[]})", id));
    EXPECT_EQ(id, "");
    ASSERT_TRUE(parseYouTubeActiveChatId(R"({"items":[{"liveStreamingDetails":{}}]})", id));
    EXPECT_EQ(id, "");
    ApiError e;
    ASSERT_TRUE(parseGoogleError(R"({"error":{"code":403,"message":"The live chat is no longer live.","errors":[{"reason":"liveChatEnded"}]}})", e));
    EXPECT_EQ(e.code, 403);
    EXPECT_EQ(e.reason, "liveChatEnded");
    EXPECT_FALSE(parseGoogleError("<html>", e));
}

TEST(ChatFacebook, ParsesComments) {
    const char* json = R"({"data":[
      {"id":"1_9","message":"Oi!","created_time":"2024-05-01T12:00:00+0000","from":{"id":"77","name":"Carlos","picture":{"data":{"url":"https://scontent/x.jpg"}}}},
      {"id":"1_8","message":"sem autor","created_time":"2024-05-01T11:59:00+0000"},
      {"id":"1_7","created_time":"2024-05-01T11:58:00+0000","from":{"id":"1","name":"Foto"}}
    ],"paging":{}})";
    GraphCommentsPage p;
    ASSERT_TRUE(parseFacebookComments(json, 5, p));
    ASSERT_EQ(p.messages.size(), 2u);
    EXPECT_EQ(p.messages[0].author, "Carlos");
    EXPECT_EQ(p.messages[0].authorId, "77");
    EXPECT_EQ(p.messages[0].tsMs, 1714564800000LL);
    EXPECT_EQ(p.messages[1].author, "Usu\xC3\xA1rio do Facebook");
    GraphError ge;
    ASSERT_TRUE(parseGraphError(R"({"error":{"message":"Error validating access token","type":"OAuthException","code":190,"error_subcode":463}})", ge));
    EXPECT_EQ(ge.code, 190);
    EXPECT_EQ(ge.subcode, 463);
    EXPECT_FALSE(parseFacebookComments("{\"error\":{}}", 0, p));
}

TEST(ChatInstagram, WebhookPayload) {
    const char* json = R"({"object":"instagram","entry":[{"id":"IG1","time":1,"changes":[
      {"field":"live_comments","value":{"id":"c1","text":"salve","from":{"id":"u1","username":"fulano"},"media":{"id":"m1"}}},
      {"field":"comments","value":{"id":"c2","text":"nao","from":{"id":"u2","username":"x"}}}]}]})";
    std::vector<ChatMessage> out;
    ASSERT_TRUE(parseInstagramLiveWebhook(json, 3, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].author, "fulano");
    EXPECT_EQ(out[0].platform, Platform::Instagram);
    EXPECT_FALSE(parseInstagramLiveWebhook("{}", 0, out));
}
