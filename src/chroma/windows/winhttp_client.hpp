#pragma once
// Windows HTTP client of the chroma module: the portable adapter (chroma/net_adapter.cpp) over the shared
// WinHTTP client of src/net. HTTPS only; redirects are followed (Hugging Face answers with a redirect to
// its CDN); integrity is enforced by the caller's SHA-256 check. NOT validated against a real network.
#include <memory>

#include "chroma/model_store.hpp"

namespace hb::chroma {

class WinHttpClient final : public IHttpClient {
public:
    WinHttpClient();
    HttpResult get(const std::string& url,
                   const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>& onData) override {
        return impl_->get(url, onData);
    }

private:
    std::unique_ptr<IHttpClient> impl_;
};

}  // namespace hb::chroma
