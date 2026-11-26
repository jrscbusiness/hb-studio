#include "net/http_fake.hpp"

#include <algorithm>
#include <cstdlib>

#include "net/body_reader.hpp"
#include "net/url.hpp"

namespace hb::net {

void FakeHttpClient::enqueue(Reply r) {
    std::lock_guard lock(m_);
    queue_.push_back(std::move(r));
}

void FakeHttpClient::setHandler(Handler h) {
    std::lock_guard lock(m_);
    handler_ = std::move(h);
}

std::vector<HttpRequest> FakeHttpClient::requests() const {
    std::lock_guard lock(m_);
    return requests_;
}

std::vector<std::string> FakeHttpClient::uploads() const {
    std::lock_guard lock(m_);
    return uploads_;
}

std::size_t FakeHttpClient::requestCount() const {
    std::lock_guard lock(m_);
    return requests_.size();
}

HttpResponse FakeHttpClient::send(const HttpRequest& req) {
    HttpResponse resp;
    Url url;
    resp.error = validateRequest(req, url, resp.errorText);
    if (resp.error != ErrorKind::None) return resp;

    std::string uploaded;
    if (req.bodySource) {
        std::string buf(std::max<std::size_t>(req.bodySourceChunk, 1), '\0');
        while (uploaded.size() < req.bodySourceLength) {
            const std::size_t n = req.bodySource(&buf[0], buf.size());
            if (n == kBodyAbort) {
                resp.error = ErrorKind::Cancelled;
                resp.errorText = "upload aborted by the body source";
                std::lock_guard lock(m_);
                requests_.push_back(req);
                uploads_.push_back(uploaded);
                return resp;
            }
            if (n == 0) break;
            uploaded.append(buf.data(), std::min(n, buf.size()));
        }
    } else {
        uploaded = req.body;
    }

    Reply reply;
    Handler handler;
    {
        std::lock_guard lock(m_);
        requests_.push_back(req);
        uploads_.push_back(uploaded);
        if (!queue_.empty()) {
            reply = std::move(queue_.front());
            queue_.pop_front();
        } else if (handler_) {
            handler = handler_;
        } else {
            reply.error = ErrorKind::Transport;
            reply.errorText = "no scripted reply";
        }
    }
    if (handler) reply = handler(req);

    if (reply.error != ErrorKind::None) {
        resp.error = reply.error;
        resp.errorText = reply.errorText;
        resp.failure = reply.failure;
        resp.nativeError = reply.nativeError;
        return resp;
    }
    resp.status = reply.status;
    resp.headers = reply.headers;
    {
        const std::string cl = resp.header("Content-Length");
        if (!cl.empty()) resp.contentLength = std::strtoull(cl.c_str(), nullptr, 10);
    }
    BodyReader reader(req, resp);
    if (!reader.wantsBody()) return resp;
    const std::size_t step = reply.chunk ? reply.chunk : std::max<std::size_t>(reply.body.size(), 1);
    for (std::size_t off = 0; off < reply.body.size(); off += step) {
        if (!reader.push(reply.body.data() + off, std::min(step, reply.body.size() - off))) break;
    }
    return resp;
}

}  // namespace hb::net
