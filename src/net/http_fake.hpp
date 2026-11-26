#pragma once
// In-memory IHttpClient for tests: records every request and answers from a queue of scripted replies
// (or a handler). It runs the same request validation and body policy as the WinHTTP client
// (validateRequest + BodyReader), so adapters can be tested end to end without a network.
#include <deque>
#include <mutex>

#include "net/http.hpp"

namespace hb::net {

class FakeHttpClient : public IHttpClient {
public:
    struct Reply {
        int status = 200;
        Headers headers;
        std::string body;
        std::size_t chunk = 0;       // deliver the body in pieces of this size (0 = one piece)
        ErrorKind error = ErrorKind::None;  // scripted transport failure (status stays 0)
        std::string errorText = "scripted failure";
    };
    using Handler = std::function<Reply(const HttpRequest&)>;

    void enqueue(Reply r);
    void setHandler(Handler h);  // used when the queue is empty

    // Everything the client was asked to send, in order. `uploads[i]` is the request body actually produced
    // (the `body` string, or whatever the body source delivered).
    std::vector<HttpRequest> requests() const;
    std::vector<std::string> uploads() const;
    std::size_t requestCount() const;

    HttpResponse send(const HttpRequest& request) override;

private:
    mutable std::mutex m_;
    std::deque<Reply> queue_;
    Handler handler_;
    std::vector<HttpRequest> requests_;
    std::vector<std::string> uploads_;
};

}  // namespace hb::net
