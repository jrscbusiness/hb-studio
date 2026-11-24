#pragma once
// WinHTTP implementation of the two network seams of the captions module:
//   IDownloader  - streaming GET to a file with progress (model download from Hugging Face; redirects followed)
//   IHttpPoster  - POST of an in-memory body (OpenAI-compatible transcription endpoint)
// NOT VALIDATED on Windows: written without a Windows compiler (see docs/LEGENDAS.md).
#ifdef _WIN32
#include "captions/cloud_recognizer.hpp"
#include "captions/model_manager.hpp"

namespace hb::captions::win {

class WinHttpClient : public IDownloader, public IHttpPoster {
public:
    bool download(const std::string& url, const std::string& destPathUtf8,
                  const std::function<bool(std::uint64_t received, std::uint64_t total)>& progress,
                  std::string& error) override;
    HttpResponse post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::string& body, int timeoutMs) override;
};

}  // namespace hb::captions::win
#endif
