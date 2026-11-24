#pragma once
// One-shot HTTP listener bound to 127.0.0.1 only, used to catch the OAuth redirect.
#include <functional>
#include <memory>
#include <string>

namespace hb::cloud {

class ILoopbackListener {
public:
    virtual ~ILoopbackListener() = default;
    // Binds 127.0.0.1 on an ephemeral port; returns the port or -1.
    virtual int start() = 0;
    // Accepts ONE connection and reads its request line. false on timeout, cancel or error.
    virtual bool waitRequestLine(std::string& line, int timeoutMs, const std::function<bool()>& cancelled) = 0;
    // Answers the pending connection with a small HTML page and closes it.
    virtual void respond(int status, const std::string& htmlUtf8) = 0;
    virtual void close() = 0;
};

// BSD sockets / Winsock. The POSIX branch is exercised by the unit tests; the Windows branch is NOT VALIDATED.
std::unique_ptr<ILoopbackListener> makeLoopbackListener();

}  // namespace hb::cloud
