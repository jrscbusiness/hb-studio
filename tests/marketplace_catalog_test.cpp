#include <gtest/gtest.h>

#include "marketplace/catalog.hpp"
#include "marketplace_test_util.hpp"

using namespace hb::marketplace;
using namespace hb::mkttest;

TEST(MarketplaceCatalog, ParsesAValidCatalog) {
    auto r = parseCatalog(catalog(themeEntry() + "," + nativeEntry()));
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.catalog.entries.size(), 2u);
    const auto& t = r.catalog.entries[0];
    EXPECT_EQ(t.id, "com.example.tema");
    EXPECT_FALSE(t.native());
    EXPECT_FALSE(t.verified);
    EXPECT_EQ(t.descriptionPt, "Um tema");
    EXPECT_EQ(t.versions[0].size, 800u);
    const auto& n = r.catalog.entries[1];
    EXPECT_TRUE(n.native());
    EXPECT_EQ(n.category, Category::VideoSource);
    EXPECT_EQ(n.versions[0].minAppVersion, "0.0.0");   // default
    EXPECT_EQ(r.catalog.dropped, 0u);
}

TEST(MarketplaceCatalog, RejectsWholeDocumentProblems) {
    EXPECT_FALSE(parseCatalog("").ok);
    EXPECT_FALSE(parseCatalog("not json").ok);
    EXPECT_FALSE(parseCatalog("[]").ok);
    EXPECT_FALSE(parseCatalog("{}").ok);
    EXPECT_FALSE(parseCatalog(R"({"schemaVersion":1})").ok);                       // no plugins
    EXPECT_FALSE(parseCatalog(R"({"schemaVersion":1,"plugins":{}})").ok);
    EXPECT_FALSE(parseCatalog(R"({"schemaVersion":"1","plugins":[]})").ok);
    EXPECT_FALSE(parseCatalog(R"({"schemaVersion":1.5,"plugins":[]})").ok);
    EXPECT_FALSE(parseCatalog(R"({"schemaVersion":0,"plugins":[]})").ok);
    EXPECT_FALSE(parseCatalog(catalog("", 1) + "garbage").ok);                      // trailing garbage
    auto newer = parseCatalog(catalog("", 2));
    EXPECT_FALSE(newer.ok);
    EXPECT_NE(newer.error.find("atualize"), std::string::npos);                     // tells the user to update the app
    EXPECT_TRUE(parseCatalog(catalog("")).ok);                                     // empty catalog is fine
}

TEST(MarketplaceCatalog, HostileDocumentsNeverCrash) {
    // deep nesting, huge size, huge arrays
    EXPECT_FALSE(parseCatalog(std::string(100000, '[')).ok);
    std::string deep = R"({"schemaVersion":1,"plugins":[)" + std::string(500, '[') + std::string(500, ']') + "]}";
    EXPECT_TRUE(parseCatalog(deep).ok == false || parseCatalog(deep).catalog.entries.empty());
    EXPECT_FALSE(parseCatalog(std::string(kMaxCatalogBytes + 1, ' ')).ok);
    std::string many = "{\"schemaVersion\":1,\"plugins\":[";
    for (int i = 0; i < 600; ++i) many += (i ? "," : "") + std::string("1");
    many += "]}";
    EXPECT_FALSE(parseCatalog(many).ok);   // more than kMaxCatalogEntries
    // NUL bytes / control characters / invalid UTF-8 in strings
    EXPECT_EQ(parseCatalog(catalog(replaceOnce(themeEntry(), "Tema", std::string("Te\0ma", 5)))).catalog.entries.size(), 0u);
    EXPECT_EQ(parseCatalog(catalog(replaceOnce(themeEntry(), "Tema", "Te\\u0007ma"))).catalog.entries.size(), 0u);
    EXPECT_EQ(parseCatalog(catalog(replaceOnce(themeEntry(), "Tema", "Te\xC3ma"))).catalog.entries.size(), 0u);
    EXPECT_EQ(parseCatalog(catalog(replaceOnce(themeEntry(), "Fulano", std::string(500, 'x')))).catalog.entries.size(), 0u);
    // entries of the wrong JSON types
    for (const char* junk : {"null", "1", "\"x\"", "[]", "{}", "true"}) {
        auto r = parseCatalog(catalog(junk));
        EXPECT_TRUE(r.ok) << junk;
        EXPECT_TRUE(r.catalog.entries.empty());
        EXPECT_EQ(r.catalog.dropped, 1u);
    }
}

