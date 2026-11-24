#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include "core/json.hpp"
#include "remote/commands.hpp"
#include "remote/http.hpp"
#include "remote/lan.hpp"
#include "remote/sha1.hpp"
#include "remote/state.hpp"
#include "remote/websocket.hpp"

using namespace hb::remote;

namespace {
std::string hex(const std::array<std::uint8_t, 20>& d) {
    static const char* h = "0123456789abcdef";
    std::string o;
    for (auto b : d) { o += h[b >> 4]; o += h[b & 15]; }
    return o;
}
const std::uint8_t kMask[4] = {0x37, 0xfa, 0x21, 0x3d};
std::vector<std::uint8_t> masked(ws::Opcode op, const std::string& s, bool fin = true) {
    return ws::encodeMaskedFrame(op, reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), kMask, fin);
}
hb::core::json::Value J(const std::string& s) {
    hb::core::json::Value v;
    EXPECT_TRUE(hb::core::json::parse(s, v));
    return v;
}
}  // namespace

TEST(RemoteSha1, Rfc3174Vectors) {
    EXPECT_EQ(hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(hex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(hex(sha1(std::string(1000000, 'a'))), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(RemoteWs, Rfc6455HandshakeExample) {
    EXPECT_EQ(ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_TRUE(ws::validClientKey("dGhlIHNhbXBsZSBub25jZQ=="));
    EXPECT_FALSE(ws::validClientKey("short"));
    EXPECT_FALSE(ws::validClientKey("dGhlIHNhbXBsZSBub25jZQ"));
}

TEST(RemoteWs, ParsesMaskedTextAndSplitDelivery) {
    ws::Parser p;
    auto f = masked(ws::Opcode::Text, "hello");
    std::vector<ws::Message> out;
    for (std::size_t i = 0; i < f.size(); ++i) ASSERT_TRUE(p.feed(&f[i], 1, out).ok);  // byte by byte
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].type, ws::Opcode::Text);
    EXPECT_EQ(out[0].data, "hello");
}

TEST(RemoteWs, LargeLengthEncodings) {
    ws::Parser p(ws::ParserLimits{200000});
    std::string s126(300, 'x'), s127(70000, 'y');
    std::vector<ws::Message> out;
    auto a = masked(ws::Opcode::Binary, s126), b = masked(ws::Opcode::Binary, s127);
    ASSERT_TRUE(p.feed(a.data(), a.size(), out).ok);
    ASSERT_TRUE(p.feed(b.data(), b.size(), out).ok);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].data.size(), 300u);
    EXPECT_EQ(out[1].data.size(), 70000u);
}

TEST(RemoteWs, FragmentationWithInterleavedPing) {
    ws::Parser p;
    std::vector<ws::Message> out;
    auto f1 = masked(ws::Opcode::Text, "hel", false);
    auto ping = masked(ws::Opcode::Ping, "pp");
    auto f2 = ws::encodeMaskedFrame(ws::Opcode::Continuation, reinterpret_cast<const std::uint8_t*>("lo"), 2, kMask, true);
    for (auto* v : {&f1, &ping, &f2}) ASSERT_TRUE(p.feed(v->data(), v->size(), out).ok);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].type, ws::Opcode::Ping);
    EXPECT_EQ(out[1].data, "hello");
}

TEST(RemoteWs, RejectsProtocolViolations) {
    auto bad = [](std::vector<std::uint8_t> bytes, std::uint16_t code, ws::ParserLimits lim = {}) {
        ws::Parser p(lim);
        std::vector<ws::Message> out;
        auto r = p.feed(bytes.data(), bytes.size(), out);
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(r.closeCode, code);
        // poisoned afterwards
        EXPECT_FALSE(p.feed(bytes.data(), 1, out).ok);
    };
    // unmasked client frame
    auto un = ws::encodeText("hi");
    bad(un, 1002);
    // reserved bit
    auto rsv = masked(ws::Opcode::Text, "x");
    rsv[0] |= 0x40;
    bad(rsv, 1002);
    // unknown opcode
    auto op = masked(ws::Opcode::Text, "x");
    op[0] = 0x83;
    bad(op, 1002);
    // fragmented control frame
    bad(masked(ws::Opcode::Ping, "x", false), 1002);
    // oversized control frame
    bad(masked(ws::Opcode::Ping, std::string(126, 'a')), 1002);
    // orphan continuation
    bad(ws::encodeMaskedFrame(ws::Opcode::Continuation, reinterpret_cast<const std::uint8_t*>("x"), 1, kMask, true), 1002);
    // too large, declared length checked before buffering (64-bit length of 1 TiB)
    std::vector<std::uint8_t> huge = {0x82, 0xFF, 0, 0, 1, 0, 0, 0, 0, 0};
    bad(huge, 1009);
    bad(masked(ws::Opcode::Binary, std::string(300, 'a')), 1009, ws::ParserLimits{100});
    // invalid UTF-8
    bad(masked(ws::Opcode::Text, std::string("\xC0\xAF")), 1007);
    // non-minimal length encoding
    std::vector<std::uint8_t> nm = {0x82, 0xFE, 0x00, 0x05};
    bad(nm, 1002);
}

