#include "rtsp/message.hpp"

#include <algorithm>
#include <cctype>

namespace hb::rtsp {

namespace {
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
void writeHeaders(std::string& o, const Headers& h, const std::string& body) {
    bool hasLen = false;
    for (auto& [k, v] : h.items()) {
        if (ieq(k, "Content-Length")) hasLen = true;
        o += k + ": " + v + "\r\n";
    }
    if (!body.empty() && !hasLen) o += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    o += "\r\n";
    o += body;
}
}  // namespace

void Headers::add(std::string name, std::string value) { items_.emplace_back(std::move(name), std::move(value)); }
void Headers::set(const std::string& name, std::string value) {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](auto& p) { return ieq(p.first, name); }),
                 items_.end());
    items_.emplace_back(name, std::move(value));
}
std::optional<std::string> Headers::get(std::string_view name) const {
    for (auto& [k, v] : items_)
        if (ieq(k, name)) return v;
    return std::nullopt;
}
std::vector<std::string> Headers::getAll(std::string_view name) const {
    std::vector<std::string> r;
    for (auto& [k, v] : items_)
        if (ieq(k, name)) r.push_back(v);
    return r;
}

int Response::cseq() const {
    auto v = headers.get("CSeq");
    if (!v || v->empty() || v->size() > 9) return -1;
    int n = 0;
    for (char c : *v) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

std::string serialize(const Request& r) {
    std::string o = r.method + " " + r.uri + " RTSP/1.0\r\n";
    writeHeaders(o, r.headers, r.body);
    return o;
}
std::string serialize(const Response& r) {
    std::string o = "RTSP/1.0 " + std::to_string(r.status) + " " + r.reason + "\r\n";
    writeHeaders(o, r.headers, r.body);
    return o;
}

Bytes frameInterleaved(std::uint8_t channel, ByteView payload) {
    if (payload.size() > 0xffff) return {};
    Bytes o;
    o.reserve(payload.size() + 4);
    o.push_back('$');
    o.push_back(channel);
    o.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    o.push_back(static_cast<std::uint8_t>(payload.size()));
    o.insert(o.end(), payload.begin(), payload.end());
    return o;
}

void MessageParser::reset() {
    buf_.clear();
    pos_ = 0;
    failed_ = false;
    error_.clear();
}

void MessageParser::feed(ByteView data) {
    if (failed_) return;
    compact();
    buf_.insert(buf_.end(), data.begin(), data.end());
}

void MessageParser::compact() {
    if (pos_ == 0) return;
    if (pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 64 * 1024) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
}

MessageParser::Status MessageParser::fail(std::string msg) {
    failed_ = true;
    error_ = std::move(msg);
    buf_.clear();
    pos_ = 0;
    return Status::Error;
}

MessageParser::Status MessageParser::next(Item& out) {
    if (failed_) return Status::Error;
    // Tolerate stray CR/LF between messages (some cameras append them).
    while (pos_ < buf_.size() && (buf_[pos_] == '\r' || buf_[pos_] == '\n')) ++pos_;
    if (pos_ >= buf_.size()) return Status::NeedMore;

    if (buf_[pos_] == '$') {
        if (buf_.size() - pos_ < 4) return Status::NeedMore;
        std::size_t len = (std::size_t(buf_[pos_ + 2]) << 8) | buf_[pos_ + 3];
        if (buf_.size() - pos_ < 4 + len) return Status::NeedMore;
        InterleavedFrame f;
        f.channel = buf_[pos_ + 1];
        f.data.assign(buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + 4),
                      buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + 4 + len));
        pos_ += 4 + len;
        out = std::move(f);
        return Status::Item;
    }

    if (!std::isalpha(buf_[pos_])) return fail("unexpected byte at message start");

    // Locate end of header block.
    const std::size_t avail = buf_.size() - pos_;
    const char* base = reinterpret_cast<const char*>(buf_.data() + pos_);
    std::string_view view(base, avail);
    std::size_t endPos = std::string_view::npos, endLen = 0;
    {
        auto a = view.find("\r\n\r\n");
        auto b = view.find("\n\n");
        if (a != std::string_view::npos && (b == std::string_view::npos || a < b)) { endPos = a; endLen = 4; }
        else if (b != std::string_view::npos) { endPos = b; endLen = 2; }
    }
    if (endPos == std::string_view::npos) {
        if (avail > kMaxHeaderBytes) return fail("header block too large");
        return Status::NeedMore;
    }
    if (endPos > kMaxHeaderBytes) return fail("header block too large");

    std::string_view head = view.substr(0, endPos);
    // Split lines, unfolding continuation lines.
    std::vector<std::string> lines;
    std::size_t p = 0;
    while (p <= head.size()) {
        auto nl = head.find('\n', p);
        std::string_view line = head.substr(p, nl == std::string_view::npos ? std::string_view::npos : nl - p);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!lines.empty() && !line.empty() && (line[0] == ' ' || line[0] == '\t'))
            lines.back() += " " + std::string(trim(line));
        else
            lines.emplace_back(line);
        if (nl == std::string_view::npos) break;
        p = nl + 1;
    }
    if (lines.empty() || lines[0].empty()) return fail("empty start line");

    Headers headers;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto colon = lines[i].find(':');
        if (colon == std::string::npos || colon == 0) continue;  // ignore junk header lines
        headers.add(std::string(trim(std::string_view(lines[i]).substr(0, colon))),
                    std::string(trim(std::string_view(lines[i]).substr(colon + 1))));
    }

    std::size_t bodyLen = 0;
    if (auto cl = headers.get("Content-Length")) {
        if (cl->empty() || cl->size() > 9) return fail("bad Content-Length");
        for (char c : *cl) {
            if (c < '0' || c > '9') return fail("bad Content-Length");
            bodyLen = bodyLen * 10 + std::size_t(c - '0');
        }
        if (bodyLen > kMaxBodyBytes) return fail("body too large");
    }
    const std::size_t total = endPos + endLen + bodyLen;
    if (avail < total) return Status::NeedMore;
    std::string body(base + endPos + endLen, bodyLen);

    // Start line: response "RTSP/1.0 200 OK" or request "METHOD uri RTSP/1.0".
    const std::string& sl = lines[0];
    if (sl.compare(0, 5, "RTSP/") == 0) {
        auto sp1 = sl.find(' ');
        if (sp1 == std::string::npos) return fail("bad status line");
        auto sp2 = sl.find(' ', sp1 + 1);
        std::string code = sl.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
        if (code.size() != 3 || !std::isdigit((unsigned char)code[0]) || !std::isdigit((unsigned char)code[1]) ||
            !std::isdigit((unsigned char)code[2]))
            return fail("bad status code");
        Response r;
        r.status = std::stoi(code);
        if (sp2 != std::string::npos) r.reason = sl.substr(sp2 + 1);
        r.headers = std::move(headers);
        r.body = std::move(body);
        out = std::move(r);
    } else {
        auto sp1 = sl.find(' ');
        auto sp2 = sp1 == std::string::npos ? sp1 : sl.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return fail("bad request line");
        Request r;
        r.method = sl.substr(0, sp1);
        r.uri = sl.substr(sp1 + 1, sp2 - sp1 - 1);
        r.headers = std::move(headers);
        r.body = std::move(body);
        out = std::move(r);
    }
    pos_ += total;
    return Status::Item;
}

}  // namespace hb::rtsp
