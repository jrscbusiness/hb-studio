// Non-Windows build: no system TLS backend is wired in (the app itself is Windows-only; the portable
// build exists for tests, which inject a fake ITlsEngine). RTMPS therefore reports a clear error here
// instead of silently falling back to plaintext.
#ifndef _WIN32
#include "streaming/tls_engine.hpp"

namespace hb::streaming {

std::unique_ptr<ITlsEngine> makeSystemTlsEngine() { return nullptr; }

}  // namespace hb::streaming
#endif