TEST(RemoteWs, FragmentedMessageOverLimitRejected) {
    ws::Parser p(ws::ParserLimits{100});
    std::vector<ws::Message> out;
    auto a = masked(ws::Opcode::Binary, std::string(80, 'a'), false);
    auto b = ws::encodeMaskedFrame(ws::Opcode::Continuation, reinterpret_cast<const std::uint8_t*>(std::string(80, 'b').data()), 80, kMask, true);
    ASSERT_TRUE(p.feed(a.data(), a.size(), out).ok);
    auto r = p.feed(b.data(), b.size(), out);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.closeCode, 1009);
}

TEST(RemoteWs, CloseFrame) {
    ws::Parser p;
    std::vector<ws::Message> out;
    std::uint8_t pl[4] = {0x03, 0xE8, 'b', 'y'};
    auto f = ws::encodeMaskedFrame(ws::Opcode::Close, pl, 4, kMask);
    ASSERT_TRUE(p.feed(f.data(), f.size(), out).ok);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].closeCode, 1000);
    EXPECT_EQ(out[0].data, "by");
    ws::Parser q;
    std::uint8_t badc[2] = {0x03, 0xED};  // 1005 is not allowed on the wire
    auto g = ws::encodeMaskedFrame(ws::Opcode::Close, badc, 2, kMask);
    EXPECT_FALSE(q.feed(g.data(), g.size(), out).ok);
}

TEST(RemoteWs, Utf8) {
    EXPECT_TRUE(ws::validUtf8("olá, mundo ✓ 😀"));
    EXPECT_FALSE(ws::validUtf8("\xED\xA0\x80"));      // surrogate
    EXPECT_FALSE(ws::validUtf8("\xF4\x90\x80\x80"));  // > U+10FFFF
    EXPECT_FALSE(ws::validUtf8("\xE2\x82"));          // truncated
    EXPECT_FALSE(ws::validUtf8("\xC0\x80"));          // overlong
}

TEST(RemoteWs, ServerFrameEncoding) {
    auto f = ws::encodeText("abc");
    ASSERT_EQ(f.size(), 5u);
    EXPECT_EQ(f[0], 0x81);
    EXPECT_EQ(f[1], 3);  // unmasked
    auto big = ws::encodeFrame(ws::Opcode::Binary, std::vector<std::uint8_t>(70000).data(), 70000);
    EXPECT_EQ(big[1], 127);
    EXPECT_EQ(big.size(), 70010u);
}

TEST(RemoteHttp, ParsesGetAndUpgradeHeaders) {
    http::RequestParser p;
    const std::string req = "GET /ws?x=1 HTTP/1.1\r\nHost: 192.168.0.5:8787\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n\r\nEXTRA";
    ASSERT_EQ(p.feed(reinterpret_cast<const std::uint8_t*>(req.data()), req.size()), http::RequestParser::State::Done);
    EXPECT_EQ(p.request().method, "GET");
    EXPECT_EQ(p.request().path, "/ws");
    EXPECT_EQ(p.leftover(), "EXTRA");
    EXPECT_TRUE(http::headerHasToken(*p.request().header("connection"), "upgrade"));
}

