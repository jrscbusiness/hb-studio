// Reconnect/backoff logic with a fake transport and a manual clock: no sockets, no sleeping.
#include <gtest/gtest.h>

#include <deque>

#include "streaming/reconnecting_publisher.hpp"

using namespace hb::streaming;

namespace {

struct Script {
    // One entry per open() attempt: result of open(); permanent flag; error text.
    struct Attempt { bool ok = true; bool permanent = false; std::string err = "falha"; };
    std::deque<Attempt> attempts;
    int opens = 0;
    int failNextSendsAfter = -1;                   // current transport: fail the Nth media send (counting from 0)
    std::vector<std::uint32_t> videoTimestamps;    // across all transports, as the "server" saw them
    std::vector<std::uint32_t> audioTimestamps;
    int stops = 0;
};

class FakeTransport final : public IStreamTransport {
public:
    explicit FakeTransport(Script& s) : s_(s) {}
    bool open(const std::string&, const std::string&, int) override {
        ++s_.opens;
        Script::Attempt a;
        if (!s_.attempts.empty()) {
            a = s_.attempts.front();
            s_.attempts.pop_front();
        }
        err_ = a.ok ? "" : a.err;
        permanent_ = a.permanent;
        publishing_ = a.ok;
        sends_ = 0;
        return a.ok;
    }
    void stop() override { publishing_ = false; ++s_.stops; }
    bool isPublishing() const override { return publishing_; }
    std::string lastError() const override { return err_; }
    bool permanentFailure() const override { return permanent_; }
    bool sendVideo(std::uint32_t ts, const std::vector<std::uint8_t>&) override { return media(ts, true); }
    bool sendAudio(std::uint32_t ts, const std::vector<std::uint8_t>&) override { return media(ts, false); }

private:
    bool media(std::uint32_t ts, bool video) {
        if (!publishing_) return false;
        if (s_.failNextSendsAfter >= 0 && sends_ >= s_.failNextSendsAfter) {
            publishing_ = false;
            err_ = "write failed";
            return false;
        }
        ++sends_;
        (video ? s_.videoTimestamps : s_.audioTimestamps).push_back(ts);
        return true;
    }
    Script& s_;
    bool publishing_ = false, permanent_ = false;
    std::string err_;
    int sends_ = 0;
};

struct Fixture {
    Script script;
    std::int64_t now = 0;
    StreamStats stats{[this] { return now; }};
    ReconnectingPublisher::Options opts;
    std::unique_ptr<ReconnectingPublisher> pub;

    explicit Fixture(int maxAttempts = 20) {
        opts.url = "rtmps://example/app";
        opts.streamKey = "k";
        opts.backoff.maxAttempts = maxAttempts;
        opts.backoff.jitterPercent = 0;   // deterministic delays
        pub = std::make_unique<ReconnectingPublisher>(
            opts, [this]() -> std::unique_ptr<IStreamTransport> { return std::make_unique<FakeTransport>(script); }, &stats,
            [this] { return now; });
    }
};

const std::vector<std::uint8_t> kBody{1, 2, 3};

}  // namespace

TEST(ReconnectBackoff, ExponentialCappedWithJitterBoundsAndExhaustion) {
    BackoffConfig c;
    c.jitterPercent = 0;
    c.maxAttempts = 8;
    ReconnectBackoff b(c);
    EXPECT_EQ(b.nextDelayMs(), 1000);
    EXPECT_EQ(b.nextDelayMs(), 2000);
    EXPECT_EQ(b.nextDelayMs(), 4000);
    EXPECT_EQ(b.nextDelayMs(), 8000);
    EXPECT_EQ(b.nextDelayMs(), 16000);
    EXPECT_EQ(b.nextDelayMs(), 30000);   // capped
    EXPECT_EQ(b.nextDelayMs(), 30000);
    EXPECT_EQ(b.nextDelayMs(), 30000);
    EXPECT_EQ(b.nextDelayMs(), -1);      // exhausted
    b.reset();
    EXPECT_EQ(b.nextDelayMs(), 1000);

    BackoffConfig j;   // 20% jitter: random source at both extremes
    ReconnectBackoff lo(j, [] { return 0.0; }), hi(j, [] { return 1.0; });
    EXPECT_EQ(lo.nextDelayMs(), 800);
    EXPECT_EQ(hi.nextDelayMs(), 1200);
}

