#pragma once
// Portable response-body policy shared by the WinHTTP client and FakeHttpClient, so both apply exactly
// the same streaming / size-cap / progress rules (and the rules are unit-tested without a network).
#include "net/http.hpp"

namespace hb::net {

class BodyReader {
public:
    // `resp.status` and `resp.contentLength` must already be filled in.
    BodyReader(const HttpRequest& req, HttpResponse& resp);

    // False when the status is outside the sink range and the request asked not to read such bodies.
    bool wantsBody() const { return wants_; }
    bool streaming() const { return streaming_; }
    // Feeds one chunk. Returns false when reading must stop (resp.error is set unless it was a plain
    // Overflow::Truncate).
    bool push(const char* data, std::size_t size);

private:
    const HttpRequest& req_;
    HttpResponse& resp_;
    bool streaming_ = false;
    bool wants_ = true;
};

}  // namespace hb::net
