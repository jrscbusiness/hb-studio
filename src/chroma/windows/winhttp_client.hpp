#pragma once
// WinHTTP implementation of IHttpClient (Windows only; NOT validated against a real network by the
// author of this module - see docs/CHROMA.md). HTTPS only; redirects are followed (Hugging Face
// answers with a redirect to its CDN); integrity is enforced by the caller's SHA-256 check.
#include "chroma/model_store.hpp"

namespace hb::chroma {

class WinHttpClient final : public IHttpClient {
public:
    HttpResult get(const std::string& url,
                   const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>& onData) override;
};

}  // namespace hb::chroma
