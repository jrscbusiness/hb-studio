#include "chroma/model_store.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/sha256.hpp"

namespace hb::chroma {

const ModelInfo& defaultModel() {
    // Values verified by hand when this module was written: the file at this exact commit hashes to
    // the SHA-256 below (462338 bytes). The model card declares Apache-2.0 (derived from Google's
    // MediaPipe Selfie Segmentation, Apache-2.0).
    static const ModelInfo m{
        "mediapipe-selfie-landscape-v1",
        "mediapipe_selfie_segmentation_landscape_2497d5b.onnx",
        "https://huggingface.co/onnx-community/mediapipe_selfie_segmentation_landscape/resolve/"
        "2497d5bec26c626c7b3c4edc6e1fefc21b64f6c3/onnx/model.onnx",
        "7a0adcfdb1715d3b0ff0f61486d8a181c33f4a208343a5abfccbc6540e872d24",
        462338,
        "Apache-2.0 (MediaPipe Selfie Segmentation, Google; conversão ONNX: onnx-community)",
        "Segmentação de pessoa 256x144, ~0,45 MB"};
    return m;
}

bool isValidSha256Hex(const std::string& s) {
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isAcceptableModelUrl(const std::string& url) {
    static const std::string pre = "https://";
    if (url.size() <= pre.size() || url.size() > 1024 || url.compare(0, pre.size(), pre) != 0) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7F || c == '\\') return false;
    const std::size_t slash = url.find('/', pre.size());
    const std::string authority = url.substr(pre.size(), slash == std::string::npos ? std::string::npos : slash - pre.size());
    if (authority.empty() || authority.find('@') != std::string::npos) return false;   // no userinfo tricks
    return true;
}

ModelStore::ModelStore(std::string dir, ModelInfo info, IHttpClient* http)
    : dir_(std::move(dir)), info_(std::move(info)), http_(http) {
    if (!isValidSha256Hex(info_.sha256) || !isAcceptableModelUrl(info_.url)) {
        st_.state = State::Unavailable;
        st_.message = "Modelo de IA não configurado neste build (hash/URL fixos ausentes).";
    }
}

ModelStore::~ModelStore() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
}

void ModelStore::setStatus(State s, std::uint64_t rcv, std::uint64_t tot, std::string msg) {
    std::lock_guard lock(m_);
    st_.state = s;
    st_.received = rcv;
    st_.total = tot;
    st_.message = std::move(msg);
}

ModelStore::Status ModelStore::status() const {
    std::lock_guard lock(m_);
    return st_;
}

std::string ModelStore::path() const {
    return (std::filesystem::path(dir_) / info_.fileName).string();
}

bool ModelStore::hashFile(const std::string& path, std::string& hex, std::uint64_t maxBytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    core::Sha256 h;
    char buf[16384];
    std::uint64_t total = 0;
    while (f) {
        f.read(buf, sizeof buf);
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        total += static_cast<std::uint64_t>(n);
        if (total > maxBytes) return false;
        h.update(buf, static_cast<std::size_t>(n));
    }
    hex = h.hexDigest();
    return true;
}

void ModelStore::refresh() {
    {
        std::lock_guard lock(m_);
        if (st_.state == State::Unavailable || st_.state == State::Downloading) return;
    }
    std::string hex;
    std::error_code ec;
    const std::string p = path();
    if (!std::filesystem::exists(p, ec)) {
        setStatus(State::NotDownloaded, 0, 0, "");
        return;
    }
    if (hashFile(p, hex, info_.sizeBytes * 2) && hex == info_.sha256) {
        setStatus(State::Ready, info_.sizeBytes, info_.sizeBytes, "");
    } else {
        // Present but not what we pinned (corrupted/tampered/partially written): never trust it.
        std::filesystem::remove(p, ec);
        setStatus(State::NotDownloaded, 0, 0, "Arquivo do modelo inválido foi removido; baixe novamente.");
    }
}

bool ModelStore::startDownload(std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    {
        std::lock_guard lock(m_);
        if (st_.state == State::Unavailable) return fail("modelo não configurado neste build");
        if (st_.state == State::Downloading) return fail("download já em andamento");
        if (st_.state == State::Ready) return fail("modelo já instalado");
        if (!http_) return fail("sem cliente HTTP nesta plataforma");
        st_.state = State::Downloading;
        st_.received = 0;
        st_.total = info_.sizeBytes;
        st_.message = "";
    }
    if (th_.joinable()) th_.join();   // previous (finished) run
    cancel_ = false;
    th_ = std::thread([this] { run(); });
    return true;
}

void ModelStore::cancel() {
    cancel_ = true;
}

void ModelStore::waitIdle() {
    if (th_.joinable()) th_.join();
}

bool ModelStore::remove() {
    {
        std::lock_guard lock(m_);
        if (st_.state == State::Downloading || st_.state == State::Unavailable) return false;
    }
    std::error_code ec;
    std::filesystem::remove(path(), ec);
    setStatus(State::NotDownloaded, 0, 0, "");
    return !ec;
}

void ModelStore::run() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        setStatus(State::Error, 0, 0, "Não foi possível criar a pasta de modelos: " + ec.message());
        return;
    }
    const std::string finalPath = path();
    const std::string part = finalPath + ".part";
    const std::uint64_t cap = info_.sizeBytes * 2 + 4096;   // hard cap: a hostile/misconfigured server cannot fill the disk
    std::uint64_t received = 0;
    bool overflow = false, writeFail = false;
    core::Sha256 hash;
    HttpResult r;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            setStatus(State::Error, 0, 0, "Não foi possível gravar o arquivo temporário do modelo.");
            return;
        }
        r = http_->get(info_.url, [&](const std::uint8_t* p, std::size_t n, std::uint64_t total) {
            if (cancel_) return false;
            if (received + n > cap) { overflow = true; return false; }
            out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
            if (!out) { writeFail = true; return false; }
            hash.update(p, n);
            received += n;
            std::lock_guard lock(m_);
            st_.received = received;
            if (total > 0) st_.total = total;
            return true;
        });
    }
    auto cleanup = [&] { std::error_code e2; fs::remove(part, e2); };
    if (cancel_) { cleanup(); setStatus(State::NotDownloaded, 0, 0, "Download cancelado."); return; }
    if (overflow) { cleanup(); setStatus(State::Error, 0, 0, "O servidor enviou mais dados do que o esperado; download abortado."); return; }
    if (writeFail) { cleanup(); setStatus(State::Error, 0, 0, "Falha ao gravar o modelo no disco (espaço insuficiente?)."); return; }
    if (!r.ok || r.status != 200) {
        cleanup();
        setStatus(State::Error, 0, 0,
                  "Falha no download: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) +
                      ". Verifique a internet e tente de novo.");
        return;
    }
    const std::string hex = hash.hexDigest();
    if (received != info_.sizeBytes || hex != info_.sha256) {
        cleanup();
        setStatus(State::Error, 0, 0, "O arquivo baixado não confere com a assinatura (SHA-256) esperada e foi descartado.");
        return;
    }
    fs::remove(finalPath, ec);
    fs::rename(part, finalPath, ec);
    if (ec) {
        cleanup();
        setStatus(State::Error, 0, 0, "Não foi possível finalizar o arquivo do modelo: " + ec.message());
        return;
    }
    setStatus(State::Ready, received, received, "");
}

}  // namespace hb::chroma
