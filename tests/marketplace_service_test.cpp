#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "core/json.hpp"
#include "core/sha256.hpp"
#include "marketplace/service.hpp"
#include "marketplace_test_util.hpp"

using namespace hb;
using namespace hb::marketplace;
using namespace hb::mkttest;
namespace fs = std::filesystem;

namespace {

struct FakeHttp : IHttpDownloader {
    // catalog
    FetchResponse catalogResponse;
    int fetches = 0;
    std::string lastIfNoneMatch;
    // files: url -> bytes (missing = transport failure)
    std::map<std::string, std::string> files;
    std::vector<std::string> requestedUrls;
    std::uint64_t lastMaxBytes = 0;
    bool cancelAfterFirstChunk = false;

    FetchResponse fetch(const FetchRequest& r) override {
        ++fetches;
        lastIfNoneMatch = r.ifNoneMatch;
        return catalogResponse;
    }
    FileResult downloadToFile(const FileRequest& r, const ProgressFn& progress) override {
        requestedUrls.push_back(r.url);
        lastMaxBytes = r.maxBytes;
        FileResult out;
        auto it = files.find(r.url);
        if (it == files.end()) {
            out.error = "servidor não encontrado";
            return out;
        }
        if (it->second.size() > r.maxBytes) {
            out.error = "o arquivo é maior que o tamanho declarado no catálogo";
            return out;
        }
        const std::size_t half = it->second.size() / 2;
        if (progress && !progress(half, it->second.size())) {
            out.cancelled = true;
            out.error = "cancelado";
            return out;
        }
        std::ofstream f(fs::path(std::u8string(r.destPathUtf8.begin(), r.destPathUtf8.end())), std::ios::binary);
        f << it->second;
        f.close();
        if (progress && !progress(it->second.size(), it->second.size())) {
            std::error_code ec;
            fs::remove(fs::path(std::u8string(r.destPathUtf8.begin(), r.destPathUtf8.end())), ec);
            out.cancelled = true;
            out.error = "cancelado";
            return out;
        }
        out.ok = true;
        out.status = 200;
        out.bytes = it->second.size();
        return out;
    }
};

FetchResponse ok200(const std::string& body, const std::string& etag = "\"v1\"") {
    FetchResponse r;
    r.transportOk = true;
    r.status = 200;
    r.body = body;
    r.etag = etag;
    return r;
}

struct Fixture {
    fs::path root;
    FakeHttp http;
    std::map<std::string, std::string> installed;
    std::string reviewReply;   // what the fake plugin service returns
    std::string reviewedPath;
    std::vector<std::string> cancelled;
    std::unique_ptr<MarketplaceService> svc;
    const std::string pkg = "PK-fake-package-bytes";
    std::string pkgSha;

    Fixture() {
        root = fs::temp_directory_path() / ("hbmkt-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(root);
        pkgSha = core::Sha256::hex(pkg);
        http.files["https://example.com/tema.hbplugin"] = pkg;
        http.catalogResponse = ok200(catalog(themeEntry("com.example.tema", pkgSha, pkg.size())));
        reviewReply = review("com.example.tema", "theme", "1.0.0", pkgSha);
        make();
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static std::string review(const std::string& id, const std::string& type, const std::string& ver, const std::string& sha) {
        return R"({"t":"plugin_review","ok":true,"token":"tok1","id":")" + id + R"(","type":")" + type + R"(","version":")" + ver + R"(","sha":")" +
               sha.substr(0, 16) + R"("})";
    }
    void make() {
        MarketplaceConfig c;
        c.catalogUrl = "https://example.com/index.json";
        c.cacheDirUtf8 = (root / "cache").string();
        c.downloadDirUtf8 = (root / "dl").string();
        c.appVersion = "0.3.0+abc";
        PluginHooks h;
        h.installedVersions = [this] { return installed; };
        h.beginInstall = [this](const std::string& p) {
            reviewedPath = p;
            return reviewReply;
        };
        h.cancelInstall = [this](const std::string& t) { cancelled.push_back(t); };
        svc = std::make_unique<MarketplaceService>(http, c, h);
    }
    core::json::Value json(const std::string& s) {
        core::json::Value v;
        EXPECT_TRUE(core::json::parse(s, v)) << s;
        return v;
    }
};

}  // namespace

