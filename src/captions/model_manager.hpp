#pragma once
// whisper.cpp ggml model catalogue + on-demand download with pinned SHA-256. Models are NOT
// committed and NOT bundled: they are fetched from a pinned Hugging Face revision of
// ggerganov/whisper.cpp into %LOCALAPPDATA%\HBStudio\models (the caller passes the directory).
// A model file is only ever used after its SHA-256 matched the pinned value; a mismatch deletes it.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hb::captions {

struct ModelInfo {
    std::string id;          // "leve" | "medio"
    std::string label;       // UI label (pt-BR)
    std::string filename;
    std::string url;
    std::string sha256;      // lowercase hex, 64 chars
    std::uint64_t sizeBytes = 0;
    int approxRamMb = 0;     // rough resident memory while loaded (estimate, not measured here)
    std::string note;
};

// Built-in catalogue (see docs/LEGENDAS.md for how the values were obtained).
const std::vector<ModelInfo>& modelCatalog();
const ModelInfo* findModel(const std::vector<ModelInfo>& catalog, const std::string& id);

// Download transport (WinHTTP on Windows; a fake in tests). Writes the body to destPathUtf8,
// calling progress(received, total) (total 0 = unknown); progress returning false cancels.
class IDownloader {
public:
    virtual ~IDownloader() = default;
    virtual bool download(const std::string& url, const std::string& destPathUtf8,
                          const std::function<bool(std::uint64_t received, std::uint64_t total)>& progress,
                          std::string& error) = 0;
};

class ModelManager {
public:
    ModelManager(std::string dirUtf8, IDownloader* downloader, std::vector<ModelInfo> catalog = modelCatalog());

    const std::vector<ModelInfo>& catalog() const { return catalog_; }
    std::string pathFor(const std::string& id) const;   // "" if unknown id
    // Cheap check: file exists with the expected size (full hash is verified right after download).
    bool isInstalled(const std::string& id) const;
    // Downloads to "<file>.part", verifies size + SHA-256, then renames. Idempotent when installed.
    bool ensure(const std::string& id, const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string& error);
    bool remove(const std::string& id);

    // Streams a file through SHA-256 (1 MiB reads). Returns "" if it cannot be read.
    static std::string sha256File(const std::string& pathUtf8);

private:
    std::string dir_;
    IDownloader* dl_;
    std::vector<ModelInfo> catalog_;
};

}  // namespace hb::captions
