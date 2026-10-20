#pragma once
// Fake transport for tests: records what was sent and delivers scripted replies.
#include <deque>
#include <functional>

#include "ptz/transport.hpp"

namespace hb::ptz {

class FakeTransport : public ITransport {
public:
    // Called on every send(); may queue replies with pushIncoming().
    std::function<void(const Bytes&, FakeTransport&)> onSend;
    std::vector<Bytes> sent;
    bool failOpen = false;
    bool failSend = false;

    TransportResult open() override {
        if (failOpen) return TransportResult::fail("open failed");
        open_ = true;
        return TransportResult::success();
    }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    TransportResult send(const Bytes& d) override {
        if (!open_) return TransportResult::fail("not open");
        if (failSend) return TransportResult::fail("send failed");
        sent.push_back(d);
        if (onSend) onSend(d, *this);
        return TransportResult::success();
    }
    TransportResult receive(Bytes& out, Millis) override {
        if (incoming_.empty()) return TransportResult::timeout();
        out = std::move(incoming_.front());
        incoming_.pop_front();
        return TransportResult::success();
    }
    void pushIncoming(Bytes b) { incoming_.push_back(std::move(b)); }

private:
    bool open_ = false;
    std::deque<Bytes> incoming_;
};

}  // namespace hb::ptz