TEST(MarketplaceService, RefreshShowsCatalogWithInstallStatus) {
    Fixture f;
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("loaded")->b);
    EXPECT_FALSE(j.get("offline")->b);
    ASSERT_EQ(j.get("list")->a.size(), 1u);
    EXPECT_EQ(j.get("list")->a[0].get("status")->s, "available");
    EXPECT_EQ(j.get("list")->a[0].get("desc")->s, "Um tema");
    EXPECT_FALSE(j.get("list")->a[0].get("native")->b);
    f.installed["com.example.tema"] = "0.5.0";
    EXPECT_EQ(f.json(f.svc->catalogJson()).get("list")->a[0].get("status")->s, "update");
    f.installed["com.example.tema"] = "1.0.0";
    EXPECT_EQ(f.json(f.svc->catalogJson()).get("list")->a[0].get("status")->s, "installed");
}

TEST(MarketplaceService, EtagCachingAndNotModified) {
    Fixture f;
    f.svc->refresh();
    EXPECT_EQ(f.http.lastIfNoneMatch, "");                       // nothing cached yet
    FetchResponse nm;
    nm.transportOk = true;
    nm.status = 304;
    f.http.catalogResponse = nm;
    f.svc->refresh();
    EXPECT_EQ(f.http.lastIfNoneMatch, "\"v1\"");                 // conditional request
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("loaded")->b);
    EXPECT_TRUE(j.get("fromCache")->b);
    EXPECT_FALSE(j.get("offline")->b);
    EXPECT_EQ(j.get("list")->a.size(), 1u);
}

TEST(MarketplaceService, OfflineFallsBackToLastGoodCopyAcrossRestarts) {
    Fixture f;
    f.svc->refresh();
    f.http.catalogResponse = FetchResponse{};                    // transport failure
    f.http.catalogResponse.error = "sem internet";
    f.make();                                                    // "restart": new service, same cache dir
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("loaded")->b);
    EXPECT_TRUE(j.get("offline")->b);
    EXPECT_EQ(j.get("list")->a.size(), 1u);
    EXPECT_NE(j.get("error")->s.find("última cópia"), std::string::npos);
}

TEST(MarketplaceService, OfflineWithoutCacheIsAGracefulEmptyState) {
    Fixture f;
    f.http.catalogResponse = FetchResponse{};
    f.http.catalogResponse.error = "tempo esgotado";
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_FALSE(j.get("loaded")->b);
    EXPECT_NE(j.get("error")->s.find("tempo esgotado"), std::string::npos);
    EXPECT_TRUE(j.get("list")->a.empty());
}

TEST(MarketplaceService, InvalidCatalogKeepsThePreviousCopy) {
    Fixture f;
    f.svc->refresh();
    f.http.catalogResponse = ok200("<html>captive portal</html>");
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("loaded")->b);
    EXPECT_NE(j.get("error")->s.find("inválido"), std::string::npos);
    EXPECT_EQ(j.get("list")->a.size(), 1u);
    // and the bad body must not have replaced the cache
    f.make();
    FetchResponse nm;
    nm.transportOk = true;
    nm.status = 304;
    f.http.catalogResponse = nm;
    f.svc->refresh();
    EXPECT_EQ(f.json(f.svc->catalogJson()).get("list")->a.size(), 1u);
}

TEST(MarketplaceService, HttpErrorStatusUsesCache) {
    Fixture f;
    f.svc->refresh();
    FetchResponse e;
    e.transportOk = true;
    e.status = 503;
    f.http.catalogResponse = e;
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("offline")->b);
    EXPECT_NE(j.get("error")->s.find("503"), std::string::npos);
}

TEST(MarketplaceService, CacheFromAnotherCatalogUrlIsNotUsed) {
    Fixture f;
    f.svc->refresh();
    ASSERT_TRUE(f.svc->setCatalogUrl("https://other.example.org/index.json"));
    f.http.catalogResponse = FetchResponse{};
    f.http.catalogResponse.error = "offline";
    f.svc->refresh();
    EXPECT_FALSE(f.json(f.svc->catalogJson()).get("loaded")->b);
}

