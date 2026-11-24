#include <gtest/gtest.h>

#include "chat/sources.hpp"
#include "chat_test_util.hpp"

using namespace hb::chat;
using namespace hb::chat::test;

namespace {
std::string page(const std::string& id, const std::string& text, int poll = 5000, const std::string& extra = "") {
    return "{\"nextPageToken\":\"T_" + id + "\",\"pollingIntervalMillis\":" + std::to_string(poll) + extra +
           ",\"items\":[{\"id\":\"" + id + "\",\"snippet\":{\"type\":\"textMessageEvent\",\"publishedAt\":\"2024-05-01T12:00:00Z\","
           "\"displayMessage\":\"" + text + "\"},\"authorDetails\":{\"channelId\":\"U\",\"displayName\":\"Ana\"}}]}";
}
const char* kVideo = R"({"items":[{"liveStreamingDetails":{"activeLiveChatId":"CHAT"}}]})";
std::string gerr(int code, const std::string& reason) {
    return "{\"error\":{\"code\":" + std::to_string(code) + ",\"errors\":[{\"reason\":\"" + reason + "\"}]}}";
}
YouTubeConfig ytCfg(bool skip = false) {
    YouTubeConfig c;
    c.videoId = "abc123";
    c.skipBacklog = skip;
    return c;
}
}  // namespace

TEST(ChatBackoff, DoublesAndCaps) {
    Backoff b(Millis(1000), Millis(5000));
    EXPECT_EQ(b.onFailure().count(), 1000);
    EXPECT_EQ(b.onFailure().count(), 2000);
    EXPECT_EQ(b.onFailure().count(), 4000);
    EXPECT_EQ(b.onFailure().count(), 5000);
    EXPECT_EQ(b.onFailure().count(), 5000);
    b.onSuccess();
    EXPECT_EQ(b.onFailure().count(), 1000);
}

TEST(ChatYouTubeSource, HappyPathRespectsPollingIntervalAndQuota) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok, [] { return 1000; });
    s.configure(ytCfg());
    http.ok(200, kVideo);
    http.ok(200, page("m1", "oi", 7000));
    http.ok(200, page("m2", "tchau", 7000));
    s.connect();
    auto m = s.poll(at(0));
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].id, "m1");
    EXPECT_EQ(s.status().state, SourceState::Connected);
    EXPECT_EQ(s.status().quotaUnits, 1 + 5);
    EXPECT_TRUE(s.poll(at(6999)).empty());       // not due yet: no request made
    EXPECT_EQ(http.requests.size(), 2u);
    m = s.poll(at(7000));
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(s.status().quotaUnits, 1 + 5 + 5);
    // second list call carries the page token and the bearer token, and the chat id
    const auto& r = http.requests.back();
    EXPECT_NE(r.url.find("pageToken=T_m1"), std::string::npos);
    EXPECT_NE(r.url.find("liveChatId=CHAT"), std::string::npos);
    EXPECT_EQ(r.headers[0].second, "Bearer tok");
}

TEST(ChatYouTubeSource, PollingIntervalIsClamped) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok);
    YouTubeConfig c = ytCfg();
    c.liveChatId = "CHAT";
    s.configure(c);
    http.ok(200, page("m1", "a", 10));   // server asks 10 ms: we never go below 1 s
    http.ok(200, page("m2", "b", 10));
    s.connect();
    s.poll(at(0));
    EXPECT_TRUE(s.poll(at(900)).empty());
    EXPECT_EQ(s.poll(at(1000)).size(), 1u);
    EXPECT_EQ(http.requests.size(), 2u);   // liveChatId override: videos.list skipped
}

TEST(ChatYouTubeSource, SkipsBacklogOnFirstPage) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok);
    s.configure(ytCfg(true));
    http.ok(200, kVideo);
    http.ok(200, page("old", "antigo"));
    http.ok(200, page("new", "novo"));
    s.connect();
    EXPECT_TRUE(s.poll(at(0)).empty());
    auto m = s.poll(at(5000));
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].id, "new");
}

