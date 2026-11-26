#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

#include "core/sha256.hpp"
#include "updater/update_service.hpp"

using namespace hb;
using namespace hb::updater;
namespace fs = std::filesystem;

namespace {

const std::string kCommit = "0123456789abcdef0123456789abcdef01234567";
const std::string kInstallerUrl = "https://github.com/o/r/releases/download/latest/HBStudio-Setup.exe";
const std::string kSumUrl = kInstallerUrl + ".sha256";

struct FakeHttp : marketplace::IHttpDownloader {
    marketplace::FetchResponse release;
    std::map<std::string, std::string> files;   // url -> bytes
    std::vector<std::string> urls;
    int fetches = 0;
    std::uint64_t lastMax = 0;

    marketplace::FetchResponse fetch(const marketplace::FetchRequest&) override {
        ++fetches;
        return release;
    }
    marketplace::FileResult downloadToFile(const marketplace::FileRequest& r, const marketplace::ProgressFn& progress) override {
        urls.push_back(r.url);
        marketplace::FileResult out;
        auto it = files.find(r.url);
        if (it == files.end()) { out.error = "servidor não encontrado"; return out; }
        if (it->second.size() > r.maxBytes) { out.error = "o arquivo é maior que o tamanho declarado"; return out; }
        lastMax = r.maxBytes;
        if (progress && !progress(it->second.size() / 2, it->second.size())) { out.cancelled = true; out.error = "cancelado"; return out; }
        std::ofstream f(fs::path(std::u8string(r.destPathUtf8.begin(), r.destPathUtf8.end())), std::ios::binary);
        f << it->second;
        f.close();
        out.ok = true;
        out.status = 200;
        out.bytes = it->second.size();
        return out;
    }
};

std::string fakeExe(std::size_t n = 1000) { return std::string("MZ") + std::string(n - 2, 'x'); }

std::string releaseBody(const std::string& exe, bool digest, bool sumAsset, std::uint64_t sizeOverride = 0) {
    const std::uint64_t size = sizeOverride ? sizeOverride : exe.size();
    std::string j = R"({"tag_name":"latest","prerelease":true,"target_commitish":")" + kCommit + R"(","body":"notas","assets":[{"name":"HBStudio-Setup.exe","size":)" +
                    std::to_string(size) + R"(,"browser_download_url":")" + kInstallerUrl + "\"";
    if (digest) j += R"(,"digest":"sha256:)" + core::Sha256::hex(exe) + "\"";
    j += "}";
    if (sumAsset) j += R"(,{"name":"HBStudio-Setup.exe.sha256","size":90,"browser_download_url":")" + kSumUrl + "\"}";
    return j + "]}";
}

fs::path pathOf(const std::string& u8) { return fs::path(std::u8string(u8.begin(), u8.end())); }

struct UpdaterSvc : ::testing::Test {
    FakeHttp http;
    fs::path dir;
    std::int64_t t = 1000000;
    void SetUp() override {
        dir = fs::path(::testing::TempDir()) / "hb_updater_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        http.release.transportOk = true;
        http.release.status = 200;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::unique_ptr<UpdateService> make(const std::string& version = "0.3.0+fedcba9") {
        UpdaterConfig c;
        c.appVersion = version;
        const auto u = dir.u8string();
        c.dataDirUtf8 = std::string(u.begin(), u.end());
        return std::make_unique<UpdateService>(http, c, [this] { return t; });
    }
};

}  // namespace

TEST_F(UpdaterSvc, CheckFindsNewVersion) {
    http.release.body = releaseBody(fakeExe(), true, false);
    auto s = make();
    s->check(true);
    EXPECT_EQ(s->state(), UpdateState::Available);
    const std::string j = s->stateJson();
    EXPECT_NE(j.find("\"state\":\"available\""), std::string::npos);
    EXPECT_NE(j.find("\"notes\":\"notas\""), std::string::npos);
    EXPECT_NE(j.find("\"hashPublished\":true"), std::string::npos);
}

TEST_F(UpdaterSvc, UpToDateWhenCommitMatches) {
    http.release.body = releaseBody(fakeExe(), true, false);
    auto s = make("0.3.0+0123456");
    s->check(true);
    EXPECT_EQ(s->state(), UpdateState::UpToDate);
    EXPECT_FALSE(s->downloadInstaller(nullptr).ok);   // nothing to download
    EXPECT_TRUE(http.urls.empty());
}

TEST_F(UpdaterSvc, FailuresReportedAndNeverThrow) {
    auto s = make();
    http.release.transportOk = false;
    http.release.error = "sem internet";
    s->check(true);
    EXPECT_NE(s->stateJson().find("sem internet"), std::string::npos);
    http.release = {};
    http.release.transportOk = true;
    http.release.status = 403;
    t += 100;
    s->check(true);
    EXPECT_NE(s->stateJson().find("limitou"), std::string::npos);
    http.release.status = 404;
    t += 100;
    s->check(true);
    EXPECT_NE(s->stateJson().find("Nenhuma vers"), std::string::npos);
    http.release.status = 200;
    http.release.body = "<html>nope</html>";
    t += 100;
    s->check(true);
    EXPECT_EQ(s->state(), UpdateState::Unknown);
}

