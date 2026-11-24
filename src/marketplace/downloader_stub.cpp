#ifndef _WIN32
#include "marketplace/downloader.hpp"

namespace hb::marketplace {
namespace {
class UnsupportedDownloader : public IHttpDownloader {
public:
    FetchResponse fetch(const FetchRequest&) override {
        FetchResponse r;
        r.error = "download HTTPS indisponível nesta plataforma";
        return r;
    }
    FileResult downloadToFile(const FileRequest&, const ProgressFn&) override {
        FileResult r;
        r.error = "download HTTPS indisponível nesta plataforma";
        return r;
    }
};
}  // namespace
std::unique_ptr<IHttpDownloader> makeDefaultDownloader() { return std::make_unique<UnsupportedDownloader>(); }
}  // namespace hb::marketplace
#endif
