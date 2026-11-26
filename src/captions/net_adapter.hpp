#pragma once
// Adapter: the two network seams of the captions module (IDownloader: streaming GET to a file with progress;
// IHttpPoster: POST of an in-memory body) on top of the shared hb::net::IHttpClient. Portable, so it is
// unit-tested with hb::net::FakeHttpClient. Policy kept here: HTTPS-only downloads, 15/15/30/60 s timeouts,
// size/ completeness checks, 4 MiB cap on transcription replies.
#include <memory>

#include "captions/cloud_recognizer.hpp"
#include "captions/model_manager.hpp"
#include "net/http.hpp"

namespace hb::captions {

class NetBackedHttp : public IDownloader, public IHttpPoster {
public:
    explicit NetBackedHttp(std::shared_ptr<hb::net::IHttpClient> client) : client_(std::move(client)) {}

    bool download(const std::string& url, const std::string& destPathUtf8,
                  const std::function<bool(std::uint64_t received, std::uint64_t total)>& progress,
                  std::string& error) override;
    HttpResponse post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::string& body, int timeoutMs) override;

private:
    std::shared_ptr<hb::net::IHttpClient> client_;
};

}  // namespace hb::captions
