#include <gtest/gtest.h>

#include <string>

#include "updater/update_check.hpp"

using namespace hb::updater;

namespace {

const std::string kSha(64, 'a');
const std::string kCommit = "0123456789abcdef0123456789abcdef01234567";
const std::string kDl = "https://github.com/o/r/releases/download/latest/";

std::string releaseJson(const std::string& extraAssets = "", const std::string& target = "\"" + kCommit + "\"",
                        const std::string& body = "\"Instalador automatico (commit " + kCommit + ").\"") {
    return R"({"tag_name":"latest","name":"HB Studio - ultima versao","prerelease":true,"published_at":"2026-11-01T10:00:00Z",)"
           R"("target_commitish":)" + target + R"(,"body":)" + body + R"(,"assets":[)"
           R"({"name":"HBStudio-Setup.exe","size":5700000,"browser_download_url":")" + kDl + R"(HBStudio-Setup.exe","digest":"sha256:)" + kSha + R"("})" +
           (extraAssets.empty() ? "" : "," + extraAssets) + "]}";
}

std::string sumAsset() {
    return R"({"name":"HBStudio-Setup.exe.sha256","size":90,"browser_download_url":")" + kDl + R"(HBStudio-Setup.exe.sha256"})";
}

}  // namespace

TEST(UpdaterParse, FullRelease) {
    const auto r = parseRelease(releaseJson(sumAsset()));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.release.tag, "latest");
    EXPECT_TRUE(r.release.prerelease);
    EXPECT_EQ(r.release.targetCommit, kCommit);
    ASSERT_TRUE(r.release.hasInstaller);
    EXPECT_EQ(r.release.installer.size, 5700000u);
    EXPECT_EQ(r.release.installer.sha256, kSha);
    EXPECT_NE(r.release.checksumUrl.find("HBStudio-Setup.exe.sha256"), std::string::npos);
    EXPECT_NE(r.release.notes.find("Instalador automatico"), std::string::npos);
}

TEST(UpdaterParse, CommitFromNotesWhenTargetIsABranch) {
    const auto r = parseRelease(releaseJson("", "\"main\""));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.release.targetCommit, kCommit);
}

TEST(UpdaterParse, NoCommitAnywhereIsUnknown) {
    const auto r = parseRelease(releaseJson("", "\"main\"", "\"sem hash\""));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.release.targetCommit.empty());
    EXPECT_EQ(compareWithRelease("0.3.0+abc1234", r.release), UpdateState::Unknown);
}

TEST(UpdaterParse, HostileInputsNeverCrash) {
    for (const char* j : {"", "null", "[]", "42", "{", "{\"tag_name\":5}", "{\"tag_name\":\"x\",\"assets\":7}", "\xff\xfe",
                          "{\"tag_name\":\"x\",\"assets\":[1,null,\"s\",{\"name\":5}]}"}) {
        const auto r = parseRelease(j);
        if (r.ok) { EXPECT_FALSE(r.release.hasInstaller); }
    }
    std::string deep(200, '[');
    EXPECT_FALSE(parseRelease(deep).ok);
    EXPECT_FALSE(parseRelease(std::string(kMaxReleaseJsonBytes + 1, ' ')).ok);
}

TEST(UpdaterParse, AssetRulesAreStrict) {
    auto withAsset = [](const std::string& asset) {
        return R"({"tag_name":"latest","target_commitish":")" + kCommit + R"(","assets":[)" + asset + "]}";
    };
    const std::string good = R"({"name":"HBStudio-Setup.exe","size":100,"browser_download_url":")" + kDl + R"(HBStudio-Setup.exe"})";
    EXPECT_TRUE(parseRelease(withAsset(good)).release.hasInstaller);
    // http, other host, userinfo, wrong name, zero/negative/huge/non-numeric size
    for (const char* bad : {
             R"({"name":"HBStudio-Setup.exe","size":100,"browser_download_url":"http://github.com/o/r/HBStudio-Setup.exe"})",
             R"({"name":"HBStudio-Setup.exe","size":100,"browser_download_url":"https://evil.example/HBStudio-Setup.exe"})",
             R"({"name":"HBStudio-Setup.exe","size":100,"browser_download_url":"https://github.com@evil.example/x"})",
             R"({"name":"Other.exe","size":100,"browser_download_url":"https://github.com/o/r/Other.exe"})",
             R"({"name":"HBStudio-Setup.exe","size":0,"browser_download_url":"https://github.com/o/r/x"})",
             R"({"name":"HBStudio-Setup.exe","size":-5,"browser_download_url":"https://github.com/o/r/x"})",
             R"({"name":"HBStudio-Setup.exe","size":99999999999,"browser_download_url":"https://github.com/o/r/x"})",
             R"({"name":"HBStudio-Setup.exe","size":"100","browser_download_url":"https://github.com/o/r/x"})"}) {
        EXPECT_FALSE(parseRelease(withAsset(bad)).release.hasInstaller) << bad;
    }
}

TEST(UpdaterParse, BadDigestIsIgnored) {
    std::string j = releaseJson();
    j.replace(j.find("sha256:" + kSha), 7 + 64, "sha256:zz");
    const auto r = parseRelease(j);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.release.installer.sha256.empty());
}

