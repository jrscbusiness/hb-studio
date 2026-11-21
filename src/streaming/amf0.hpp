#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Minimal AMF0 (Action Message Format 0) encoder/decoder - the value format RTMP command messages
// (connect, createStream, publish, onStatus, _result...) are carried in. Only the subset RTMP
// actually uses for these commands is implemented: Number, Boolean, String, Object, Null,
// Undefined. ECMA/strict arrays, References, Dates, XML, Typed objects and AMF3 are out of scope -
// no real media server sends those in a connect/createStream/publish/onStatus exchange.
namespace hb::streaming::amf0 {

struct Value {
    enum class Type { Number, Boolean, String, Object, Null, Undefined } type = Type::Undefined;
    double num = 0;
    bool boolean = false;
    std::string str;
    std::vector<std::pair<std::string, Value>> obj;   // Type::Object only

    static Value number(double v) { Value x; x.type = Type::Number; x.num = v; return x; }
    static Value boolean_(bool v) { Value x; x.type = Type::Boolean; x.boolean = v; return x; }
    static Value string(std::string v) { Value x; x.type = Type::String; x.str = std::move(v); return x; }
    static Value null() { Value x; x.type = Type::Null; return x; }
    static Value object(std::vector<std::pair<std::string, Value>> props) {
        Value x; x.type = Type::Object; x.obj = std::move(props); return x;
    }

    // Looks up a key in an Object value; nullptr if absent or not an Object.
    const Value* field(const std::string& key) const;
};

// Appends the AMF0 encoding of `v` to `out`.
void encode(const Value& v, std::vector<std::uint8_t>& out);

// Decodes one value starting at data[pos]; advances pos past it. False (pos untouched) on
// malformed or truncated input - never throws, never reads past `len`.
bool decode(const std::uint8_t* data, std::size_t len, std::size_t& pos, Value& out);

// Decodes every value from pos to len into a flat list (a full AMF0 command message, e.g.
// ["onStatus", 0, null, {...}]), stopping (keeping whatever decoded so far) at the first malformed
// value rather than failing the whole message - server messages sometimes carry trailing values
// this client doesn't need to understand.
std::vector<Value> decodeAll(const std::uint8_t* data, std::size_t len);

}  // namespace hb::streaming::amf0