TEST(MarketplaceCatalog, DropsInvalidEntriesButKeepsGoodOnes) {
    const std::string bad1 = replaceOnce(themeEntry("com.example.b1"), kHash, std::string(64, 'A'));          // uppercase hex
    const std::string bad2 = replaceOnce(themeEntry("com.example.b2"), kHash, std::string(63, 'a'));          // short hash
    const std::string bad3 = themeEntry("com.example.b3", kHash, 800, "1.0.0", "http://example.com/x.hbplugin");   // http
    const std::string bad4 = themeEntry("../evil");                                                           // path traversal id
    const std::string bad5 = themeEntry("com.example.b5", kHash, 0);                                          // size 0
    const std::string bad6 = themeEntry("com.example.b6", kHash, 64ull * 1024 * 1024 + 1);                    // above cap
    const std::string bad7 = themeEntry("com.example.b7", kHash, 800, "v1");                                  // bad semver
    const std::string bad8 = themeEntry("COM.EXAMPLE.UPPER");                                                  // uppercase id
    const std::string bad9 = themeEntry("con.example");                                                        // Windows device name
    auto r = parseCatalog(catalog(bad1 + "," + bad2 + "," + bad3 + "," + bad4 + "," + bad5 + "," + bad6 + "," + bad7 + "," + bad8 + "," + bad9 + "," +
                                  themeEntry("com.example.good")));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.catalog.entries.size(), 1u);
    EXPECT_EQ(r.catalog.entries[0].id, "com.example.good");
    EXPECT_EQ(r.catalog.dropped, 9u);
    EXPECT_FALSE(r.catalog.warnings.empty());
}

TEST(MarketplaceCatalog, DuplicateIdsAndVersionsAreDropped) {
    auto r = parseCatalog(catalog(themeEntry() + "," + themeEntry()));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.catalog.entries.size(), 1u);
    EXPECT_EQ(r.catalog.dropped, 1u);
    std::string dupVer = replaceOnce(themeEntry(), R"("versions":[)", R"("versions":[{"version":"1.0.0","platforms":["windows-x64"],"downloadUrls":["https://example.com/a"],"sha256":")" + kHash + R"(","size":1},)");
    EXPECT_TRUE(parseCatalog(catalog(dupVer)).catalog.entries.empty());
}

TEST(MarketplaceCatalog, RiskModelIsEnforced) {
    // native code must be declared; a theme must not claim it
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(nativeEntry(), R"("permissions":["native-code"])", R"("permissions":[])"))).catalog.entries.empty());
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("permissions":[])", R"("permissions":["native-code"])"))).catalog.entries.empty());
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"(,"permissions":[])", ""))).catalog.entries.empty());   // must be explicit
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(nativeEntry(), "native-code", "teleport"))).catalog.entries.empty());   // unknown permission
    // category / type consistency
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("category":"theme")", R"("category":"video-source")"))).catalog.entries.empty());
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(nativeEntry(), R"("type":"source")", R"("type":"effect")"))).catalog.entries.empty());
    EXPECT_FALSE(parseCatalog(catalog(replaceOnce(nativeEntry(), R"("category":"video-source")", R"("category":"graphic")"))).catalog.entries.empty());
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("category":"theme")", R"("category":"template")"))).catalog.entries.empty());   // template needs type template
    EXPECT_FALSE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("category":"theme","type":"theme")", R"("category":"graphic","type":"theme")"))).catalog.entries.empty());
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("category":"theme")", R"("category":"jogo")"))).catalog.entries.empty());
}

TEST(MarketplaceCatalog, VerifiedMustBeABoolean) {
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("verified":false)", R"("verified":"yes")"))).catalog.entries.empty());
    auto r = parseCatalog(catalog(replaceOnce(themeEntry(), R"("verified":false)", R"("verified":true)")));
    ASSERT_EQ(r.catalog.entries.size(), 1u);
    EXPECT_TRUE(r.catalog.entries[0].verified);
    EXPECT_FALSE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("verified":false,)", ""))).catalog.entries[0].verified);   // default: not verified
}

TEST(MarketplaceCatalog, UnknownFieldsAreIgnoredForForwardCompatibility) {
    auto r = parseCatalog(catalog(replaceOnce(themeEntry(), R"("tags")", R"("futureField":{"a":1},"tags")")));
    EXPECT_EQ(r.catalog.entries.size(), 1u);
}

TEST(MarketplaceCatalog, UrlPolicy) {
    EXPECT_TRUE(isSafeHttpsUrl("https://example.com/a.hbplugin"));
    EXPECT_TRUE(isSafeHttpsUrl("https://objects.githubusercontent.com:443/x?y=1#z"));
    for (const char* bad : {"", "http://example.com/a", "ftp://example.com/a", "file:///c:/x", "https://", "https:///a", "https://user:pw@example.com/a",
                            "https://example.com@evil.com/a", "https://localhost/a", "https://127.0.0.1/a", "https://10.0.0.5/a", "https://[::1]/a",
                            "https://example.local/a", "https://exa mple.com/a", "https://example.com/a b", "https://example.com/\n", "https://example.com:0/a",
                            "https://example.com:99999/a", "https://example..com/a", "https://example.com./a", "https://-/a", "javascript:alert(1)",
                            "https://example.com/a\\b", "https://exämple.com/a", "//example.com/a", "https://exa_mple.com/a"})
        EXPECT_FALSE(isSafeHttpsUrl(bad)) << bad;
    EXPECT_FALSE(isSafeHttpsUrl(std::string("https://example.com/") + std::string(3000, 'a')));
}