TEST(ReconnectingPublisher, ConnectsImmediatelyOnFirstPoll) {
    Fixture f;
    EXPECT_TRUE(f.pub->poll());
    EXPECT_TRUE(f.pub->connected());
    EXPECT_EQ(f.pub->generation(), 1u);
    EXPECT_EQ(f.stats.snapshot().reconnects, 0u);   // first connect is not a "re"connect
}

TEST(ReconnectingPublisher, FailedAttemptsBackOffExponentiallyThenSucceed) {
    Fixture f;
    f.script.attempts = {{false, false, "sem rede"}, {false, false, "sem rede"}, {true, false, ""}};
    EXPECT_FALSE(f.pub->poll());                       // attempt 1 fails -> wait 1 s
    EXPECT_EQ(f.pub->state(), ReconnectingPublisher::State::Waiting);
    EXPECT_EQ(f.pub->msUntilRetry(), 1000);
    EXPECT_EQ(f.pub->lastError(), "sem rede");
    EXPECT_EQ(f.stats.snapshot().lastError, "sem rede");

    f.now += 999;
    EXPECT_FALSE(f.pub->poll());                       // too early: no new attempt
    EXPECT_EQ(f.script.opens, 1);
    f.now += 1;
    EXPECT_FALSE(f.pub->poll());                       // attempt 2 fails -> wait 2 s
    EXPECT_EQ(f.script.opens, 2);
    EXPECT_EQ(f.pub->msUntilRetry(), 2000);
    f.now += 2000;
    EXPECT_TRUE(f.pub->poll());                        // attempt 3 succeeds
    EXPECT_EQ(f.script.opens, 3);
    EXPECT_EQ(f.pub->generation(), 1u);
}

TEST(ReconnectingPublisher, PermanentFailureGivesUpWithoutRetrying) {
    Fixture f;
    f.script.attempts = {{false, true, "o servidor recusou a publicação (NetStream.Publish.BadName)."}};
    EXPECT_FALSE(f.pub->poll());
    EXPECT_TRUE(f.pub->gaveUp());
    f.now += 1'000'000;
    EXPECT_FALSE(f.pub->poll());
    EXPECT_EQ(f.script.opens, 1);
    EXPECT_NE(f.pub->lastError().find("recusou"), std::string::npos);
}

TEST(ReconnectingPublisher, GivesUpAfterMaxAttempts) {
    Fixture f(3);
    for (int i = 0; i < 10; ++i) f.script.attempts.push_back({false, false, "x"});
    for (int i = 0; i < 20 && !f.pub->gaveUp(); ++i) {
        f.pub->poll();
        f.now += 60'000;
    }
    EXPECT_TRUE(f.pub->gaveUp());
    EXPECT_EQ(f.script.opens, 4);   // 1 initial + 3 retried (maxAttempts=3 delays)
    EXPECT_NE(f.pub->lastError().find("Desistindo"), std::string::npos);
}

