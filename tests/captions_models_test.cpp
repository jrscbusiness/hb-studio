#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "captions/model_manager.hpp"
#include "core/sha256.hpp"

using namespace hb::captions;
namespace fs = std::filesystem;

namespace {
struct FakeDownloader : IDownloader {
    std::string payload;
    bool fail = false;
    int calls = 0;
    bool download(const std::string&, const std::string& dest,
                  const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string& error) override {
        ++calls;
        if (fail) { error = "sem rede"; return false; }
        if (!progress(payload.size() / 2, payload.size())) { error = "cancelado"; return false; }
        {
            std::ofstream f(fs::path(reinterpret_cast<const char8_t*>(dest.c_str())), std::ios::binary);
            f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        }
        progress(payload.size(), payload.size());
        return true;
    }
};

struct TmpDir {
    fs::path p;
    TmpDir() {
        p = fs::temp_directory_path() / ("hb_captions_models_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(p);
    }
    ~TmpDir() { std::error_code ec; fs::remove_all(p, ec); }
};

std::vector<ModelInfo> catalogFor(const std::string& payload) {
    ModelInfo m;
    m.id = "t";
    m.filename = "t.bin";
    m.url = "https://example.invalid/t.bin";
    m.sha256 = hb::core::Sha256::hex(payload);
    m.sizeBytes = payload.size();
    return {m};
}
}  // namespace

TEST(CaptionsModels, BuiltInCatalogIsPinned) {
    const auto& c = modelCatalog();
    ASSERT_GE(c.size(), 2u);
    for (const auto& m : c) {
        EXPECT_EQ(m.sha256.size(), 64u);
        EXPECT_EQ(m.url.rfind("https://huggingface.co/ggerganov/whisper.cpp/resolve/", 0), 0u);
        EXPECT_EQ(m.url.find("/main/"), std::string::npos);   // pinned revision, never a moving branch
        EXPECT_GT(m.sizeBytes, 1000000u);
    }
    EXPECT_NE(findModel(c, "leve"), nullptr);
    EXPECT_NE(findModel(c, "medio"), nullptr);
    EXPECT_EQ(findModel(c, "nao-existe"), nullptr);
}

TEST(CaptionsModels, DownloadVerifyAndInstall) {
    TmpDir d;
    FakeDownloader dl;
    dl.payload = std::string(5000, 'x') + "fim";
    const std::string dir = (d.p / "models").string();
    ModelManager mm(dir, &dl, catalogFor(dl.payload));
    EXPECT_FALSE(mm.isInstalled("t"));
    std::uint64_t lastGot = 0;
    std::string err;
    ASSERT_TRUE(mm.ensure("t", [&](std::uint64_t got, std::uint64_t) { lastGot = got; return true; }, err)) << err;
    EXPECT_EQ(lastGot, dl.payload.size());
    EXPECT_TRUE(mm.isInstalled("t"));
    EXPECT_FALSE(fs::exists(mm.pathFor("t") + ".part"));
    EXPECT_EQ(ModelManager::sha256File(mm.pathFor("t")), hb::core::Sha256::hex(dl.payload));
    EXPECT_TRUE(mm.ensure("t", nullptr, err));   // idempotent
    EXPECT_EQ(dl.calls, 1);
    EXPECT_TRUE(mm.remove("t"));
    EXPECT_FALSE(mm.isInstalled("t"));
}

TEST(CaptionsModels, CorruptDownloadIsDiscarded) {
    TmpDir d;
    FakeDownloader dl;
    dl.payload = "conteudo adulterado";
    auto cat = catalogFor("conteudo original!");   // same size class, different hash/size
    cat[0].sizeBytes = dl.payload.size();           // size matches, SHA does not
    ModelManager mm(d.p.string(), &dl, cat);
    std::string err;
    EXPECT_FALSE(mm.ensure("t", nullptr, err));
    EXPECT_NE(err.find("SHA-256"), std::string::npos);
    EXPECT_FALSE(mm.isInstalled("t"));
    EXPECT_FALSE(fs::exists(mm.pathFor("t") + ".part"));
}

TEST(CaptionsModels, WrongSizeNetworkFailureAndCancel) {
    TmpDir d;
    FakeDownloader dl;
    dl.payload = "abcdef";
    auto cat = catalogFor(dl.payload);
    cat[0].sizeBytes = 99;
    ModelManager mm(d.p.string(), &dl, cat);
    std::string err;
    EXPECT_FALSE(mm.ensure("t", nullptr, err));
    EXPECT_NE(err.find("tamanho"), std::string::npos);

    ModelManager mm2(d.p.string(), &dl, catalogFor(dl.payload));
    dl.fail = true;
    EXPECT_FALSE(mm2.ensure("t", nullptr, err));
    EXPECT_EQ(err, "sem rede");
    dl.fail = false;
    EXPECT_FALSE(mm2.ensure("t", [](std::uint64_t, std::uint64_t) { return false; }, err));
    EXPECT_EQ(err, "cancelado");
    EXPECT_FALSE(mm2.ensure("desconhecido", nullptr, err));
    EXPECT_EQ(mm2.pathFor("desconhecido"), "");
}

TEST(CaptionsModels, NoDownloaderOrDir) {
    ModelManager mm("", nullptr, catalogFor("a"));
    std::string err;
    EXPECT_FALSE(mm.ensure("t", nullptr, err));
    EXPECT_FALSE(mm.isInstalled("t"));
    TmpDir d;
    ModelManager mm2(d.p.string(), nullptr, catalogFor("a"));
    EXPECT_FALSE(mm2.ensure("t", nullptr, err));
}