TEST(MarketplaceService, CatalogUrlMustBeHttps) {
    Fixture f;
    EXPECT_FALSE(f.svc->setCatalogUrl("http://example.com/index.json"));
    EXPECT_FALSE(f.svc->setCatalogUrl("file:///etc/passwd"));
    EXPECT_EQ(f.svc->catalogUrl(), "https://example.com/index.json");
    EXPECT_TRUE(f.svc->setCatalogUrl(""));                                   // reset to default
    EXPECT_EQ(f.svc->catalogUrl(), kDefaultCatalogUrl);
}

TEST(MarketplaceService, InstallDownloadsVerifiesAndHandsOverToTheReviewDialog) {
    Fixture f;
    f.svc->refresh();
    std::vector<std::string> phases;
    auto out = f.svc->install("com.example.tema", [&](const std::string& p, int) { if (phases.empty() || phases.back() != p) phases.push_back(p); });
    ASSERT_TRUE(out.ok) << out.message;
    EXPECT_EQ(f.http.lastMaxBytes, f.pkg.size());                        // the declared size is the hard cap
    EXPECT_FALSE(f.reviewedPath.empty());
    EXPECT_EQ(fs::path(f.reviewedPath).extension(), ".hbplugin");
    EXPECT_TRUE(fs::exists(fs::path(f.reviewedPath)));                   // kept until the user confirms
    EXPECT_EQ(f.json(out.reviewJson).get("token")->s, "tok1");
    auto ctx = f.json(out.contextJson);
    EXPECT_FALSE(ctx.get("verified")->b);                               // "publisher not verified" is conveyed to the UI
    EXPECT_FALSE(ctx.get("native")->b);
    EXPECT_EQ(phases, (std::vector<std::string>{"download", "verify", "review"}));
    EXPECT_FALSE(f.svc->installing());
}

TEST(MarketplaceService, HashMismatchAbortsBeforeTheReviewDialog) {
    Fixture f;
    f.http.files["https://example.com/tema.hbplugin"] = "PK-tampered-package!!";
    ASSERT_EQ(f.http.files["https://example.com/tema.hbplugin"].size(), f.pkg.size());
    f.svc->refresh();
    f.reviewedPath.clear();
    auto out = f.svc->install("com.example.tema", {});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.message.find("SHA-256"), std::string::npos);
    EXPECT_TRUE(f.reviewedPath.empty());                                // beginInstall never called
    EXPECT_TRUE(out.reviewJson.empty());
    int left = 0;
    for (auto& e : fs::directory_iterator(f.root / "dl")) { (void)e; ++left; }
    EXPECT_EQ(left, 0);                                                 // the bad file was deleted
}

TEST(MarketplaceService, OversizedOrWrongSizeDownloadIsRejected) {
    Fixture f;
    f.http.files["https://example.com/tema.hbplugin"] = f.pkg + std::string(1000, 'x');   // bigger than declared
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", {});
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(f.reviewedPath.empty());
    f.http.files["https://example.com/tema.hbplugin"] = f.pkg.substr(0, 5);                 // smaller than declared
    out = f.svc->install("com.example.tema", {});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.message.find("tamanho"), std::string::npos);
    EXPECT_TRUE(f.reviewedPath.empty());
}

TEST(MarketplaceService, MirrorsAreTriedInOrder) {
    Fixture f;
    std::string e = replaceOnce(themeEntry("com.example.tema", f.pkgSha, f.pkg.size()), R"("downloadUrls":["https://example.com/tema.hbplugin"])",
                                R"("downloadUrls":["https://down.example.com/a","https://example.com/tema.hbplugin"])");
    f.http.catalogResponse = ok200(catalog(e));
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", {});
    ASSERT_TRUE(out.ok) << out.message;
    ASSERT_EQ(f.http.requestedUrls.size(), 2u);
    EXPECT_EQ(f.http.requestedUrls[0], "https://down.example.com/a");
}

TEST(MarketplaceService, DownloadFailureIsReportedInPortuguese) {
    Fixture f;
    f.http.files.clear();
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", {});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.message.find("Falha no download"), std::string::npos);
    EXPECT_TRUE(f.reviewedPath.empty());
}

TEST(MarketplaceService, CancelStopsTheDownload) {
    Fixture f;
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", [&](const std::string&, int) { f.svc->cancelInstall(); });
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.message.find("cancelado"), std::string::npos);
    EXPECT_TRUE(f.reviewedPath.empty());
}