TEST(ReconnectingPublisher, WriteFailureTriggersReconnectNewGenerationAndRebasedTimestamps) {
    Fixture f;
    ASSERT_TRUE(f.pub->poll());
    ASSERT_TRUE(f.pub->sendVideo(5000, kBody));        // first ts becomes the base -> 0
    ASSERT_TRUE(f.pub->sendAudio(5020, kBody));
    ASSERT_TRUE(f.pub->sendVideo(5033, kBody));
    EXPECT_EQ(f.script.videoTimestamps, (std::vector<std::uint32_t>{0, 33}));
    EXPECT_EQ(f.script.audioTimestamps, (std::vector<std::uint32_t>{20}));

    f.script.failNextSendsAfter = 0;                   // the current transport dies on its next write
    EXPECT_FALSE(f.pub->sendVideo(5066, kBody));
    EXPECT_EQ(f.pub->state(), ReconnectingPublisher::State::Waiting);
    EXPECT_FALSE(f.pub->sendAudio(5070, kBody));       // not connected: refused, no crash
    EXPECT_NE(f.pub->lastError().find("write failed"), std::string::npos);

    f.script.failNextSendsAfter = -1;
    f.now += 1000;                                     // backoff elapsed
    ASSERT_TRUE(f.pub->poll());
    EXPECT_EQ(f.pub->generation(), 2u);
    EXPECT_EQ(f.stats.snapshot().reconnects, 1u);
    ASSERT_TRUE(f.pub->sendVideo(9000, kBody));        // new session: timestamps start from 0 again
    ASSERT_TRUE(f.pub->sendVideo(9033, kBody));
    EXPECT_EQ(f.script.videoTimestamps, (std::vector<std::uint32_t>{0, 33, 0, 33}));
}

TEST(ReconnectingPublisher, StableConnectionResetsBackoffButFlappingDoesNot) {
    Fixture f;
    // lose the first connection quickly, reconnect, lose again quickly: delays keep growing
    ASSERT_TRUE(f.pub->poll());
    f.script.failNextSendsAfter = 0;
    f.pub->sendVideo(0, kBody);                        // lost #1 -> 1 s
    EXPECT_EQ(f.pub->msUntilRetry(), 1000);
    f.now += 1000;
    f.script.failNextSendsAfter = -1;
    ASSERT_TRUE(f.pub->poll());
    f.script.failNextSendsAfter = 0;
    f.pub->sendVideo(0, kBody);                        // lost #2 right away -> 2 s (not reset)
    EXPECT_EQ(f.pub->msUntilRetry(), 2000);
    f.now += 2000;
    f.script.failNextSendsAfter = -1;
    ASSERT_TRUE(f.pub->poll());
    // stay up longer than stableAfterMs: backoff resets
    f.now += f.opts.stableAfterMs + 1;
    EXPECT_TRUE(f.pub->poll());
    EXPECT_EQ(f.pub->attempts(), 0);
    f.script.failNextSendsAfter = 0;
    f.pub->sendVideo(0, kBody);
    EXPECT_EQ(f.pub->msUntilRetry(), 1000);            // back to the first delay
}

TEST(ReconnectingPublisher, TransportThatStopsPublishingIsTreatedAsLost) {
    Fixture f;
    ASSERT_TRUE(f.pub->poll());
    f.script.failNextSendsAfter = 1;
    EXPECT_TRUE(f.pub->sendVideo(0, kBody));
    EXPECT_FALSE(f.pub->sendVideo(33, kBody));
    EXPECT_EQ(f.pub->state(), ReconnectingPublisher::State::Waiting);
}

TEST(ReconnectingPublisher, StopTearsDownAndNullFactoryGivesUp) {
    Fixture f;
    ASSERT_TRUE(f.pub->poll());
    f.pub->stop();
    EXPECT_FALSE(f.pub->connected());
    EXPECT_EQ(f.script.stops, 1);

    ReconnectingPublisher none(f.opts, [] { return std::unique_ptr<IStreamTransport>(); });
    EXPECT_FALSE(none.poll());
    EXPECT_TRUE(none.gaveUp());
}

TEST(ReconnectingPublisher, CountsSentBytesInStats) {
    Fixture f;
    ASSERT_TRUE(f.pub->poll());
    ASSERT_TRUE(f.pub->sendVideo(0, kBody));
    ASSERT_TRUE(f.pub->sendAudio(0, kBody));
    EXPECT_EQ(f.stats.snapshot().sentBytes, 6u);
}