TEST(RemoteHttp, RejectsMalformedAndOversized) {
    auto run = [](const std::string& r, std::size_t max = 8192) {
        http::RequestParser p(max);
        auto st = p.feed(reinterpret_cast<const std::uint8_t*>(r.data()), r.size());
        return std::make_pair(st, p.errorStatus());
    };
    EXPECT_EQ(run("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n").first, http::RequestParser::State::Error);        // duplicate
    EXPECT_EQ(run("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n").first, http::RequestParser::State::Error);  // body
    EXPECT_EQ(run("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").first, http::RequestParser::State::Error);
    EXPECT_EQ(run("GET / HTTP/2\r\nHost: a\r\n\r\n").second, 505);
    EXPECT_EQ(run("GET nopath HTTP/1.1\r\nHost: a\r\n\r\n").first, http::RequestParser::State::Error);
    EXPECT_EQ(run("GET / HTTP/1.1\r\nHost: a\r\n bad: fold\r\n\r\n").first, http::RequestParser::State::Error);
    EXPECT_EQ(run("GET / HTTP/1.1\r\nHost: a\r\nX: \x01\r\n\r\n").first, http::RequestParser::State::Error);
    auto big = run("GET / HTTP/1.1\r\nHost: a\r\n" + std::string(9000, 'a'));
    EXPECT_EQ(big.first, http::RequestParser::State::Error);
    EXPECT_EQ(big.second, 431);
    EXPECT_EQ(run("GET / HTTP/1.1\r\nHost").first, http::RequestParser::State::NeedMore);
}

TEST(RemoteHttp, HostAndOriginChecks) {
    EXPECT_TRUE(http::hostIsIpLiteralOrLocalhost("192.168.0.5:8787"));
    EXPECT_TRUE(http::hostIsIpLiteralOrLocalhost("localhost:80"));
    EXPECT_TRUE(http::hostIsIpLiteralOrLocalhost("[fe80::1]:80"));
    EXPECT_FALSE(http::hostIsIpLiteralOrLocalhost("evil.example.com"));
    EXPECT_FALSE(http::hostIsIpLiteralOrLocalhost("evil.example.com:8787"));
    EXPECT_FALSE(http::hostIsIpLiteralOrLocalhost("300.1.1.1"));
    EXPECT_FALSE(http::hostIsIpLiteralOrLocalhost("1.2.3"));
    EXPECT_FALSE(http::hostIsIpLiteralOrLocalhost(""));
    EXPECT_TRUE(http::originMatchesHost("http://192.168.0.5:8787", "192.168.0.5:8787"));
    EXPECT_FALSE(http::originMatchesHost("http://evil.com", "192.168.0.5:8787"));
    EXPECT_FALSE(http::originMatchesHost("https://192.168.0.5:8787", "192.168.0.5:8787"));
    EXPECT_FALSE(http::originMatchesHost("null", "192.168.0.5:8787"));
}

TEST(RemoteLan, PrivateRanges) {
    EXPECT_TRUE(isPrivateIpv4("192.168.1.20"));
    EXPECT_TRUE(isPrivateIpv4("10.0.0.1"));
    EXPECT_TRUE(isPrivateIpv4("172.16.5.5"));
    EXPECT_TRUE(isPrivateIpv4("172.31.5.5"));
    EXPECT_TRUE(isPrivateIpv4("169.254.1.1"));
    EXPECT_FALSE(isPrivateIpv4("172.32.0.1"));
    EXPECT_FALSE(isPrivateIpv4("8.8.8.8"));
    EXPECT_FALSE(isPrivateIpv4(""));
    EXPECT_FALSE(isPrivateIpv4("10.0.0.1.5"));
    EXPECT_FALSE(isPrivateIpv4("+10.0.0.1"));
    EXPECT_FALSE(isPrivateIpv4("999.1.1.1"));
    for (const auto& a : listLanAddresses()) EXPECT_TRUE(isPrivateIpv4(a));  // whatever this machine has
}

TEST(RemoteCommands, AllowlistTranslation) {
    auto t = [](const char* j) { return translate(J(j)); };
    EXPECT_EQ(t(R"({"c":"cut"})").hostCommand, "cut");
    EXPECT_EQ(t(R"({"c":"auto"})").hostCommand, "dissolve|500");
    EXPECT_EQ(t(R"({"c":"auto","ms":1000})").hostCommand, "dissolve|1000");
    EXPECT_EQ(t(R"({"c":"pvw","i":3})").hostCommand, "preview|3");
    EXPECT_EQ(t(R"({"c":"pgm","i":15})").hostCommand, "program|15");
    EXPECT_EQ(t(R"({"c":"tbar","v":0.5})").hostCommand, "tbar|0.500");
    EXPECT_EQ(t(R"({"c":"mute","ch":2,"on":true})").hostCommand, "aud|strip|2|mute|1");
    EXPECT_EQ(t(R"({"c":"solo","ch":0,"on":false})").hostCommand, "aud|strip|0|solo|0");
    EXPECT_EQ(t(R"({"c":"fader","ch":1,"db":-6.5})").hostCommand, "aud|strip|1|faderDb|-6.5");
    EXPECT_EQ(t(R"({"c":"fader","ch":1,"db":-80})").hostCommand, "aud|strip|1|faderDb|-120.0");
    EXPECT_EQ(t(R"({"c":"fader","ch":1,"db":9})").hostCommand, "aud|strip|1|faderDb|6.0");
    EXPECT_EQ(t(R"({"c":"rec","on":true})").hostCommand, "rec|start");
    EXPECT_EQ(t(R"({"c":"rec","on":false})").hostCommand, "rec|stop");
    EXPECT_EQ(t(R"({"c":"xmit","id":"d3","on":true})").hostCommand, "xmitstart|d3");
    EXPECT_EQ(t(R"({"c":"xmit","id":"d3","on":false})").hostCommand, "xmitstop|d3");
    EXPECT_EQ(t(R"({"c":"tp","a":"play"})").hostCommand, "tp|play");
    EXPECT_EQ(t(R"({"c":"tp","a":"pause"})").hostCommand, "tp|pause");
    EXPECT_EQ(t(R"({"c":"tp","a":"speed","v":1.2})").hostCommand, "tp|speed|1.200");
    EXPECT_EQ(t(R"({"c":"tp","a":"seek","v":30})").hostCommand, "tp|seek|30.000");
}