TEST(MarketplaceCatalog, ScreenshotsMustBeHttps) {
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(themeEntry(), R"("tags")", R"x("screenshots":["javascript:alert(1)"],"tags")x"))).catalog.entries.empty());
    auto r = parseCatalog(catalog(replaceOnce(themeEntry(), R"("tags")", R"("screenshots":["https://example.com/1.png"],"tags")")));
    ASSERT_EQ(r.catalog.entries.size(), 1u);
    EXPECT_EQ(r.catalog.entries[0].screenshots.size(), 1u);
}

TEST(MarketplaceCatalog, SelectsHighestCompatibleStableVersion) {
    std::string e = themeEntry();
    auto add = [&](const std::string& ver, const std::string& min, const std::string& plat = "windows-x64") {
        return std::string(R"({"version":")") + ver + R"(","minAppVersion":")" + min + R"(","platforms":[")" + plat +
               R"("],"downloadUrls":["https://example.com/p"],"sha256":")" + kHash + R"(","size":10})";
    };
    e = replaceOnce(e, R"("versions":[)", R"("versions":[)" + add("2.0.0", "9.0.0") + "," + add("1.5.0", "0.2.0") + "," + add("1.9.0-beta", "0.1.0") + "," +
                                          add("3.0.0", "0.1.0", "linux-x64") + ",");
    auto r = parseCatalog(catalog(e));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.catalog.entries.size(), 1u);
    const auto& ent = r.catalog.entries[0];
    auto sel = selectVersion(ent, "0.3.0+abc1234", "windows-x64");
    ASSERT_NE(sel.version, nullptr);
    EXPECT_EQ(sel.version->version, "1.5.0");   // not 2.0.0 (needs app 9), not the beta, not the linux build
    sel = selectVersion(ent, "0.1.5", "windows-x64");
    ASSERT_NE(sel.version, nullptr);
    EXPECT_EQ(sel.version->version, "1.0.0");
    sel = selectVersion(ent, "0.0.1", "windows-x64");
    EXPECT_EQ(sel.version, nullptr);
    EXPECT_NE(sel.reason.find("0.1.0"), std::string::npos);   // says which app version is needed
    sel = selectVersion(ent, "0.3.0", "macos-arm64");
    EXPECT_EQ(sel.version, nullptr);
    EXPECT_NE(sel.reason.find("plataforma"), std::string::npos);
    EXPECT_EQ(selectVersion(ent, "unknown", "windows-x64").version, nullptr);   // fail closed on an unparsable app version
}

TEST(MarketplaceCatalog, InstallStatus) {
    auto r = parseCatalog(catalog(themeEntry()));
    const auto sel = selectVersion(r.catalog.entries[0], "0.3.0", "windows-x64");
    EXPECT_EQ(installStatus(sel, ""), InstallStatus::Available);
    EXPECT_EQ(installStatus(sel, "0.9.0"), InstallStatus::UpdateAvailable);
    EXPECT_EQ(installStatus(sel, "1.0.0"), InstallStatus::Installed);
    EXPECT_EQ(installStatus(sel, "1.2.0"), InstallStatus::Installed);       // locally newer: no downgrade offer
    EXPECT_EQ(installStatus(sel, "weird"), InstallStatus::Installed);
    EXPECT_STREQ(installStatusName(InstallStatus::UpdateAvailable), "update");
}

TEST(MarketplaceCatalog, Utf8Validation) {
    EXPECT_TRUE(isValidUtf8("ação — ✓"));
    EXPECT_FALSE(isValidUtf8("\xC3"));
    EXPECT_FALSE(isValidUtf8("\xC0\x80"));          // overlong
    EXPECT_FALSE(isValidUtf8("\xED\xA0\x80"));      // surrogate
    EXPECT_FALSE(isValidUtf8("\xF5\x80\x80\x80"));  // > U+10FFFF
}

TEST(MarketplaceCatalog, TemplateEntriesArePureDataAndSmall) {
    const std::string t = replaceOnce(themeEntry("com.example.culto", kHash, 4000), R"("category":"theme","type":"theme")", R"("category":"template","type":"template")");
    auto r = parseCatalog(catalog(t));
    ASSERT_EQ(r.catalog.entries.size(), 1u);
    EXPECT_EQ(r.catalog.entries[0].type, PackageType::Template);
    EXPECT_FALSE(r.catalog.entries[0].native());
    // a template may not claim native code, and may not exceed the importer's 1 MB cap
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(t, R"("permissions":[])", R"("permissions":["native-code"])"))).catalog.entries.empty());
    const std::string big = replaceOnce(themeEntry("com.example.culto", kHash, 2 * 1024 * 1024), R"("category":"theme","type":"theme")", R"("category":"template","type":"template")");
    EXPECT_TRUE(parseCatalog(catalog(big)).catalog.entries.empty());
    // category template must use type template, and type template only category template
    EXPECT_TRUE(parseCatalog(catalog(replaceOnce(t, R"("category":"template")", R"("category":"graphic")"))).catalog.entries.empty());
}
