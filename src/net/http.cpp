#include "net/http.hpp"

#include <algorithm>
#include <cctype>

#include "net/body_reader.hpp"
#include "net/url.hpp"

namespace hb::net {

std::string HttpResponse::header(const std::string& name) const {
    for (const auto& h : headers) {
        if (h.first.size() != name.size()) continue;
        bool eq = true;
        for (std::size_t i = 0; i < name.size() && eq; ++i)
            eq = std::tolower(static_cast<unsigned char>(h.first[i])) == std::tolower(static_cast<unsigned char>(name[i]));
        if (eq) return h.second;
    }
    return {};
}

ErrorKind validateRequest(const HttpRequest& request, Url& url, std::string& errorText) {
    std::string why;
    if (!parseUrl(request.url, url, &why)) {
        errorText = "invalid URL: " + why;
        return ErrorKind::InvalidRequest;
    }
    if (request.httpsOnly && !url.https()) {
        errorText = "plain http:// refused (https only)";
        return ErrorKind::InvalidRequest;
    }
    constexpr std::uint64_t kMaxBody = 0xFFFFFFF0ull;  // WinHTTP total-length is a DWORD
    const std::uint64_t len = request.bodySource ? request.bodySourceLength : request.body.size();
    if (len > kMaxBody) {
        errorText = "request body too large";
        return ErrorKind::InvalidRequest;
    }
    if (request.method.empty()) {
        errorText = "empty method";
        return ErrorKind::InvalidRequest;
    }
    for (const auto& h : request.headers) {
        // CR/LF in a header would allow header injection.
        if (h.first.empty() || h.first.find_first_of("\r\n:") != std::string::npos ||
            h.second.find_first_of("\r\n") != std::string::npos) {
            errorText = "invalid header";
            return ErrorKind::InvalidRequest;
        }
    }
    return ErrorKind::None;
}

BodyReader::BodyReader(const HttpRequest& req, HttpResponse& resp) : req_(req), resp_(resp) {
    const bool inRange = resp.status >= req.sinkMinStatus && resp.status <= req.sinkMaxStatus;
    streaming_ = static_cast<bool>(req.onData) && inRange;
    wants_ = !(req.onData && !inRange && !req.readBodyOutsideSink);
}

bool BodyReader::push(const char* data, std::size_t size) {
    resp_.bytesReceived += size;
    if (streaming_) {
        if (!req_.onData(data, size, resp_.contentLength)) {
            resp_.error = ErrorKind::Cancelled;
            resp_.errorText = "stopped by the consumer";
            return false;
        }
    } else {
        const std::size_t cap = req_.maxResponseBytes;
        if (cap != 0 && resp_.body.size() + size > cap) {
            if (req_.overflow == Overflow::Fail) {
                resp_.error = ErrorKind::TooLarge;
                resp_.errorText = "response too large";
                return false;
            }
            resp_.body.append(data, cap - resp_.body.size());
            return false;  // truncated: stop reading, not an error
        }
        resp_.body.append(data, size);
    }
    if (req_.onProgress && !req_.onProgress(resp_.bytesReceived, resp_.contentLength)) {
        resp_.error = ErrorKind::Cancelled;
        resp_.errorText = "cancelled";
        return false;
    }
    return true;
}

#ifndef _WIN32
namespace {
class UnavailableClient : public IHttpClient {
public:
    HttpResponse send(const HttpRequest&) override {
        HttpResponse r;
        r.error = ErrorKind::Transport;
        r.errorText = "HTTP client unavailable on this platform";
        return r;
    }
};
}  // namespace

std::shared_ptr<IHttpClient> makeWinHttpClient(const HttpClientOptions&) { return std::make_shared<UnavailableClient>(); }
#endif

}  // namespace hb::net
