#include <gtest/gtest.h>

#include <cstdio>

#include "app/stream_destinations.hpp"

using namespace hb::app;

TEST(StreamDestinations, MaskSecretNeverReturnsTheFullKey) {
    EXPECT_EQ(maskSecret(""), "");
    EXPECT_EQ(maskSecret("ab"), "••••ab");
    EXPECT_EQ(maskSecret("live_abcdef1234"), "••••1234");
    EXPECT_EQ(maskSecret("1234"), "••••1234");
}

TEST(StreamDestinations, AddGetUpdateRemove) {
    StreamDestinations d;
    EXPECT_TRUE(d.list().empty());

    StreamDestination a;
    a.name = "YouTube principal";
    a.platform = "youtube";
    a.serverUrl = "rtmp://a.rtmp.youtube.com/live2";
    a.streamKey = "abcd-1234-efgh-5678";
    const std::string id = d.upsert(a);
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(d.list().size(), 1u);

    auto got = d.get(id);
    EXPECT_EQ(got.name, "YouTube principal");
    EXPECT_EQ(got.platform, "youtube");
    EXPECT_EQ(got.streamKey, "abcd-1234-efgh-5678");
    EXPECT_TRUE(got.enabled);

    // Updating: same id, new name - still one entry.
    got.name = "YouTube (ao vivo da manhã)";
    const std::string id2 = d.upsert(got);
    EXPECT_EQ(id2, id);
    EXPECT_EQ(d.list().size(), 1u);
    EXPECT_EQ(d.get(id).name, "YouTube (ao vivo da manhã)");

    // An id that doesn't exist is rejected, not silently added.
    StreamDestination ghost;
    ghost.id = "not-a-real-id";
    ghost.name = "x";
    ghost.serverUrl = "rtmp://x";
    EXPECT_TRUE(d.upsert(ghost).empty());
    EXPECT_EQ(d.list().size(), 1u);

    EXPECT_TRUE(d.setEnabled(id, false));
    EXPECT_FALSE(d.get(id).enabled);
    EXPECT_FALSE(d.setEnabled("nope", false));

    EXPECT_TRUE(d.remove(id));
    EXPECT_TRUE(d.list().empty());
    EXPECT_FALSE(d.remove(id));   // already gone
}

TEST(StreamDestinations, RejectsNamelessOrServerlessEntries) {
    StreamDestinations d;
    StreamDestination noName;
    noName.serverUrl = "rtmp://x";
    EXPECT_TRUE(d.upsert(noName).empty());

    StreamDestination noServer;
    noServer.name = "x";
    EXPECT_TRUE(d.upsert(noServer).empty());
    EXPECT_TRUE(d.list().empty());
}

TEST(StreamDestinations, CapsAtSixteenDestinations) {
    StreamDestinations d;
    for (int i = 0; i < StreamDestinations::kMax; ++i) {
        StreamDestination x;
        x.name = "Dest " + std::to_string(i);
        x.serverUrl = "rtmp://x/" + std::to_string(i);
        EXPECT_FALSE(d.upsert(x).empty());
    }
    EXPECT_EQ(d.list().size(), static_cast<std::size_t>(StreamDestinations::kMax));
    StreamDestination overflow;
    overflow.name = "one too many";
    overflow.serverUrl = "rtmp://x";
    EXPECT_TRUE(d.upsert(overflow).empty());
    EXPECT_EQ(d.list().size(), static_cast<std::size_t>(StreamDestinations::kMax));
}

TEST(StreamDestinations, SanitizeClampsAndWhitelists) {
    StreamDestinations d;
    StreamDestination x;
    x.name = "x";
    x.serverUrl = "rtmp://x";
    x.platform = "instagram";      // not an offered preset (no official third-party RTMP ingest)
    x.protocol = "udp";
    x.encoder = "magic";
    x.videoKbps = -5;
    x.audioKbps = 999999;
    x.width = 1;
    x.fpsNum = 0;
    const std::string id = d.upsert(x);
    ASSERT_FALSE(id.empty());
    const auto got = d.get(id);
    EXPECT_EQ(got.platform, "custom");
    EXPECT_EQ(got.protocol, "rtmp");
    EXPECT_EQ(got.encoder, "auto");
    EXPECT_GE(got.videoKbps, 300);
    EXPECT_LE(got.audioKbps, 512);
    EXPECT_GE(got.width, 160);
    EXPECT_GE(got.fpsNum, 1);
}

TEST(StreamDestinations, JsonRoundTripIncludesKeyAndUiJsonNeverDoes) {
    StreamDestinations d;
    StreamDestination x;
    x.name = "Twitch";
    x.platform = "twitch";
    x.serverUrl = "rtmp://live.twitch.tv/app";
    x.streamKey = "live_123456789_supersecret";
    x.videoKbps = 4500;
    const std::string id = d.upsert(x);
    ASSERT_FALSE(id.empty());

    const std::string saved = d.toJson();
    EXPECT_NE(saved.find("supersecret"), std::string::npos);   // the private store DOES keep the key

    StreamDestinations reloaded;
    ASSERT_TRUE(reloaded.fromJson(saved));
    EXPECT_EQ(reloaded.get(id).streamKey, "live_123456789_supersecret");
    EXPECT_EQ(reloaded.get(id).videoKbps, 4500);

    // New destinations after a reload still get fresh, non-colliding ids.
    StreamDestination y;
    y.name = "second";
    y.serverUrl = "rtmp://x";
    const std::string id2 = reloaded.upsert(y);
    EXPECT_NE(id2, id);
    EXPECT_FALSE(id2.empty());

    const std::string ui = d.uiJson();
    EXPECT_EQ(ui.find("supersecret"), std::string::npos);      // the UI message never does
    EXPECT_NE(ui.find("keyHint"), std::string::npos);
    EXPECT_NE(ui.find("\"keySet\":true"), std::string::npos);
}

TEST(StreamDestinations, HostileOrTruncatedFilesAreIgnoredEntryByEntry) {
    StreamDestinations d;
    EXPECT_FALSE(d.fromJson("not json"));
    EXPECT_FALSE(d.fromJson("{}"));
    EXPECT_TRUE(d.fromJson("{\"destinations\":[{\"id\":\"d1\",\"name\":\"\",\"serverUrl\":\"rtmp://x\"},"
                           "{\"id\":\"d2\",\"name\":\"ok\",\"serverUrl\":\"rtmp://y\"},\"garbage\",42]}"));
    EXPECT_EQ(d.list().size(), 1u);
    EXPECT_EQ(d.get("d2").name, "ok");
}

TEST(StreamDestinations, FilePersistence) {
    const std::string path = "xmit_dest_test.json";
    StreamDestinations a;
    StreamDestination x;
    x.name = "Kick";
    x.platform = "kick";
    x.serverUrl = "rtmps://fa723fc1b171.global-contribute.live-video.net";
    x.streamKey = "sk_live_abcdef";
    const std::string id = a.upsert(x);
    ASSERT_TRUE(a.save(path));

    StreamDestinations b;
    ASSERT_TRUE(b.load(path));
    EXPECT_EQ(b.get(id).name, "Kick");
    EXPECT_EQ(b.get(id).streamKey, "sk_live_abcdef");
    std::remove(path.c_str());
    EXPECT_FALSE(b.load("nonexistent.json"));
    EXPECT_EQ(b.get(id).name, "Kick");   // untouched when the file is missing
}