TEST(ChatYouTubeSource, DedupesRepeatedItems) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok);
    YouTubeConfig c = ytCfg();
    c.liveChatId = "CHAT";
    s.configure(c);
    http.ok(200, page("same", "x"));
    http.ok(200, page("same", "x"));
    s.connect();
    EXPECT_EQ(s.poll(at(0)).size(), 1u);
    EXPECT_TRUE(s.poll(at(5000)).empty());
}

TEST(ChatYouTubeSource, TransientErrorsBackOffExponentially) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok);
    YouTubeConfig c = ytCfg();
    c.liveChatId = "CHAT";
    s.configure(c);
    http.netFail("timeout");
    http.ok(500, "oops");
    http.ok(200, page("m", "ok"));
    s.connect();
    EXPECT_TRUE(s.poll(at(0)).empty());
    EXPECT_EQ(s.status().state, SourceState::Backoff);
    EXPECT_TRUE(s.poll(at(2999)).empty());       // base delay 3 s
    EXPECT_EQ(http.requests.size(), 1u);
    EXPECT_TRUE(s.poll(at(3000)).empty());       // second failure -> 6 s
    EXPECT_TRUE(s.poll(at(8999)).empty());
    EXPECT_EQ(http.requests.size(), 2u);
    EXPECT_EQ(s.poll(at(9000)).size(), 1u);
    EXPECT_EQ(s.status().state, SourceState::Connected);
}

TEST(ChatYouTubeSource, PermanentAndSpecialErrors) {
    {
        FakeHttp http; FakeTokens tok; YouTubeSource s(http, tok);
        YouTubeConfig c = ytCfg(); c.liveChatId = "CHAT"; s.configure(c);
        http.ok(403, gerr(403, "liveChatEnded"));
        s.connect(); s.poll(at(0));
        EXPECT_EQ(s.status().state, SourceState::Ended);
        EXPECT_TRUE(s.poll(at(60000)).empty());
        EXPECT_EQ(http.requests.size(), 1u);     // stopped polling
    }
    {
        FakeHttp http; FakeTokens tok; YouTubeSource s(http, tok);
        YouTubeConfig c = ytCfg(); c.liveChatId = "CHAT"; s.configure(c);
        http.ok(403, gerr(403, "liveChatDisabled"));
        s.connect(); s.poll(at(0));
        EXPECT_EQ(s.status().state, SourceState::Failed);
    }
    {
        FakeHttp http; FakeTokens tok; YouTubeSource s(http, tok);
        YouTubeConfig c = ytCfg(); c.liveChatId = "CHAT"; s.configure(c);
        http.ok(403, gerr(403, "quotaExceeded"));
        s.connect(); s.poll(at(0));
        EXPECT_EQ(s.status().state, SourceState::Backoff);
        EXPECT_NE(s.status().detail.find("Cota"), std::string::npos);
        EXPECT_TRUE(s.poll(at(60 * 1000)).empty());      // waits 30 min, no request
        EXPECT_EQ(http.requests.size(), 1u);
    }
    {
        FakeHttp http; FakeTokens tok; YouTubeSource s(http, tok);
        s.configure(ytCfg());
        http.ok(200, R"({"items":[{"liveStreamingDetails":{}}]})");   // not live yet
        s.connect(); s.poll(at(0));
        EXPECT_EQ(s.status().state, SourceState::Connecting);
        http.ok(200, kVideo);
        http.ok(200, page("m", "x"));
        EXPECT_TRUE(s.poll(at(19000)).empty());
        EXPECT_EQ(s.poll(at(20000)).size(), 1u);
    }
}

TEST(ChatYouTubeSource, AuthFailures) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok);
    s.configure(ytCfg());
    tok.fail = true;
    tok.authNeeded = true;
    s.connect();
    EXPECT_TRUE(s.poll(at(0)).empty());
    EXPECT_EQ(s.status().state, SourceState::NeedsAuth);
    EXPECT_TRUE(http.requests.empty());
    // 401 from the API invalidates the cached token and retries, eventually asking to re-authorize
    tok.fail = false;
    tok.authNeeded = false;
    YouTubeConfig c = ytCfg(); c.liveChatId = "CHAT"; s.configure(c);
    http.ok(401, gerr(401, "authError"));
    http.ok(401, gerr(401, "authError"));
    http.ok(401, gerr(401, "authError"));
    s.connect();
    s.poll(at(0));
    EXPECT_EQ(tok.invalidations, 1);
    s.poll(at(10000));
    s.poll(at(30000));
    EXPECT_EQ(s.status().state, SourceState::NeedsAuth);
}