TEST(MarketplaceService, PackageThatDoesNotMatchTheCatalogIsRefusedAndTheTokenCancelled) {
    for (const auto& reply : {Fixture::review("com.evil.other", "theme", "1.0.0", core::Sha256::hex("PK-fake-package-bytes")),
                              Fixture::review("com.example.tema", "source", "1.0.0", core::Sha256::hex("PK-fake-package-bytes")),   // claims native code
                              Fixture::review("com.example.tema", "theme", "9.9.9", core::Sha256::hex("PK-fake-package-bytes")),
                              Fixture::review("com.example.tema", "theme", "1.0.0", std::string(64, 'b'))}) {
        Fixture f;
        f.reviewReply = reply;
        f.svc->refresh();
        auto out = f.svc->install("com.example.tema", {});
        EXPECT_FALSE(out.ok) << reply;
        EXPECT_TRUE(out.reviewJson.empty());
        EXPECT_EQ(f.cancelled, (std::vector<std::string>{"tok1"})) << reply;
    }
}

TEST(MarketplaceService, ReviewErrorFromThePluginSystemIsForwarded) {
    Fixture f;
    f.reviewReply = R"({"t":"plugin_review","ok":false,"error":"manifest inválido"})";
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", {});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.reviewJson.find("manifest inválido"), std::string::npos);
}

TEST(MarketplaceService, NativePluginIsFlaggedAndVerifiedComesFromTheCatalog) {
    Fixture f;
    const std::string e = replaceOnce(nativeEntry("com.example.fonte", f.pkgSha, f.pkg.size()), R"("verified")", R"("verified")");
    const std::string withVerified = replaceOnce(e, R"("license":"MIT")", R"("license":"MIT","verified":true,"homepage":"https://example.com")");
    f.http.catalogResponse = ok200(catalog(withVerified));
    f.http.files["https://example.com/f.hbplugin"] = f.pkg;
    f.reviewReply = Fixture::review("com.example.fonte", "source", "2.1.0", f.pkgSha);
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("list")->a[0].get("native")->b);
    EXPECT_TRUE(j.get("list")->a[0].get("verified")->b);
    auto out = f.svc->install("com.example.fonte", {});
    ASSERT_TRUE(out.ok) << out.message;
    auto ctx = f.json(out.contextJson);
    EXPECT_TRUE(ctx.get("native")->b);
    EXPECT_TRUE(ctx.get("verified")->b);
}

TEST(MarketplaceService, UnknownIdAndIncompatibleEntriesCannotBeInstalled) {
    Fixture f;
    f.svc->refresh();
    EXPECT_FALSE(f.svc->install("com.example.nope", {}).ok);
    EXPECT_FALSE(f.svc->install("../../etc/passwd", {}).ok);
    f.http.catalogResponse = ok200(catalog(replaceOnce(themeEntry("com.example.tema", f.pkgSha, f.pkg.size()), R"("minAppVersion":"0.1.0")", R"("minAppVersion":"9.0.0")")));
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", {});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.message.find("9.0.0"), std::string::npos);
    auto j = f.json(f.svc->catalogJson());
    EXPECT_FALSE(j.get("list")->a[0].get("compatible")->b);
}

TEST(MarketplaceService, DownloadFileNameNeverComesFromTheCatalog) {
    Fixture f;
    f.svc->refresh();
    auto out = f.svc->install("com.example.tema", {});
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(fs::path(f.reviewedPath).parent_path(), f.root / "dl");
    EXPECT_EQ(fs::path(f.reviewedPath).filename().string().rfind("hbm-", 0), 0u);
}

TEST(MarketplaceService, HostileCatalogJsonInTheCacheIsIgnored) {
    Fixture f;
    fs::create_directories(f.root / "cache");
    { std::ofstream(f.root / "cache" / "index.json") << "{\"schemaVersion\":1,\"plugins\":[{\"id\":\"../x\"}]}"; }
    { std::ofstream(f.root / "cache" / "index.meta") << "https://example.com/index.json\n\"v9\"\n"; }
    f.http.catalogResponse = FetchResponse{};
    f.http.catalogResponse.error = "offline";
    f.svc->refresh();
    auto j = f.json(f.svc->catalogJson());
    EXPECT_TRUE(j.get("list")->a.empty());          // the poisoned entry was dropped by the same validator
}
