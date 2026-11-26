#include <gtest/gtest.h>

#include <algorithm>

#include "net/body_reader.hpp"
#include "net/http.hpp"
#include "net/http_fake.hpp"

using namespace hb::net;

namespace {
HttpRequest get(const std::string& url = "https://example.com/x") {
    HttpRequest r;
    r.url = url;
    return r;
}
}  // namespace

TEST(NetHttp, ValidationRefusesBadRequests) {
    FakeHttpClient c;
    c.enqueue({});
    EXPECT_EQ(c.send(get("ftp://x/")).error, ErrorKind::InvalidRequest);
    HttpRequest r = get("http://example.com/");
    r.httpsOnly = true;
    EXPECT_EQ(c.send(r).error, ErrorKind::InvalidRequest);
    r = get();
    r.headers = {{"X-Evil", "a\r\nInjected: 1"}};
    EXPECT_EQ(c.send(r).error, ErrorKind::InvalidRequest);
    r = get();
    r.method.clear();
    EXPECT_EQ(c.send(r).error, ErrorKind::InvalidRequest);
    EXPECT_EQ(c.requestCount(), 0u);  // nothing refused ever reaches the transport
    EXPECT_TRUE(c.send(get()).ok());
}

TEST(NetHttp, BufferedBodyAndHeaders) {
    FakeHttpClient c;
    FakeHttpClient::Reply rep;
    rep.status = 429;
    rep.headers = {{"Retry-After", "7"}, {"Content-Length", "5"}};
    rep.body = "hello";
    c.enqueue(rep);
    const HttpResponse r = c.send(get());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(r.body, "hello");
    EXPECT_EQ(r.header("retry-after"), "7");
    EXPECT_EQ(r.header("missing"), "");
    EXPECT_EQ(r.contentLength, 5u);
}

TEST(NetHttp, ScriptedTransportFailure) {
    FakeHttpClient c;
    FakeHttpClient::Reply rep;
    rep.error = ErrorKind::Timeout;
    c.enqueue(rep);
    const HttpResponse r = c.send(get());
    EXPECT_EQ(r.error, ErrorKind::Timeout);
    EXPECT_EQ(r.status, 0);
}

TEST(NetHttp, SizeCapFailsOrTruncates) {
    FakeHttpClient c;
    FakeHttpClient::Reply rep;
    rep.body = std::string(100, 'a');
    rep.chunk = 30;
    c.enqueue(rep);
    c.enqueue(rep);
    HttpRequest r = get();
    r.maxResponseBytes = 50;
    HttpResponse failed = c.send(r);
    EXPECT_EQ(failed.error, ErrorKind::TooLarge);
    r.overflow = Overflow::Truncate;
    HttpResponse cut = c.send(r);
    EXPECT_TRUE(cut.ok());
    EXPECT_EQ(cut.body.size(), 50u);
}

TEST(NetHttp, SinkOnlyForSuccessStatuses) {
    FakeHttpClient c;
    FakeHttpClient::Reply ok;
    ok.body = "abcdef";
    ok.chunk = 2;
    FakeHttpClient::Reply bad;
    bad.status = 500;
    bad.body = "oops";
    c.enqueue(ok);
    c.enqueue(bad);
    std::string streamed;
    HttpRequest r = get();
    r.onData = [&](const char* d, std::size_t n, std::uint64_t) {
        streamed.append(d, n);
        return true;
    };
    HttpResponse a = c.send(r);
    EXPECT_TRUE(a.ok());
    EXPECT_EQ(streamed, "abcdef");
    EXPECT_TRUE(a.body.empty());
    EXPECT_EQ(a.bytesReceived, 6u);
    streamed.clear();
    HttpResponse b = c.send(r);
    EXPECT_EQ(b.status, 500);
    EXPECT_EQ(b.body, "oops");  // error bodies are accumulated, not streamed
    EXPECT_TRUE(streamed.empty());
}

TEST(NetHttp, SinkCanSkipErrorBodies) {
    FakeHttpClient c;
    FakeHttpClient::Reply bad;
    bad.status = 404;
    bad.body = "not found page";
    c.enqueue(bad);
    HttpRequest r = get();
    r.onData = [](const char*, std::size_t, std::uint64_t) { return true; };
    r.readBodyOutsideSink = false;
    const HttpResponse resp = c.send(r);
    EXPECT_EQ(resp.status, 404);
    EXPECT_TRUE(resp.body.empty());
    EXPECT_EQ(resp.bytesReceived, 0u);
}

TEST(NetHttp, SinkAbortAndProgressCancel) {
    FakeHttpClient c;
    FakeHttpClient::Reply rep;
    rep.body = std::string(10, 'x');
    rep.chunk = 2;
    c.enqueue(rep);
    c.enqueue(rep);
    HttpRequest r = get();
    int chunks = 0;
    r.onData = [&](const char*, std::size_t, std::uint64_t) { return ++chunks < 3; };
    EXPECT_EQ(c.send(r).error, ErrorKind::Cancelled);
    EXPECT_EQ(chunks, 3);

    HttpRequest p = get();
    std::uint64_t last = 0;
    p.onProgress = [&](std::uint64_t got, std::uint64_t) {
        last = got;
        return got < 4;
    };
    EXPECT_EQ(c.send(p).error, ErrorKind::Cancelled);
    EXPECT_EQ(last, 4u);
}

TEST(NetHttp, StreamingUploadThroughBodySource) {
    FakeHttpClient c;
    c.enqueue({});
    HttpRequest r = get();
    r.method = "PUT";
    const std::string payload(100, 'z');
    r.bodySourceLength = payload.size();
    r.bodySourceChunk = 32;
    std::size_t off = 0;
    r.bodySource = [&](char* buf, std::size_t cap) {
        const std::size_t n = std::min(cap, payload.size() - off);
        std::copy(payload.begin() + static_cast<long>(off), payload.begin() + static_cast<long>(off + n), buf);
        off += n;
        return n;
    };
    ASSERT_TRUE(c.send(r).ok());
    ASSERT_EQ(c.uploads().size(), 1u);
    EXPECT_EQ(c.uploads()[0], payload);
}

TEST(NetHttp, BodySourceAbortCancelsUpload) {
    FakeHttpClient c;
    c.enqueue({});
    HttpRequest r = get();
    r.method = "PUT";
    r.bodySourceLength = 10;
    r.bodySource = [](char*, std::size_t) { return kBodyAbort; };
    EXPECT_EQ(c.send(r).error, ErrorKind::Cancelled);
}

TEST(NetHttp, HandlerAnswersWhenQueueIsEmpty) {
    FakeHttpClient c;
    c.setHandler([](const HttpRequest& r) {
        FakeHttpClient::Reply rep;
        rep.body = r.url;
        return rep;
    });
    EXPECT_EQ(c.send(get("https://a.example/q")).body, "https://a.example/q");
}

TEST(NetHttp, UnscriptedFakeFailsLoudly) {
    FakeHttpClient c;
    EXPECT_EQ(c.send(get()).error, ErrorKind::Transport);
}

#ifndef _WIN32
TEST(NetHttp, PortableBuildHasUnavailableClient) {
    auto c = makeWinHttpClient();
    ASSERT_TRUE(c);
    EXPECT_EQ(c->send(get()).error, ErrorKind::Transport);
}
#endif