TEST_F(UpdaterSvc, ManualCheckIsThrottledAutoIntervalHonoured) {
    http.release.body = releaseBody(fakeExe(), true, false);
    auto s = make();
    EXPECT_TRUE(s->dueForAutoCheck());
    s->check(false);
    EXPECT_EQ(http.fetches, 1);
    EXPECT_FALSE(s->dueForAutoCheck());
    s->check(true);   // 0 s later: throttled, no second request
    EXPECT_EQ(http.fetches, 1);
    t += 6 * 3600 - 1;
    EXPECT_FALSE(s->dueForAutoCheck());
    t += 1;
    EXPECT_TRUE(s->dueForAutoCheck());
    s->setAutoCheck(false);
    EXPECT_FALSE(s->dueForAutoCheck());
}

TEST_F(UpdaterSvc, SettingsPersistAcrossInstances) {
    http.release.body = releaseBody(fakeExe(), true, false);
    {
        auto s = make();
        s->check(true);
        s->setAutoCheck(false);
        s->dismiss();
        EXPECT_FALSE(s->setReleaseUrl("http://api.github.com/x"));
        EXPECT_FALSE(s->setReleaseUrl("https://evil.example/x"));
        EXPECT_TRUE(s->setReleaseUrl("https://api.github.com/repos/fork/hb-studio/releases/tags/latest"));
    }
    auto s2 = make();
    s2->load();
    EXPECT_FALSE(s2->autoCheckEnabled());
    EXPECT_NE(s2->stateJson().find("repos/fork/hb-studio"), std::string::npos);
    EXPECT_TRUE(s2->setReleaseUrl(""));
    EXPECT_NE(s2->stateJson().find("jrscbusiness"), std::string::npos);
}

TEST_F(UpdaterSvc, DownloadVerifiesWithApiDigest) {
    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, true, false);
    http.files[kInstallerUrl] = exe;
    auto s = make();
    s->check(true);
    int last = -1;
    const auto d = s->downloadInstaller([&](int pct) { last = pct; });
    ASSERT_TRUE(d.ok) << d.error;
    EXPECT_GE(last, 0);
    EXPECT_EQ(http.lastMax, exe.size());   // the cap is the size the release declares
    EXPECT_EQ(d.staged.sha256, core::Sha256::hex(exe));
    EXPECT_TRUE(fs::exists(pathOf(d.staged.pathUtf8)));
    std::string err;
    EXPECT_TRUE(s->verifyStaged(err)) << err;
    // tampering after verification is caught right before the run
    {
        std::ofstream f(pathOf(d.staged.pathUtf8), std::ios::binary | std::ios::trunc);
        f << fakeExe() << "!";
    }
    EXPECT_FALSE(s->verifyStaged(err));
}

TEST_F(UpdaterSvc, DownloadVerifiesWithChecksumAssetOnly) {
    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, false, true);
    http.files[kInstallerUrl] = exe;
    http.files[kSumUrl] = core::Sha256::hex(exe) + "  HBStudio-Setup.exe\n";
    auto s = make();
    s->check(true);
    EXPECT_TRUE(s->downloadInstaller(nullptr).ok);
}

TEST_F(UpdaterSvc, RefusesWithoutAnyPublishedHash) {
    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, false, false);
    http.files[kInstallerUrl] = exe;
    auto s = make();
    s->check(true);
    const auto d = s->downloadInstaller(nullptr);
    EXPECT_FALSE(d.ok);
    EXPECT_NE(d.error.find("SHA-256"), std::string::npos);
    EXPECT_TRUE(http.urls.empty());   // not even downloaded
}

TEST_F(UpdaterSvc, RefusesHashMismatchAndDeletesFile) {
    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, true, false);
    http.files[kInstallerUrl] = fakeExe() + "x";   // bigger than declared: stopped by the size cap
    auto s = make();
    s->check(true);
    EXPECT_FALSE(s->downloadInstaller(nullptr).ok);

    std::string other = fakeExe();
    other[500] = 'y';                               // same size, different content
    http.files[kInstallerUrl] = other;
    const auto d = s->downloadInstaller(nullptr);
    EXPECT_FALSE(d.ok);
    EXPECT_NE(d.error.find("SHA-256"), std::string::npos);
    EXPECT_FALSE(fs::exists(dir / "downloads" / "HBStudio-Setup.exe"));
    EXPECT_EQ(s->staged().pathUtf8, "");
}

TEST_F(UpdaterSvc, RefusesDivergingPublishedHashes) {
    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, true, true);
    http.files[kInstallerUrl] = exe;
    http.files[kSumUrl] = std::string(64, 'c') + "  HBStudio-Setup.exe\n";
    auto s = make();
    s->check(true);
    const auto d = s->downloadInstaller(nullptr);
    EXPECT_FALSE(d.ok);
    EXPECT_NE(d.error.find("divergem"), std::string::npos);
}

TEST_F(UpdaterSvc, RefusesNonExecutableAndWrongSize) {
    const std::string notExe(1000, 'x');
    http.release.body = releaseBody(notExe, true, false);
    http.files[kInstallerUrl] = notExe;
    auto s = make();
    s->check(true);
    EXPECT_FALSE(s->downloadInstaller(nullptr).ok);

    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, true, false, exe.size() + 10);   // declares more than it serves
    http.files[kInstallerUrl] = exe;
    t += 100;
    s->check(true);
    EXPECT_FALSE(s->downloadInstaller(nullptr).ok);
}

TEST_F(UpdaterSvc, CancelLeavesNothingStaged) {
    const std::string exe = fakeExe();
    http.release.body = releaseBody(exe, true, false);
    http.files[kInstallerUrl] = exe;
    auto s = make();
    s->check(true);
    const auto d = s->downloadInstaller([&](int) { s->cancelDownload(); });
    EXPECT_FALSE(d.ok);
    EXPECT_TRUE(d.cancelled);
    EXPECT_EQ(s->staged().pathUtf8, "");
}
