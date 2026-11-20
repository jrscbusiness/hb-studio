#include <gtest/gtest.h>

#include "plugins/manifest.hpp"

using namespace hb::plugins;

namespace {
const char* kSource = R"({"schema":1,"id":"com.acme.cam","name":"Acme Cam","version":"2.0.1-beta.1","author":"Acme","type":"source","apiVersion":1,
 "library":{"windows-x64":"acme.dll","linux-x64":"libacme.so"},
 "sha256":{"acme.dll":"0000000000000000000000000000000000000000000000000000000000000000","libacme.so":"1111111111111111111111111111111111111111111111111111111111111111"}})";
}

TEST(Manifest, ParsesSourceEffectAndTheme) {
    auto s = parseManifest(kSource);
    ASSERT_TRUE(s.ok) << s.error;
    EXPECT_EQ(s.manifest.type, PluginType::Source);
    EXPECT_EQ(s.manifest.libraryFor("windows-x64"), "acme.dll");
    EXPECT_EQ(s.manifest.libraryFor("macos-arm64"), "");

    std::string eff = kSource;
    eff.replace(eff.find("\"source\""), 8, "\"effect\"");
    auto e = parseManifest(eff);
    ASSERT_TRUE(e.ok) << e.error;
    EXPECT_EQ(e.manifest.type, PluginType::Effect);

    auto t = parseManifest(R"J({"schema":1,"id":"com.acme.theme","name":"T","version":"1.0.0","type":"theme",
        "theme":{"colorScheme":"dark","vars":{"--bg":"#112233","--ink":"rgba(1,2,3,.5)"}}})J");
    ASSERT_TRUE(t.ok) << t.error;
    EXPECT_EQ(t.manifest.themeVars.size(), 2u);
    EXPECT_EQ(t.manifest.colorScheme, "dark");
}

TEST(Manifest, RejectsStructuralProblems) {
    for (const char* bad : {"", "[]", "{", R"({"schema":2})", R"({"schema":1})",
                            R"({"schema":1,"id":"ab","name":"x","version":"1.0.0","type":"theme","theme":{"vars":{"--bg":"#fff"}}})",
                            R"({"schema":1,"id":"com.x.y","name":"","version":"1.0.0","type":"theme"})",
                            R"({"schema":1,"id":"com.x.y","name":"x","version":"1.0","type":"theme"})",
                            R"({"schema":1,"id":"com.x.y","name":"x","version":"1.0.0","type":"virus"})",
                            R"({"schema":1,"id":"com.x.y","name":"x","version":"1.0.0","type":"theme"})"}) {
        EXPECT_FALSE(parseManifest(bad).ok) << bad;
    }
}

TEST(Manifest, SourceNeedsIntegrityHashForEveryLibrary) {
    std::string m = kSource;
    m.replace(m.find("\"libacme.so\":\"1111"), 20, "\"other.so\":\"11111");     // library without a hash entry
    EXPECT_FALSE(parseManifest(m).ok);
    std::string noApi = kSource;
    noApi.replace(noApi.find("\"apiVersion\":1"), 14, "\"apiVersion\":2");
    EXPECT_FALSE(parseManifest(noApi).ok);
    std::string badHash = kSource;
    badHash.replace(badHash.find("0000000000"), 10, "ZZZZZZZZZZ");
    EXPECT_FALSE(parseManifest(badHash).ok);
}

TEST(Manifest, ThemeOnlyAcceptsWhitelistedVariablesAndPlainColours) {
    auto mk = [](const std::string& var, const std::string& val) {
        return R"({"schema":1,"id":"com.x.th","name":"T","version":"1.0.0","type":"theme","theme":{"vars":{")" + var + R"(":")" + val + R"("}}})";
    };
    EXPECT_TRUE(parseManifest(mk("--bg", "#abc")).ok);
    EXPECT_FALSE(parseManifest(mk("--evil", "#abc")).ok);
    EXPECT_FALSE(parseManifest(mk("background", "#abc")).ok);
    for (const char* v : {"red", "url(http://x/y.png)", "#12", "#GGGGGG", "expression(alert(1))", "rgb(1,2,3);color:red", "#fff;}body{display:none",
                          "rgba(0,0,0,0.5)\"", "var(--x)", ""})
        EXPECT_FALSE(parseManifest(mk("--bg", v)).ok) << v;
    EXPECT_FALSE(validThemeColor(std::string(100, '#')));
    EXPECT_TRUE(validThemeColor("rgb(10 20 30 / 50%)"));
}

TEST(Manifest, IdVersionAndNameValidators) {
    for (const char* ok : {"com.acme.cam", "abc", "a-b_c.d", "x1y"}) EXPECT_TRUE(validPluginId(ok)) << ok;
    for (const char* bad : {"", "ab", "Com.Acme", "com..acme", ".abc", "abc.", "a b c", "con", "nul.x", "com1", "lpt9.test", "../x", "a/b", "a\\b"})
        EXPECT_FALSE(validPluginId(bad)) << bad;
    EXPECT_FALSE(validPluginId(std::string(65, 'a')));
    for (const char* ok : {"0.0.1", "10.20.30", "1.0.0-rc.1", "1.0.0+build5"}) EXPECT_TRUE(validVersion(ok)) << ok;
    for (const char* bad : {"1", "1.0", "a.b.c", "1.0.0.0", "1.0.0-", "1..0", "-1.0.0", "1.0.0-a b"}) EXPECT_FALSE(validVersion(bad)) << bad;
}

TEST(Manifest, LibraryFileNameAndEntryNameSafety) {
    for (const char* ok : {"a.dll", "libx-1.2.so", "Plugin_x64.dll"}) EXPECT_TRUE(validLibraryFileName(ok)) << ok;
    for (const char* bad : {"", "a", "a.txt", "../a.dll", "dir/a.dll", "a\\b.dll", "con.dll", "a..dll", ".dll", "a.dll.", "a b.dll", "a:b.dll"})
        EXPECT_FALSE(validLibraryFileName(bad)) << bad;

    for (const char* ok : {"manifest.json", "lib/a.dll", "a/b/c/d.txt", "dir/"}) EXPECT_TRUE(safeEntryName(ok)) << ok;
    for (const char* bad : {"", "/etc/passwd", "\\win\\x", "../x", "a/../b", "a//b", "./a", "C:\\x", "C:x", "a:b", "x/y/z/w/v.txt", "CON", "nul.txt",
                            "aux/x", "a.", "a /b", "a\\b", "a*b", "a?b", "a|b", "a<b", "a>b", "a\"b"})
        EXPECT_FALSE(safeEntryName(bad)) << bad;
    EXPECT_FALSE(safeEntryName(std::string("a\0b", 3)));
    EXPECT_FALSE(safeEntryName("a\x01" "b"));
    EXPECT_FALSE(safeEntryName(std::string(201, 'a')));
}

TEST(Manifest, ControlCharactersAndHugeFieldsAreRejected) {
    EXPECT_FALSE(parseManifest("{\"schema\":1,\"id\":\"com.x.y\",\"name\":\"a\\u0001b\",\"version\":\"1.0.0\",\"type\":\"theme\"}").ok);
    std::string big = R"({"schema":1,"id":"com.x.y","name":")" + std::string(200, 'x') + R"(","version":"1.0.0","type":"theme"})";
    EXPECT_FALSE(parseManifest(big).ok);
    EXPECT_FALSE(parseManifest(std::string(70 * 1024, ' ') + "{}").ok);
}
