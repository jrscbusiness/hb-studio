#include "captions/model_manager.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/sha256.hpp"

namespace hb::captions {
namespace fs = std::filesystem;

namespace {
// Hugging Face repo ggerganov/whisper.cpp pinned to this commit; sizes and SHA-256 values below are
// the LFS object ids reported by the Hugging Face API for that revision (verified when written).
constexpr const char* kHfBase =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/";

fs::path toPath(const std::string& utf8) { return fs::path(reinterpret_cast<const char8_t*>(utf8.c_str())); }
std::string toUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}
}  // namespace

const std::vector<ModelInfo>& modelCatalog() {
    static const std::vector<ModelInfo> kCatalog = {
        {"leve", "Modelo leve (base, quantizado)", "ggml-base-q5_1.bin", std::string(kHfBase) + "ggml-base-q5_1.bin",
         "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898", 59707625ULL, 300,
         "Mais rápido; erra mais com sotaque, ruído e termos incomuns."},
        {"medio", "Modelo médio (small, quantizado)", "ggml-small-q5_1.bin", std::string(kHfBase) + "ggml-small-q5_1.bin",
         "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb", 190085487ULL, 700,
         "Mais preciso, cerca de 3x mais pesado na CPU; exige computador mais forte."},
    };
    return kCatalog;
}

const ModelInfo* findModel(const std::vector<ModelInfo>& catalog, const std::string& id) {
    for (const auto& m : catalog)
        if (m.id == id) return &m;
    return nullptr;
}

ModelManager::ModelManager(std::string dirUtf8, IDownloader* downloader, std::vector<ModelInfo> catalog)
    : dir_(std::move(dirUtf8)), dl_(downloader), catalog_(std::move(catalog)) {}

std::string ModelManager::pathFor(const std::string& id) const {
    const ModelInfo* m = findModel(catalog_, id);
    if (!m || dir_.empty()) return {};
    try {
        return toUtf8(toPath(dir_) / toPath(m->filename));
    } catch (...) {
        return {};
    }
}

bool ModelManager::isInstalled(const std::string& id) const {
    const ModelInfo* m = findModel(catalog_, id);
    if (!m || dir_.empty()) return false;
    std::error_code ec;
    const fs::path p = toPath(dir_) / toPath(m->filename);
    if (!fs::is_regular_file(p, ec)) return false;
    return fs::file_size(p, ec) == m->sizeBytes && !ec;
}

std::string ModelManager::sha256File(const std::string& pathUtf8) {
    try {
        std::ifstream f(toPath(pathUtf8), std::ios::binary);
        if (!f) return {};
        hb::core::Sha256 h;
        std::vector<char> buf(1 << 20);
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = f.gcount();
            if (n > 0) h.update(buf.data(), static_cast<std::size_t>(n));
        }
        return h.hexDigest();
    } catch (...) {
        return {};
    }
}

bool ModelManager::ensure(const std::string& id, const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
                          std::string& error) {
    const ModelInfo* m = findModel(catalog_, id);
    if (!m) { error = "modelo desconhecido"; return false; }
    if (dir_.empty()) { error = "pasta de modelos indisponível"; return false; }
    if (isInstalled(id)) return true;
    if (!dl_) { error = "download indisponível nesta compilação"; return false; }
    try {
        std::error_code ec;
        fs::create_directories(toPath(dir_), ec);
        const fs::path finalPath = toPath(dir_) / toPath(m->filename);
        fs::path part = finalPath;
        part += ".part";
        fs::remove(part, ec);
        const std::string partUtf8 = toUtf8(part);
        if (!dl_->download(m->url, partUtf8, progress, error)) {
            fs::remove(part, ec);
            if (error.empty()) error = "download falhou";
            return false;
        }
        if (fs::file_size(part, ec) != m->sizeBytes || ec) {
            fs::remove(part, ec);
            error = "arquivo baixado com tamanho inesperado";
            return false;
        }
        if (sha256File(partUtf8) != m->sha256) {
            fs::remove(part, ec);
            error = "SHA-256 do modelo não confere; arquivo descartado";
            return false;
        }
        fs::rename(part, finalPath, ec);
        if (ec) {
            fs::remove(part, ec);
            error = "não foi possível salvar o modelo";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("erro: ") + e.what();
        return false;
    }
}

bool ModelManager::remove(const std::string& id) {
    const ModelInfo* m = findModel(catalog_, id);
    if (!m || dir_.empty()) return false;
    std::error_code ec;
    return fs::remove(toPath(dir_) / toPath(m->filename), ec);
}

}  // namespace hb::captions