TEST(UpdaterCompare, VersionAndCommit) {
    EXPECT_EQ(commitOfVersion("0.3.0+abc1234"), "abc1234");
    EXPECT_EQ(commitOfVersion("0.3.0+ABC1234"), "abc1234");
    EXPECT_EQ(commitOfVersion("0.3.0"), "");
    EXPECT_EQ(commitOfVersion("0.3.0+xyz"), "");
    EXPECT_EQ(commitOfVersion("0.3.0+abc"), "");   // too short to be a commit
    EXPECT_EQ(commitOfVersion(""), "");

    ReleaseInfo rel;
    rel.hasInstaller = true;
    rel.targetCommit = kCommit;
    EXPECT_EQ(compareWithRelease("0.3.0+0123456", rel), UpdateState::UpToDate);
    EXPECT_EQ(compareWithRelease("0.3.0+" + kCommit, rel), UpdateState::UpToDate);
    EXPECT_EQ(compareWithRelease("0.3.0+fedcba9", rel), UpdateState::Available);
    EXPECT_EQ(compareWithRelease("0.3.0", rel), UpdateState::Unknown);
    rel.hasInstaller = false;
    EXPECT_EQ(compareWithRelease("0.3.0+fedcba9", rel), UpdateState::Unknown);
}

TEST(UpdaterPolicy, AutoCheckInterval) {
    const std::int64_t now = 1000000;
    EXPECT_FALSE(autoCheckDue(false, now, 0, 0));
    EXPECT_TRUE(autoCheckDue(true, now, 0, 0));
    EXPECT_FALSE(autoCheckDue(true, now, now - 5 * 3600, 0));        // 5 h ago: not yet
    EXPECT_TRUE(autoCheckDue(true, now, now - 6 * 3600, 0));         // exactly 6 h
    EXPECT_FALSE(autoCheckDue(true, now, 0, now - 600));             // failed 10 min ago: wait 1 h
    EXPECT_TRUE(autoCheckDue(true, now, 0, now - 3600));
    EXPECT_TRUE(autoCheckDue(true, now, now + 99999, now + 99999));  // clock went backwards: do not freeze
    EXPECT_FALSE(manualCheckAllowed(now, now - 5));
    EXPECT_TRUE(manualCheckAllowed(now, now - 20));
}

TEST(UpdaterChecksum, ParsesCommonFormats) {
    const std::string h(64, 'b');
    EXPECT_EQ(parseChecksumFile(h + "  HBStudio-Setup.exe\n", kInstallerAssetName), h);
    EXPECT_EQ(parseChecksumFile(h + " *HBStudio-Setup.exe\r\n", kInstallerAssetName), h);
    EXPECT_EQ(parseChecksumFile(h + "\n", kInstallerAssetName), h);
    EXPECT_EQ(parseChecksumFile(std::string(64, 'B') + "  HBStudio-Setup.exe", kInstallerAssetName), h);   // lowercased
    EXPECT_EQ(parseChecksumFile(h + "  Other.exe\n", kInstallerAssetName), "");
    EXPECT_EQ(parseChecksumFile(h.substr(1) + "\n", kInstallerAssetName), "");
    EXPECT_EQ(parseChecksumFile(h + "ab\n", kInstallerAssetName), "");
    EXPECT_EQ(parseChecksumFile("not a hash\n", kInstallerAssetName), "");
    EXPECT_EQ(parseChecksumFile("", kInstallerAssetName), "");
    EXPECT_EQ(parseChecksumFile(std::string(kMaxChecksumBytes + 1, 'a'), kInstallerAssetName), "");
}

TEST(UpdaterNotes, SanitizedAndTruncated) {
    EXPECT_EQ(sanitizeNotes("a\r\n\r\n\r\n\r\nb\x01\x07 c\t\n", 100), "a\n\nb c");
    const std::string longText(2000, 'x');
    EXPECT_LE(sanitizeNotes(longText, 100).size(), 100u + 3u);
    // never splits a UTF-8 sequence: 'e acute' is 2 bytes, an odd cut would land inside one
    std::string accents;
    for (int i = 0; i < 100; ++i) accents += "\xC3\xA9";
    const std::string c2 = sanitizeNotes(accents, 101);
    ASSERT_GE(c2.size(), 3u);
    EXPECT_EQ(c2.substr(c2.size() - 3), "\xE2\x80\xA6");
    EXPECT_EQ((c2.size() - 3) % 2, 0u);
    EXPECT_EQ(sanitizeNotes("\xff\xfe broken", 100), "");
}

TEST(UpdaterUrl, Host) {
    EXPECT_EQ(urlHost("https://GitHub.com/a/b"), "github.com");
    EXPECT_EQ(urlHost("https://api.github.com:443/x?y"), "api.github.com");
    EXPECT_EQ(urlHost("http://github.com/"), "");
    EXPECT_EQ(urlHost("ftp://github.com/"), "");
    EXPECT_EQ(urlHost(""), "");
}
