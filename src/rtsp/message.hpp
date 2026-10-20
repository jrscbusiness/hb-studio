#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "rtsp/types.hpp"

namespace hb::rtsp {

// Case-insensitive, order-preserving header list.
class Headers {
public:
    void add(std::string name, std::string value);
    void set(const std::string& name, std::string value);  // replaces all with same name
    std::optional<std::string> get(std::string_view name) const;
    std::vector<std::string> getAll(std::string_view name) const;
    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct Request {
    std::string method;
    std::string uri;
    Headers headers;
    std::string body;
};

struct Response {
    int status = 0;
    std::string reason;
    Headers headers;
    std::string body;
    int cseq() const;  // -1 if absent/invalid
};

struct InterleavedFrame {
    std::uint8_t channel = 0;
    Bytes data;
};

// Serializers add Content-Length automatically when a body is present.
std::string serialize(const Request& r);
std::string serialize(const Response& r);
// "$" + channel + 16-bit length + payload (RFC 2326 10.12). Payloads > 65535 are rejected (empty).
Bytes frameInterleaved(std::uint8_t channel, ByteView payload);

// Incremental parser for an RTSP-over-TCP byte stream: text messages (responses or
// server-initiated requests) interleaved with binary '$' frames. Hostile input cannot make
// it allocate unboundedly (header and body sizes are capped) and never throws.
class MessageParser {
public:
    using Item = std::variant<Response, Request, InterleavedFrame>;
    enum class Status { NeedMore, Item, Error };

    static constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
    static constexpr std::size_t kMaxBodyBytes = 256 * 1024;

    void feed(ByteView data);
    Status next(Item& out);
    const std::string& error() const { return error_; }
    std::size_t buffered() const { return buf_.size() - pos_; }
    void reset();

private:
    Status fail(std::string msg);
    void compact();

    Bytes buf_;
    std::size_t pos_ = 0;
    std::string error_;
    bool failed_ = false;
};

}  // namespace hb::rtsp
