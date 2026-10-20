#include <gtest/gtest.h>

#include <random>

#include "rtsp/base64.hpp"
#include "rtsp/md5.hpp"
#include "rtsp/message.hpp"
#include "rtsp/rtp.hpp"
#include "rtsp/url.hpp"

using namespace hb::rtsp;

namespace {
Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
}  // namespace

// ---- MD5: test suite from RFC 1321 appendix A.5
TEST(Md5, Rfc1321Vectors) {
    EXPECT_EQ(md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(md5Hex("a"), "0cc175b9c0f1b6a831c399e269772661");
    EXPECT_EQ(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(md5Hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(md5Hex("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b");
    EXPECT_EQ(md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
              "d174ab98d277d9f5a5611c2c9f419d9f");
    EXPECT_EQ(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
              "57edf4a22be3c955ac49da2e2107b67a");
}

TEST(Md5, PaddingBoundaries) {
    // 55/56/63/64/65 byte inputs exercise the one-block / two-block padding paths.
    EXPECT_EQ(md5Hex(std::string(55, 'a')), "ef1772b6dff9a122358552954ad0df65");
    EXPECT_EQ(md5Hex(std::string(56, 'a')), "3b0c8ac703f828b04c6c197006d17218");
    EXPECT_EQ(md5Hex(std::string(63, 'a')), "b06521f39153d618550606be297466d5");
    EXPECT_EQ(md5Hex(std::string(64, 'a')), "014842d480b571495a4a0363793f7367");
    EXPECT_EQ(md5Hex(std::string(65, 'a')), "c743a45e0d2e6a95cb859adae0248435");
}

TEST(Base64, RoundTripAndTolerance) {
    EXPECT_EQ(base64Encode(B("")), "");
    EXPECT_EQ(base64Encode(B("f")), "Zg==");
    EXPECT_EQ(base64Encode(B("fo")), "Zm8=");
    EXPECT_EQ(base64Encode(B("foo")), "Zm9v");
    EXPECT_EQ(base64Encode(B("foobar")), "Zm9vYmFy");
    EXPECT_EQ(*base64Decode("Zm9vYmFy"), B("foobar"));
    EXPECT_EQ(*base64Decode("Zm8"), B("fo"));          // missing padding
    EXPECT_EQ(*base64Decode("Zm9v\r\nYmFy"), B("foobar"));  // whitespace
    EXPECT_FALSE(base64Decode("Zm9v!").has_value());
}

TEST(Url, ParseVariants) {
    auto u = parseRtspUrl("rtsp://admin:p%40ss@192.168.1.10:8554/live/ch0?x=1");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->host, "192.168.1.10");
    EXPECT_EQ(u->port, 8554);
    EXPECT_EQ(u->pathAndQuery, "/live/ch0?x=1");
    EXPECT_EQ(u->userinfo.username, "admin");
    EXPECT_EQ(u->userinfo.password, "p@ss");
    EXPECT_EQ(u->requestUri(), "rtsp://192.168.1.10:8554/live/ch0?x=1");  // no credentials on the wire

    auto d = parseRtspUrl("RTSP://cam.local/stream");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->port, 554);
    EXPECT_EQ(d->requestUri(), "rtsp://cam.local/stream");

    auto v6 = parseRtspUrl("rtsp://[fe80::1]:554/s");
    ASSERT_TRUE(v6);
    EXPECT_EQ(v6->host, "fe80::1");
    EXPECT_EQ(v6->requestUri(), "rtsp://[fe80::1]/s");

    EXPECT_FALSE(parseRtspUrl("http://x/y"));
    EXPECT_FALSE(parseRtspUrl("rtsps://x/y"));
    EXPECT_FALSE(parseRtspUrl("rtsp:///path"));
    EXPECT_FALSE(parseRtspUrl("rtsp://host:99999/"));
    EXPECT_FALSE(parseRtspUrl("rtsp://host:abc/"));
    EXPECT_FALSE(parseRtspUrl("rtsp://[::1/"));
}

TEST(Url, MaskHidesCredentials) {
    EXPECT_EQ(maskUrl("rtsp://admin:hunter2@10.0.0.2/s"), "rtsp://***@10.0.0.2/s");
    EXPECT_EQ(maskUrl("rtsp://10.0.0.2/s"), "rtsp://10.0.0.2/s");
    EXPECT_EQ(maskUrl("rtsp://u@h:554/a@b"), "rtsp://***@h:554/a@b");
}

TEST(Url, ResolveControl) {
    EXPECT_EQ(resolveControl("rtsp://h/s/", "trackID=1"), "rtsp://h/s/trackID=1");
    EXPECT_EQ(resolveControl("rtsp://h/s", "trackID=1"), "rtsp://h/s/trackID=1");
    EXPECT_EQ(resolveControl("rtsp://h/s/", "rtsp://h/other/track1"), "rtsp://h/other/track1");
    EXPECT_EQ(resolveControl("rtsp://h/s/", "*"), "rtsp://h/s/");
    EXPECT_EQ(resolveControl("rtsp://h/s/", ""), "rtsp://h/s/");
}

// ---- message parser
TEST(Message, SerializeRequest) {
    Request r;
    r.method = "DESCRIBE";
    r.uri = "rtsp://h/s";
    r.headers.add("CSeq", "2");
    r.headers.add("Accept", "application/sdp");
    EXPECT_EQ(serialize(r), "DESCRIBE rtsp://h/s RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
}

TEST(Message, ParseResponseWithBody) {
    MessageParser p;
    std::string wire = "RTSP/1.0 200 OK\r\nCSeq: 3\r\nContent-Type: application/sdp\r\nContent-Length: 5\r\n\r\nhello";
    p.feed(B(wire));
    MessageParser::Item it;
    ASSERT_EQ(p.next(it), MessageParser::Status::Item);
    auto& r = std::get<Response>(it);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.reason, "OK");
    EXPECT_EQ(r.cseq(), 3);
    EXPECT_EQ(r.body, "hello");
    EXPECT_EQ(*r.headers.get("content-type"), "application/sdp");  // case-insensitive
    EXPECT_EQ(p.next(it), MessageParser::Status::NeedMore);
}

TEST(Message, ByteByByteFeedYieldsSameResult) {
    std::string wire = "RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Basic realm=\"x\"\r\n\r\n";
    MessageParser p;
    MessageParser::Item it;
    int items = 0;
    for (char c : wire) {
        std::uint8_t b = static_cast<std::uint8_t>(c);
        p.feed(ByteView(&b, 1));
        if (p.next(it) == MessageParser::Status::Item) ++items;
    }
    EXPECT_EQ(items, 1);
    EXPECT_EQ(std::get<Response>(it).status, 401);
}

TEST(Message, InterleavedFramesMixedWithText) {
    Bytes wire = frameInterleaved(0, B("RTPDATA"));
    auto resp = B("RTSP/1.0 200 OK\r\nCSeq: 9\r\n\r\n");
    wire.insert(wire.end(), resp.begin(), resp.end());
    auto f2 = frameInterleaved(1, B("RTCP"));
    wire.insert(wire.end(), f2.begin(), f2.end());
    MessageParser p;
    p.feed(wire);
    MessageParser::Item it;
    ASSERT_EQ(p.next(it), MessageParser::Status::Item);
    EXPECT_EQ(std::get<InterleavedFrame>(it).channel, 0);
    EXPECT_EQ(std::get<InterleavedFrame>(it).data, B("RTPDATA"));
    ASSERT_EQ(p.next(it), MessageParser::Status::Item);
    EXPECT_EQ(std::get<Response>(it).cseq(), 9);
    ASSERT_EQ(p.next(it), MessageParser::Status::Item);
    EXPECT_EQ(std::get<InterleavedFrame>(it).channel, 1);
}

TEST(Message, ServerRequestAndStrayCrlf) {
    MessageParser p;
    p.feed(B("\r\nGET_PARAMETER rtsp://h/s RTSP/1.0\r\nCSeq: 7\r\n\r\n"));
    MessageParser::Item it;
    ASSERT_EQ(p.next(it), MessageParser::Status::Item);
    auto& rq = std::get<Request>(it);
    EXPECT_EQ(rq.method, "GET_PARAMETER");
    EXPECT_EQ(rq.uri, "rtsp://h/s");
}

TEST(Message, HeaderFoldingAndLfOnly) {
    MessageParser p;
    p.feed(B("RTSP/1.0 200 OK\nCSeq: 4\nPublic: OPTIONS,\n DESCRIBE\n\n"));
    MessageParser::Item it;
    ASSERT_EQ(p.next(it), MessageParser::Status::Item);
    EXPECT_EQ(*std::get<Response>(it).headers.get("Public"), "OPTIONS, DESCRIBE");
}

TEST(Message, HostileInputIsRejectedNotCrashing) {
    {
        MessageParser p;
        p.feed(B("\x01\x02garbage"));
        MessageParser::Item it;
        EXPECT_EQ(p.next(it), MessageParser::Status::Error);
        EXPECT_EQ(p.next(it), MessageParser::Status::Error);  // sticky
    }
    {
        MessageParser p;  // header never terminates
        p.feed(B("RTSP/1.0 200 OK\r\n"));
        Bytes junk(MessageParser::kMaxHeaderBytes + 10, 'a');
        p.feed(junk);
        MessageParser::Item it;
        EXPECT_EQ(p.next(it), MessageParser::Status::Error);
    }
    {
        MessageParser p;
        p.feed(B("RTSP/1.0 200 OK\r\nContent-Length: 99999999\r\n\r\n"));
        MessageParser::Item it;
        EXPECT_EQ(p.next(it), MessageParser::Status::Error);
    }
    {
        MessageParser p;
        p.feed(B("RTSP/1.0 200 OK\r\nContent-Length: -5\r\n\r\n"));
        MessageParser::Item it;
        EXPECT_EQ(p.next(it), MessageParser::Status::Error);
    }
    {
        MessageParser p;
        p.feed(B("RTSP/1.0 2x0 OK\r\n\r\n"));
        MessageParser::Item it;
        EXPECT_EQ(p.next(it), MessageParser::Status::Error);
    }
}

TEST(Message, RandomBytesNeverCrash) {
    std::mt19937 g(1234);
    for (int round = 0; round < 300; ++round) {
        MessageParser p;
        Bytes junk(1 + g() % 600);
        for (auto& b : junk) b = static_cast<std::uint8_t>(g());
        if (round % 3 == 0) junk[0] = '$';
        if (round % 3 == 1) { auto h = B("RTSP/1.0 200 OK\r\n"); junk.insert(junk.begin(), h.begin(), h.end()); }
        p.feed(junk);
        MessageParser::Item it;
        for (int i = 0; i < 50; ++i)
            if (p.next(it) != MessageParser::Status::Item) break;
    }
}

// ---- RTP / RTCP
TEST(Rtp, ParseValid) {
    Bytes pkt = buildRtp(96, true, 65535, 0xdeadbeef, 0x01020304, B("payload"));
    auto v = parseRtp(pkt);
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->marker);
    EXPECT_EQ(v->payloadType, 96);
    EXPECT_EQ(v->sequence, 65535);
    EXPECT_EQ(v->timestamp, 0xdeadbeefu);
    EXPECT_EQ(v->ssrc, 0x01020304u);
    EXPECT_EQ(toString(v->payload), "payload");
}

TEST(Rtp, ParseWithCsrcExtensionPadding) {
    Bytes pkt = buildRtp(96, false, 1, 2, 3, B("abc"));
    pkt[0] |= 0x02;  // CC=2
    pkt.insert(pkt.begin() + 12, 8, 0xAA);
    pkt[0] |= 0x10;  // extension: 1 word
    Bytes ext{0xBE, 0xDE, 0x00, 0x01, 1, 2, 3, 4};
    pkt.insert(pkt.begin() + 20, ext.begin(), ext.end());
    pkt[0] |= 0x20;  // padding 3
    pkt.insert(pkt.end(), {0, 0, 3});
    auto v = parseRtp(pkt);
    ASSERT_TRUE(v);
    EXPECT_EQ(toString(v->payload), "abc");
}

TEST(Rtp, RejectsMalformed) {
    EXPECT_FALSE(parseRtp(Bytes{}));
    EXPECT_FALSE(parseRtp(Bytes(11, 0x80)));
    Bytes v1 = buildRtp(96, false, 1, 2, 3, B("x"));
    v1[0] = 0x40;  // version 1
    EXPECT_FALSE(parseRtp(v1));
    Bytes cc = buildRtp(96, false, 1, 2, 3, {});
    cc[0] |= 0x0f;  // 15 CSRC but no room
    EXPECT_FALSE(parseRtp(cc));
    Bytes ext = buildRtp(96, false, 1, 2, 3, {});
    ext[0] |= 0x10;
    EXPECT_FALSE(parseRtp(ext));  // extension header missing
    ext.insert(ext.end(), {0, 0, 0xff, 0xff});
    EXPECT_FALSE(parseRtp(ext));  // extension length beyond packet
    Bytes pad = buildRtp(96, false, 1, 2, 3, B("x"));
    pad[0] |= 0x20;
    pad.back() = 200;  // padding larger than packet
    EXPECT_FALSE(parseRtp(pad));
}

TEST(Rtcp, ReceiverReportLayoutAndSenderReportParse) {
    ReportBlock b;
    b.sourceSsrc = 0xAABBCCDD;
    b.fractionLost = 25;
    b.cumulativeLost = 0x1000000 + 5;  // clamped to 24 bits
    b.extHighestSeq = 0x00010002;
    b.jitter = 77;
    b.lastSr = 0x11112222;
    b.delaySinceLastSr = 0x33;
    Bytes rr = buildReceiverReport(0x01020304, b, "hbstudio@x");
    ASSERT_EQ(rr.size() % 4, 0u);
    EXPECT_EQ(rr[0], 0x81);
    EXPECT_EQ(rr[1], 201);
    EXPECT_EQ((rr[2] << 8) | rr[3], 7);
    EXPECT_EQ(rr[8], 0xAA);   // report block: source ssrc
    EXPECT_EQ(rr[12], 25);    // fraction lost
    EXPECT_EQ(rr[13], 0xff);  // cumulative lost, clamped to 24 bits
    EXPECT_EQ(rr[14], 0xff);
    EXPECT_EQ(rr[15], 0xff);
    EXPECT_EQ(rr[19], 0x02);  // extended highest sequence (low byte)
    EXPECT_EQ(rr[23], 77);    // jitter
    EXPECT_EQ(rr[32], 0x81);  // SDES follows
    EXPECT_EQ(rr[33], 202);
    auto info = parseRtcp(rr);
    EXPECT_FALSE(info.malformed);
    EXPECT_FALSE(info.senderReport.has_value());

    SenderReportInfo sr;
    sr.ssrc = 9;
    sr.ntpMsw = 0x11223344;
    sr.ntpLsw = 0x55667788;
    sr.rtpTimestamp = 1234;
    Bytes wire = buildSenderReport(sr);
    wire.insert(wire.end(), {0x81, 203, 0, 1, 0, 0, 0, 9});  // BYE
    auto p = parseRtcp(wire);
    ASSERT_TRUE(p.senderReport);
    EXPECT_EQ(p.senderReport->ntpMsw, 0x11223344u);
    EXPECT_EQ(p.senderReport->rtpTimestamp, 1234u);
    EXPECT_TRUE(p.bye);
}

TEST(Rtcp, TruncatedIsMalformedNotCrashing) {
    Bytes sr = buildSenderReport({});
    sr.resize(10);
    EXPECT_TRUE(parseRtcp(sr).malformed);
    EXPECT_TRUE(parseRtcp(Bytes{0x00, 0x00, 0x00, 0x00}).malformed);
}
