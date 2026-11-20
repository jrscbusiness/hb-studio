#include <gtest/gtest.h>

#include <string>

#include "core/json.hpp"
#include "core/sha256.hpp"

using namespace hb::core;

TEST(Sha256, NistVectors) {
    EXPECT_EQ(Sha256::hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Sha256::hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 h;                      // one million 'a', fed in odd-sized chunks
    const std::string chunk(997, 'a');
    std::size_t left = 1000000;
    while (left) {
        const std::size_t n = std::min(left, chunk.size());
        h.update(chunk.data(), n);
        left -= n;
    }
    EXPECT_EQ(h.hexDigest(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ResetAndBoundaryLengths) {
    for (std::size_t n : {55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u}) {
        const std::string data(n, 'x');
        Sha256 a;                  // one shot
        a.update(data);
        Sha256 b;                  // byte by byte
        for (char c : data) b.update(&c, 1);
        EXPECT_EQ(a.hexDigest(), b.hexDigest()) << n;
    }
    Sha256 h;
    h.update("abc");
    h.hexDigest();
    h.reset();
    h.update("abc");
    EXPECT_EQ(h.hexDigest(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Json, ParsesAllTypesAndEscapes) {
    json::Value v;
    // Not a raw string here: MSVC's lexer treats a literal "\u" followed by 4 hex digits as a
    // universal-character-name even inside a raw string, and rejects \ud83d/\ude00 (surrogate
    // halves are not valid UCN values). Doubling each backslash ("\\n", "\\u00e9", ...) makes the
    // lexer consume it as the single-backslash escape "\\" first, so the following letter/digits
    // are never looked at as the start of a UCN; the JSON text the parser sees is unchanged.
    ASSERT_TRUE(json::parse("{\"a\":1.5e2,\"b\":[true,false,null],\"c\":\"x\\ny\\u00e9\\ud83d\\ude00\",\"d\":{\"e\":-3}}", v));
    EXPECT_EQ(v.get("a")->n, 150.0);
    EXPECT_EQ(v.get("b")->a.size(), 3u);
    EXPECT_TRUE(v.get("b")->a[0].b);
    EXPECT_EQ(v.get("c")->s, "x\ny\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_EQ(v.get("d")->get("e")->n, -3);
    EXPECT_EQ(v.get("nope"), nullptr);
}

TEST(Json, RejectsMalformedAndHostileInput) {
    json::Value v;
    for (const char* bad : {"", "{", "[1,", "{\"a\":}", "{'a':1}", "[1 2]", "{\"a\":1,}", "01", "1.", "-", "\"\\x\"", "\"\\ud800\"",
                            "\"\\udc00\"", "nul", "tru", "{\"a\":1} extra", "[\"unterminated"})
        EXPECT_FALSE(json::parse(bad, v)) << bad;
    EXPECT_FALSE(json::parse(std::string("\"a\x01z\""), v));        // raw control char inside a string
    EXPECT_FALSE(json::parse(std::string(5000, '['), v));           // depth bomb
    std::string big = "[";
    for (int i = 0; i < 5000; ++i) big += "1,";
    big += "1]";
    EXPECT_FALSE(json::parse(big, v));                               // too many elements
    json::Limits tiny;
    tiny.maxBytes = 10;
    EXPECT_FALSE(json::parse("[1,2,3,4,5,6,7,8]", v, tiny));
    EXPECT_TRUE(json::parse("\xEF\xBB\xBF{\"bom\":true}", v));       // BOM tolerated
}

TEST(Json, FailedParseLeavesOutputUntouched) {
    json::Value v;
    ASSERT_TRUE(json::parse("[1]", v));
    EXPECT_FALSE(json::parse("[1,", v));
    EXPECT_EQ(v.a.size(), 1u);
}

TEST(Json, EscapeRoundTrip) {
    const std::string raw = "quote\" back\\ nl\n tab\t ctl\x01 uni\xC3\xA9";
    json::Value v;
    ASSERT_TRUE(json::parse("\"" + json::escape(raw) + "\"", v));
    EXPECT_EQ(v.s, raw);
}
