#pragma once
// On-demand download of the segmentation model into %LOCALAPPDATA%\HBStudio\models.
// The model is NOT committed to the repository and NOT bundled in the installer: it is fetched
// once, only after the operator asks, from a commit-pinned URL and accepted ONLY if its SHA-256
// matches the pinned value (anything else is deleted). HTTP is behind IHttpClient so the logic is
// unit-tested with a fake; the Windows implementation (WinHTTP) lives in src/chroma/windows/.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hb::chroma {

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string error;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    // GET `url` (https only; redirects are followed by the implementation). `onData(ptr, len, total)`
    // is called for each body chunk (`total` = Content-Length or 0 if unknown); returning false aborts
    // the transfer. Blocking: run it on a worker thread.
    virtual HttpResult get(const std::string& url,
                           const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>& onData) = 0;
};

struct ModelInfo {
    std::string id;           // short stable id
    std::string fileName;     // file name inside the models dir
    std::string url;          // https, commit-pinned
    std::string sha256;       // 64 lowercase hex chars; anything else => model "not configured"
    std::uint64_t sizeBytes;  // expected size (hard cap = 2x this)
    std::string license;      // human readable
    std::string description;
};

// The one model this build knows: MediaPipe Selfie Segmentation (landscape, 256x144 input) in the
// ONNX conversion published by onnx-community on Hugging Face, Apache-2.0. See docs/CHROMA.md.
const ModelInfo& defaultModel();

bool isValidSha256Hex(const std::string& s);
// https:// only, no userinfo, no whitespace/control characters, bounded length.
bool isAcceptableModelUrl(const std::string& url);

class ModelStore {
public:
    enum class State { Unavailable, NotDownloaded, Downloading, Ready, Error };
    struct Status {
        State state = State::NotDownloaded;
        std::uint64_t received = 0, total = 0;
        std::string message;   // pt-BR, human readable (errors / hints)
    };

    ModelStore(std::string dir, ModelInfo info, IHttpClient* http);   // `http` may be null (download unavailable)
    ~ModelStore();

    // Verifies an existing file against the pinned hash (cheap: sub-MB model). Call at startup.
    void refresh();
    // Starts a background download (no-op if already downloading/ready). Returns false with `why` if it cannot start.
    bool startDownload(std::string* why = nullptr);
    void cancel();
    void waitIdle();   // joins the worker thread (tests / shutdown)

    Status status() const;
    bool ready() const { return status().state == State::Ready; }
    std::string path() const;      // full path of the model file (valid when ready())
    const ModelInfo& info() const { return info_; }

    // Deletes the downloaded model (not while downloading).
    bool remove();

private:
    void run();
    void setStatus(State s, std::uint64_t rcv, std::uint64_t tot, std::string msg);
    static bool hashFile(const std::string& path, std::string& hex, std::uint64_t maxBytes);

    std::string dir_;
    ModelInfo info_;
    IHttpClient* http_;
    mutable std::mutex m_;
    Status st_;
    std::atomic<bool> cancel_{false};
    std::thread th_;
};

}  // namespace hb::chroma
