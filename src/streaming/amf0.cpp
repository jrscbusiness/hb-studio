#include "streaming/amf0.hpp"

#include <algorithm>
#include <cstring>

namespace hb::streaming::amf0 {

namespace {
constexpr std::uint8_t kNumber = 0x00, kBoolean = 0x01, kString = 0x02, kObject = 0x03, kNull = 0x05,
                       kUndefined = 0x06, kObjectEnd = 0x09;

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void putDouble(std::vector<std::uint8_t>& out, double v) {
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(v));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
}

bool getU16(const std::uint8_t* d, std::size_t len, std::size_t pos, std::uint16_t& v) {
    if (pos + 2 > len) return false;
    v = static_cast<std::uint16_t>((d[pos] << 8) | d[pos + 1]);
    return true;
}

bool getDouble(const std::uint8_t* d, std::size_t len, std::size_t pos, double& v) {
    if (pos + 8 > len) return false;
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | d[pos + i];
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}
}  // namespace

const Value* Value::field(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void encode(const Value& v, std::vector<std::uint8_t>& out) {
    switch (v.type) {
        case Value::Type::Number:
            out.push_back(kNumber);
            putDouble(out, v.num);
            break;
        case Value::Type::Boolean:
            out.push_back(kBoolean);
            out.push_back(v.boolean ? 1 : 0);
            break;
        case Value::Type::String:
            out.push_back(kString);
            putU16(out, static_cast<std::uint16_t>(std::min<std::size_t>(v.str.size(), 0xFFFF)));
            out.insert(out.end(), v.str.begin(), v.str.begin() + std::min<std::size_t>(v.str.size(), 0xFFFF));
            break;
        case Value::Type::Object:
            out.push_back(kObject);
            for (const auto& kv : v.obj) {
                putU16(out, static_cast<std::uint16_t>(std::min<std::size_t>(kv.first.size(), 0xFFFF)));
                out.insert(out.end(), kv.first.begin(), kv.first.begin() + std::min<std::size_t>(kv.first.size(), 0xFFFF));
                encode(kv.second, out);
            }
            putU16(out, 0);
            out.push_back(kObjectEnd);
            break;
        case Value::Type::Null:
            out.push_back(kNull);
            break;
        case Value::Type::Undefined:
            out.push_back(kUndefined);
            break;
    }
}

bool decode(const std::uint8_t* data, std::size_t len, std::size_t& pos, Value& out) {
    if (pos >= len) return false;
    const std::uint8_t marker = data[pos];
    std::size_t p = pos + 1;
    switch (marker) {
        case kNumber: {
            double v;
            if (!getDouble(data, len, p, v)) return false;
            out = Value::number(v);
            p += 8;
            break;
        }
        case kBoolean: {
            if (p >= len) return false;
            out = Value::boolean_(data[p] != 0);
            p += 1;
            break;
        }
        case kString: {
            std::uint16_t n;
            if (!getU16(data, len, p, n)) return false;
            p += 2;
            if (p + n > len) return false;
            out = Value::string(std::string(reinterpret_cast<const char*>(data + p), n));
            p += n;
            break;
        }
        case kObject: {
            std::vector<std::pair<std::string, Value>> props;
            for (;;) {
                std::uint16_t klen;
                if (!getU16(data, len, p, klen)) return false;
                p += 2;
                if (klen == 0) {   // the empty-key marker preceding ObjectEnd
                    if (p >= len || data[p] != kObjectEnd) return false;
                    p += 1;
                    break;
                }
                if (p + klen > len) return false;
                std::string key(reinterpret_cast<const char*>(data + p), klen);
                p += klen;
                Value v;
                if (!decode(data, len, p, v)) return false;
                props.emplace_back(std::move(key), std::move(v));
                if (props.size() > 256) return false;   // hostile/corrupt input guard
            }
            out = Value::object(std::move(props));
            break;
        }
        case kNull:
            out = Value::null();
            break;
        case kUndefined:
            out = Value{};
            out.type = Value::Type::Undefined;
            break;
        default:
            return false;   // an AMF0 type this client doesn't need (array/date/xml/ref/AMF3...)
    }
    pos = p;
    return true;
}

std::vector<Value> decodeAll(const std::uint8_t* data, std::size_t len) {
    std::vector<Value> out;
    std::size_t pos = 0;
    while (pos < len) {
        Value v;
        if (!decode(data, len, pos, v)) break;
        out.push_back(std::move(v));
    }
    return out;
}

}  // namespace hb::streaming::amf0
