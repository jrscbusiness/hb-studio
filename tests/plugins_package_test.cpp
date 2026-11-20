#include <gtest/gtest.h>

#include "plugins/package.hpp"
#include "plugins_test_util.hpp"

using namespace hb;
using namespace hb::plugins;
using namespace hb::testutil;

namespace {
PackageResult inspect(const Entries& e, bool compress = true) { return inspectPackageBytes(makeZip(e, compress), platformKey()); }
}

TEST(Package, ValidThemeIsAccepted) {
    auto r = inspect({{"manifest.json", themeManifest()}});
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.manifest.id, "com.test.theme");
    EXPECT_EQ(r.info.packageSha256.size(), 64u);
    EXPECT_EQ(r.info.fileCount, 1);
}

TEST(Package, ValidNativeSourceWithMatchingHashIsAccepted) {
    const std::string lib = "FAKE-LIBRARY-BYTES";
    auto r = inspect({{"manifest.json", sourceManifest("com.test.src", "libfake.so", lib)}, {"libfake.so", lib}});
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.info.supportedHere);
}

TEST(Package, TamperedLibraryIsRejected) {
    auto r = inspect({{"manifest.json", sourceManifest("com.test.src", "libfake.so", "original")}, {"libfake.so", "tampered"}});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("sha256"), std::string::npos);
}

TEST(Package, MissingLibraryDeclaredForThisPlatformIsRejected) {
    auto r = inspect({{"manifest.json", sourceManifest("com.test.src", "libfake.so", "x")}});
    EXPECT_FALSE(r.ok);
}

TEST(Package, UnhashedExecutableIsRejected) {
    const std::string lib = "x";
    auto r = inspect({{"manifest.json", sourceManifest("com.test.src", "libfake.so", lib)}, {"libfake.so", lib}, {"helper.exe", "MZ"}});
    EXPECT_FALSE(r.ok);
    r = inspect({{"manifest.json", sourceManifest("com.test.src", "libfake.so", lib)}, {"libfake.so", lib}, {"other.dll", "MZ"}});
    EXPECT_FALSE(r.ok);
}

TEST(Package, ThemesCannotCarryExecutables) {
    for (const char* name : {"evil.exe", "x.dll", "a.bat", "a.ps1", "a.js", "a.cmd", "a.msi", "A.EXE"})
        EXPECT_FALSE(inspect({{"manifest.json", themeManifest()}, {name, "x"}}).ok) << name;
    EXPECT_TRUE(inspect({{"manifest.json", themeManifest()}, {"readme.txt", "ok"}, {"img/logo.png", "png"}}).ok);
}

TEST(Package, ZipSlipAndHostileNamesAreRejected) {
    for (const char* name : {"../evil.txt", "a/../../evil.txt", "C:/win.txt", "C:\\win.txt", "..\\evil.txt", "dir\\file.txt", "con.txt",
                             "a/aux", "file.", "x/y/z/w/v.txt", "a:stream", ".pending-delete", "state.json"}) {
        auto r = inspect({{"manifest.json", themeManifest()}, {name, "x"}});
        EXPECT_FALSE(r.ok) << name;
    }
    // absolute names, crafted byte for byte (the zip writer would silently drop the leading '/')
    for (const char* hostile : {"/abs.txt", "\\abs.txt"}) {
        auto zip = makeZipRawName({{"manifest.json", themeManifest()}, {"Xabs.txt", "x"}}, "Xabs.txt", hostile);
        EXPECT_FALSE(inspectPackageBytes(zip, platformKey()).ok) << hostile;
    }
}

TEST(Package, CaseInsensitiveDuplicatesAreRejected) {
    EXPECT_FALSE(inspect({{"manifest.json", themeManifest()}, {"a.txt", "1"}, {"A.TXT", "2"}}).ok);
}

TEST(Package, ManifestProblems) {
    EXPECT_FALSE(inspect({{"readme.txt", "x"}}).ok);                                        // no manifest
    EXPECT_FALSE(inspect({{"sub/manifest.json", themeManifest()}}).ok);                     // not at the root
    EXPECT_FALSE(inspect({{"manifest.json", "{not json"}}).ok);
    EXPECT_FALSE(inspect({{"manifest.json", std::string(100 * 1024, ' ') + themeManifest()}}).ok);   // oversize manifest
}

