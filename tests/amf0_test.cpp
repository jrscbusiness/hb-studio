#include <gtest/gtest.h>

#include "streaming/amf0.hpp"

using namespace hb::streaming::amf0;

namespace {
template <typename T>
Value roundTrip(const T& v) {
    std::vector<std::uint8_t> buf;
    encode(v, buf);
    std::size_t pos = 0;
    Value out;
    EXPECT_TRUE(decode(buf.data(), buf.size(), pos, out));
    EXPECT_EQ(pos, buf.size());
    return out;
}
}  // namespace

TEST(Amf0, NumberRoundTrips) {
    auto v = roundTrip(Value::number(3.5));
    EXPECT_EQ(v.type, Value::Type::Number);
    EXPECT_DOUBLE_EQ(v.num, 3.5);
    auto neg = roundTrip(Value::number(-1));
    EXPECT_DOUBLE_EQ(neg.num, -1);
}

TEST(Amf0, BooleanRoundTrips) {
    EXPECT_TRUE(roundTrip(Value::boolean_(true)).boolean);
    EXPECT_FALSE(roundTrip(Value::boolean_(false)).boolean);
}

TEST(Amf0, StringRoundTrips) {
    auto v = roundTrip(Value::string("rtmp://example/live"));
    EXPECT_EQ(v.type, Value::Type::String);
    EXPECT_EQ(v.str, "rtmp://example/live");
}

TEST(Amf0, NullAndUndefinedRoundTrip) {
    EXPECT_EQ(roundTrip(Value::null()).type, Value::Type::Null);
    Value u; u.type = Value::Type::Undefined;
    EXPECT_EQ(roundTrip(u).type, Value::Type::Undefined);
}

TEST(Amf0, ObjectRoundTripsNestedAndField) {
    auto obj = Value::object({{"app", Value::string("live")}, {"tcUrl", Value::string("rtmp://x/live")},
                               {"fpad", Value::boolean_(false)}, {"nested", Value::object({{"ok", Value::number(1)}})}});
    auto v = roundTrip(obj);
    ASSERT_EQ(v.type, Value::Type::Object);
    EXPECT_EQ(v.field("app")->str, "live");
    EXPECT_EQ(v.field("tcUrl")->str, "rtmp://x/live");
    EXPECT_FALSE(v.field("fpad")->boolean);
    EXPECT_DOUBLE_EQ(v.field("nested")->field("ok")->num, 1);
    EXPECT_EQ(v.field("missing"), nullptr);
}

TEST(Amf0, DecodeAllParsesAFullCommandMessage) {
    // ["_result", 1, null, {"code": "NetConnection.Connect.Success"}]
    std::vector<std::uint8_t> buf;
    encode(Value::string("_result"), buf);
    encode(Value::number(1), buf);
    encode(Value::null(), buf);
    encode(Value::object({{"code", Value::string("NetConnection.Connect.Success")}}), buf);
    auto vals = decodeAll(buf.data(), buf.size());
    ASSERT_EQ(vals.size(), 4u);
    EXPECT_EQ(vals[0].str, "_result");
    EXPECT_DOUBLE_EQ(vals[1].num, 1);
    EXPECT_EQ(vals[2].type, Value::Type::Null);
    EXPECT_EQ(vals[3].field("code")->str, "NetConnection.Connect.Success");
}

TEST(Amf0, MalformedOrTruncatedInputNeverCrashesAndFailsCleanly) {
    std::vector<std::uint8_t> buf;
    encode(Value::string("hi"), buf);
    for (std::size_t cut = 0; cut < buf.size(); ++cut) {
        std::size_t pos = 0;
        Value out;
        EXPECT_FALSE(decode(buf.data(), cut, pos, out));
    }
    const std::uint8_t garbage[] = {0xFE, 0x01, 0x02};
    std::size_t pos = 0;
    Value out;
    EXPECT_FALSE(decode(garbage, sizeof garbage, pos, out));   // an AMF0 type we don't implement
    EXPECT_TRUE(decodeAll(garbage, sizeof garbage).empty());
    EXPECT_TRUE(decodeAll(nullptr, 0).empty());
}

TEST(Amf0, ObjectEndMarkerWithoutMatchingStartIsRejected) {
    const std::uint8_t bad[] = {0x03, 0x00, 0x00, 0x09};   // Object with 0-length key but no End marker byte missing len
    std::size_t pos = 0;
    Value out;
    // This one IS well-formed (empty object): 0x03 (Object) then key-len 0 then 0x09 (End).
    EXPECT_TRUE(decode(bad, sizeof bad, pos, out));
    EXPECT_EQ(pos, sizeof bad);
    EXPECT_TRUE(out.obj.empty());
}