TEST(ChatYouTubeSource, ReconfigureRestarts) {
    FakeHttp http;
    FakeTokens tok;
    YouTubeSource s(http, tok);
    YouTubeConfig c = ytCfg(); c.liveChatId = "A"; s.configure(c);
    http.ok(200, page("m1", "x"));
    s.connect();
    EXPECT_EQ(s.poll(at(0)).size(), 1u);
    c.liveChatId = "B";
    s.configure(c);
    http.ok(200, page("m2", "y"));
    EXPECT_EQ(s.poll(at(1)).size(), 1u);   // applied immediately, next poll is due right away
    EXPECT_NE(http.requests.back().url.find("liveChatId=B"), std::string::npos);
}

TEST(ChatFacebookSource, PollsReordersAndSkipsBacklog) {
    FakeHttp http;
    FacebookSource s(http, [] { return std::string("PAGETOKEN"); }, [] { return 5; });
    FacebookConfig c;
    c.liveVideoId = "12345";
    c.skipBacklog = true;
    s.configure(c);
    http.ok(200, R"({"data":[{"id":"2","message":"b"},{"id":"1","message":"a"}]})");
    http.ok(200, R"({"data":[{"id":"4","message":"d"},{"id":"3","message":"c"},{"id":"2","message":"b"}]})");
    s.connect();
    EXPECT_TRUE(s.poll(at(0)).empty());
    EXPECT_NE(http.requests[0].url.find("access_token=PAGETOKEN"), std::string::npos);
    EXPECT_NE(http.requests[0].url.find("/12345/comments"), std::string::npos);
    EXPECT_TRUE(s.poll(at(4999)).empty());
    auto m = s.poll(at(5000));
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[0].id, "3");   // oldest first
    EXPECT_EQ(m[1].id, "4");
    EXPECT_EQ(s.status().state, SourceState::Connected);
}

TEST(ChatFacebookSource, ErrorsMapToStates) {
    auto run = [](int status, const std::string& body) {
        FakeHttp http;
        FacebookSource s(http, [] { return std::string("T"); });
        FacebookConfig c; c.liveVideoId = "1"; s.configure(c);
        http.ok(status, body);
        s.connect();
        s.poll(at(0));
        return s.status();
    };
    EXPECT_EQ(run(400, R"({"error":{"code":190,"message":"expired"}})").state, SourceState::NeedsAuth);
    EXPECT_EQ(run(403, R"({"error":{"code":200,"message":"perm"}})").state, SourceState::Failed);
    EXPECT_EQ(run(400, R"({"error":{"code":613,"message":"rate"}})").state, SourceState::Backoff);
    EXPECT_EQ(run(500, "x").state, SourceState::Backoff);
    // token never leaks into status/error text
    FakeHttp http;
    FacebookSource s(http, [] { return std::string("SECRETTOKEN"); });
    FacebookConfig c; c.liveVideoId = "1"; s.configure(c);
    http.netFail("connect error access_token=SECRETTOKEN");
    s.connect();
    s.poll(at(0));
    EXPECT_EQ(s.status().lastError.find("SECRETTOKEN"), std::string::npos);
    // no token configured
    FacebookSource n(http, [] { return std::string(); });
    n.configure(c);
    n.connect();
    n.poll(at(0));
    EXPECT_EQ(n.status().state, SourceState::NeedsAuth);
}

TEST(ChatInstagramSource, UnavailableButAcceptsWebhookRelay) {
    InstagramWebhookSource s([] { return 9; });
    s.connect();
    EXPECT_EQ(s.status().state, SourceState::Unavailable);
    EXPECT_NE(s.status().detail.find("Indispon"), std::string::npos);
    EXPECT_EQ(s.feedWebhook("garbage"), 0);
    const char* body = R"({"entry":[{"changes":[{"field":"live_comments","value":{"id":"c1","text":"oi","from":{"id":"1","username":"u"}}}]}]})";
    EXPECT_EQ(s.feedWebhook(body), 1);
    auto m = s.poll(at(0));
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].text, "oi");
    EXPECT_EQ(s.status().state, SourceState::Unavailable);
}