TEST(Package, NotAZipOrEmpty) {
    EXPECT_FALSE(inspectPackageBytes({}, platformKey()).ok);
    EXPECT_FALSE(inspectPackageBytes(std::vector<std::uint8_t>(1000, 0x41), platformKey()).ok);
    auto good = makeZip({{"manifest.json", themeManifest()}});
    good.resize(good.size() / 2);                                                            // truncated archive
    EXPECT_FALSE(inspectPackageBytes(good, platformKey()).ok);
    auto corrupt = makeZip({{"manifest.json", themeManifest()}}, false);
    corrupt[corrupt.size() / 3] ^= 0xFF;
    // either the structure or the contents break; it must never be accepted as a valid, different plugin silently
    auto r = inspectPackageBytes(corrupt, platformKey());
    if (r.ok) EXPECT_EQ(r.info.manifest.id, "com.test.theme");
}

TEST(Package, DecompressionBombIsRejected) {
    const std::string zeros(65 * 1024 * 1024, '\0');          // 65 MB of zeros compresses to ~65 KB
    auto zip = makeZip({{"manifest.json", themeManifest()}, {"big.bin", zeros}});
    EXPECT_LT(zip.size(), 1024u * 1024u);
    EXPECT_FALSE(inspectPackageBytes(zip, platformKey()).ok);
}

TEST(Package, TooManyEntriesIsRejected) {
    Entries e{{"manifest.json", themeManifest()}};
    for (int i = 0; i < 250; ++i) e.emplace_back("f" + std::to_string(i) + ".txt", "x");
    EXPECT_FALSE(inspect(e).ok);
}

TEST(Package, PackageHashBindsReviewToInstall) {
    auto a = makeZip({{"manifest.json", themeManifest()}});
    auto b = makeZip({{"manifest.json", themeManifest()}, {"extra.txt", "x"}});
    auto ra = inspectPackageBytes(a, platformKey()), rb = inspectPackageBytes(b, platformKey());
    ASSERT_TRUE(ra.ok && rb.ok);
    EXPECT_NE(ra.info.packageSha256, rb.info.packageSha256);
    EXPECT_EQ(ra.info.packageSha256, inspectPackageBytes(a, platformKey()).info.packageSha256);
}

TEST(Package, ExtractWritesVerifiedFilesAndRefusesChangedPackages) {
    const std::string lib = "LIBRARY-DATA";
    auto zip = makeZip({{"manifest.json", sourceManifest("com.test.src", "libfake.so", lib)}, {"libfake.so", lib}, {"docs/readme.txt", "hello"}});
    auto r = inspectPackageBytes(zip, platformKey());
    ASSERT_TRUE(r.ok) << r.error;

    auto dir = makeTempDir("extract");
    PackageInfo info;
    std::string err;
    ASSERT_TRUE(extractPackageBytes(zip, platformKey(), r.info.packageSha256, pluginsPath(dir / "out"), info, err)) << err;
    EXPECT_EQ(readFile(pathToUtf8Join(dir / "out", "docs/readme.txt")).size(), 5u);
    const auto written = readFile(pathToUtf8Join(dir / "out", "libfake.so"));
    EXPECT_EQ(core::Sha256::hex(std::string(written.begin(), written.end())), core::Sha256::hex(lib));

    // package swapped after review -> refused, nothing written
    auto other = makeZip({{"manifest.json", themeManifest()}});
    EXPECT_FALSE(extractPackageBytes(other, platformKey(), r.info.packageSha256, pluginsPath(dir / "out2"), info, err));
    EXPECT_NE(err.find("mudou"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir / "out2" / "manifest.json"));

    // destination must be empty
    EXPECT_FALSE(extractPackageBytes(zip, platformKey(), "", pluginsPath(dir / "out"), info, err));
    std::filesystem::remove_all(dir);
}