TEST(RemoteCommands, RejectsEverythingElse) {
    auto bad = [](const char* j) { return !translate(J(j)).ok; };
    // Not on the allowlist: config, secrets, destructive/native commands, other agents' commands.
    for (const char* c : {"xmitsave", "xmitremove", "xmitstartall", "xmitstopall", "outfmt", "devassign", "ndiout", "outwin", "plugin",
                          "fxeditor", "uitheme", "rec|start", "cut|1", "remote", "aud", "ready", "ptz", ""})
        EXPECT_TRUE(bad((std::string("{\"c\":\"") + c + "\"}").c_str())) << c;
    EXPECT_TRUE(bad(R"({"c":"pvw","i":16})"));
    EXPECT_TRUE(bad(R"({"c":"pvw","i":-1})"));
    EXPECT_TRUE(bad(R"({"c":"pvw","i":1.5})"));
    EXPECT_TRUE(bad(R"({"c":"pvw","i":"1"})"));
    EXPECT_TRUE(bad(R"({"c":"tbar","v":1.5})"));
    EXPECT_TRUE(bad(R"({"c":"auto","ms":5})"));
    EXPECT_TRUE(bad(R"({"c":"auto","ms":999999})"));
    EXPECT_TRUE(bad(R"({"c":"mute","ch":99,"on":true})"));
    EXPECT_TRUE(bad(R"({"c":"mute","ch":1,"on":1})"));
    EXPECT_TRUE(bad(R"({"c":"xmit","id":"d1|xmitremove|d2","on":true})"));  // injection attempt
    EXPECT_TRUE(bad(R"({"c":"xmit","id":"","on":true})"));
    EXPECT_TRUE(bad(R"({"c":"tp","a":"rm"})"));
    EXPECT_TRUE(bad(R"({"c":"tp","a":"speed"})"));
    EXPECT_TRUE(bad(R"({"c":"tp","a":"speed","v":1e30})"));
    EXPECT_TRUE(bad(R"({"c":5})"));
    EXPECT_TRUE(bad(R"([1,2])"));
    EXPECT_TRUE(bad(R"({})"));
}

TEST(RemoteState, JsonHasNoSecretsAndIsValid) {
    RemoteState s;
    s.pvw = 2; s.pgm = 1; s.recording = true;
    s.sources = {{"CAM \"1\"", "online"}, {"x\n", "test"}};
    s.strips = {{"Mic", -6.4, true, false, -20.2}};
    s.dests = {{"d1", "YouTube", "youtube", true, false}};
    s.progress = 0.5; s.masterPeakDb = std::numeric_limits<double>::quiet_NaN();
    const std::string j = toJson(s);
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(j, v)) << j;
    EXPECT_EQ(v.get("t")->s, "s");
    EXPECT_EQ(v.get("pvw")->n, 2);
    EXPECT_EQ(v.get("prog")->n, 50);
    EXPECT_EQ(v.get("src")->a.size(), 2u);
    EXPECT_EQ(v.get("aud")->a[0].get("db")->n, -6);
    EXPECT_EQ(v.get("tx")->a[0].get("run")->b, true);
    for (const char* k : {"key", "url", "server", "rtmp", "path"}) EXPECT_EQ(j.find(k), std::string::npos) << k;
    EXPECT_EQ(toJson(s), j);  // deterministic -> "only on change" dedupe works
}