TEST(ChatTwitchSource, ConnectsJoinsAndReadsMessages) {
    FakeSocket sock;
    TwitchSource s(sock, [] { return 123456; });
    TwitchConfig c;
    c.channel = "#MeuCanal";
    s.configure(c);
    s.connect();
    EXPECT_TRUE(s.poll(at(0)).empty());
    EXPECT_EQ(sock.connectedUrl, TwitchSource::kUrl);
    ASSERT_GE(sock.sent.size(), 4u);
    EXPECT_NE(sock.sent[0].find("CAP REQ"), std::string::npos);
    EXPECT_EQ(sock.sent[2].rfind("NICK justinfan", 0), 0u);
    EXPECT_EQ(sock.sent[3], "JOIN #meucanal");
    for (const auto& l : sock.sent) EXPECT_EQ(l.find("oauth:"), std::string::npos);   // anonymous: never a token
    sock.inbox.push_back(":tmi.twitch.tv 001 justinfan1 :Welcome\r\n:justinfan1.tmi.twitch.tv 366 justinfan1 #meucanal :End\r\n");
    sock.inbox.push_back("PING :tmi.twitch.tv");
    sock.inbox.push_back("@display-name=Ana;id=a1;user-id=1 :ana!ana@x PRIVMSG #meucanal :oi\r\n@display-name=Bia;id=b1 :bia!bia@x PRIVMSG #meucanal :tchau");
    auto m = s.poll(at(100));
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[0].author, "Ana");
    EXPECT_EQ(m[1].text, "tchau");
    EXPECT_EQ(s.status().state, SourceState::Connected);
    EXPECT_EQ(sock.sent.back(), "PONG :tmi.twitch.tv");
}

TEST(ChatTwitchSource, ReconnectsWithBackoffAndHandlesRECONNECT) {
    FakeSocket sock;
    TwitchSource s(sock);
    TwitchConfig c;
    c.channel = "canal";
    s.configure(c);
    sock.failConnect = true;
    s.connect();
    s.poll(at(0));
    EXPECT_EQ(s.status().state, SourceState::Backoff);
    s.poll(at(1000));
    EXPECT_EQ(sock.connects, 1);          // still inside the 2 s backoff
    sock.failConnect = false;
    s.poll(at(2000));
    EXPECT_EQ(sock.connects, 2);
    sock.inbox.push_back(":tmi.twitch.tv RECONNECT");
    s.poll(at(2100));
    EXPECT_FALSE(sock.open);
    s.poll(at(2500));
    EXPECT_EQ(sock.connects, 2);
    s.poll(at(3100));
    EXPECT_EQ(sock.connects, 3);
    sock.closeWhenEmpty = true;           // server drops us
    s.poll(at(3200));
    EXPECT_EQ(s.status().state, SourceState::Backoff);
}

TEST(ChatTwitchSource, InvalidChannelAndDisconnect) {
    FakeSocket sock;
    TwitchSource s(sock);
    TwitchConfig c;
    c.channel = "bad channel!";
    s.configure(c);
    s.connect();
    s.poll(at(0));
    EXPECT_EQ(s.status().state, SourceState::Failed);
    EXPECT_EQ(sock.connects, 0);
    EXPECT_TRUE(TwitchSource::validChannel("a_b9"));
    EXPECT_FALSE(TwitchSource::validChannel(""));
    EXPECT_FALSE(TwitchSource::validChannel("UPPER"));
    c.channel = "ok";
    s.configure(c);
    s.connect();
    s.poll(at(1));
    s.disconnect();
    EXPECT_EQ(s.status().state, SourceState::Disconnected);
    EXPECT_FALSE(sock.open);
}

TEST(ChatTwitchSource, SilentConnectionIsDropped) {
    FakeSocket sock;
    TwitchSource s(sock);
    TwitchConfig c;
    c.channel = "canal";
    s.configure(c);
    s.connect();
    s.poll(at(0));
    s.poll(at(60 * 1000));
    EXPECT_TRUE(sock.open);
    s.poll(at(8 * 60 * 1000));
    EXPECT_FALSE(sock.open);
    EXPECT_EQ(s.status().state, SourceState::Backoff);
}
