#include <gtest/gtest.h>

#include <thread>

#include "core/json.hpp"
#include "plugins/service.hpp"
#include "plugins_test_util.hpp"

using namespace hb;
using namespace hb::plugins;
using namespace hb::testutil;
namespace fs = std::filesystem;

namespace {
struct Svc {
    fs::path root = makeTempDir("svc");
    PluginService s{pathToUtf8(root / "plugins"), platformKey()};
    Svc() { s.start(); }
    ~Svc() { std::error_code ec; fs::remove_all(root, ec); }
    std::string pkg(const std::string& id, const char* bg = "#0A1226") {
        const std::string m = R"({"schema":1,"id":")" + id + R"(","name":"Tema","version":"1.0.0","type":"theme","theme":{"colorScheme":"dark","vars":{"--bg":")" + std::string(bg) + R"("}}})";
        return writeFile(root / (id + ".hbplugin"), makeZip({{"manifest.json", m}}));
    }
};
core::json::Value parse(const std::string& s) {
    core::json::Value v;
    EXPECT_TRUE(core::json::parse(s, v)) << s;
    return v;
}
}  // namespace

TEST(PluginService, TwoStepInstallRequiresTheReviewToken) {
    Svc x;
    auto review = parse(x.s.beginInstall(x.pkg("com.test.t")));
    ASSERT_TRUE(review.get("ok")->b);
    EXPECT_FALSE(review.get("native")->b);
    EXPECT_EQ(review.get("type")->s, "theme");
    EXPECT_TRUE(x.s.manager().list().empty());                      // reviewing installs nothing
    const std::string token = review.get("token")->s;

    EXPECT_FALSE(parse(x.s.confirmInstall("deadbeef")).get("ok")->b);   // wrong token
    EXPECT_TRUE(x.s.manager().list().empty());

    auto done = parse(x.s.confirmInstall(token));
    EXPECT_TRUE(done.get("ok")->b) << done.get("message")->s;
    EXPECT_EQ(done.get("id")->s, "com.test.t");
    EXPECT_FALSE(parse(x.s.confirmInstall(token)).get("ok")->b);        // single use
    ASSERT_EQ(x.s.manager().list().size(), 1u);
    EXPECT_FALSE(x.s.manager().list()[0].enabled);
}

TEST(PluginService, OnlyTheNewestReviewStaysValid) {
    Svc x;
    auto a = parse(x.s.beginInstall(x.pkg("com.test.a")));
    auto b = parse(x.s.beginInstall(x.pkg("com.test.b")));
    EXPECT_FALSE(parse(x.s.confirmInstall(a.get("token")->s)).get("ok")->b);
    EXPECT_TRUE(parse(x.s.confirmInstall(b.get("token")->s)).get("ok")->b);
}

TEST(PluginService, CancelAndExpiry) {
    Svc x;
    auto a = parse(x.s.beginInstall(x.pkg("com.test.a")));
    x.s.cancelInstall(a.get("token")->s);
    EXPECT_FALSE(parse(x.s.confirmInstall(a.get("token")->s)).get("ok")->b);
    x.s.setReviewTtl(std::chrono::seconds(0));
    auto b = parse(x.s.beginInstall(x.pkg("com.test.b")));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto r = parse(x.s.confirmInstall(b.get("token")->s));
    EXPECT_FALSE(r.get("ok")->b);
    EXPECT_TRUE(x.s.manager().list().empty());
}

TEST(PluginService, FileSwappedBetweenReviewAndConfirmIsRefused) {
    Svc x;
    const std::string path = x.pkg("com.test.a", "#111111");
    auto review = parse(x.s.beginInstall(path));
    x.pkg("com.test.a", "#999999");                                   // same path, different bytes
    EXPECT_FALSE(parse(x.s.confirmInstall(review.get("token")->s)).get("ok")->b);
    EXPECT_TRUE(x.s.manager().list().empty());
}

TEST(PluginService, InvalidPackageGivesAnErrorNotAToken) {
    Svc x;
    const std::string junk = writeFile(x.root / "junk.hbplugin", std::vector<std::uint8_t>(500, 7));
    auto r = parse(x.s.beginInstall(junk));
    EXPECT_FALSE(r.get("ok")->b);
    EXPECT_EQ(r.get("token"), nullptr);
    EXPECT_FALSE(r.get("error")->s.empty());
    EXPECT_FALSE(parse(x.s.beginInstall("/no/such/file.hbplugin")).get("ok")->b);
}

TEST(PluginService, ThemeJsonOnlyCarriesValidatedVariables) {
    Svc x;
    parse(x.s.confirmInstall(parse(x.s.beginInstall(x.pkg("com.test.t"))).get("token")->s));
    EXPECT_TRUE(parse(x.s.themeJson()).get("vars")->o.empty());       // not enabled yet
    ASSERT_TRUE(x.s.setEnabled("com.test.t", true).ok);
    auto t = parse(x.s.themeJson());
    EXPECT_EQ(t.get("vars")->get("--bg")->s, "#0A1226");
    EXPECT_EQ(t.get("scheme")->s, "dark");
    auto list = parse(x.s.listJson());
    ASSERT_EQ(list.get("list")->a.size(), 1u);
    EXPECT_TRUE(list.get("list")->a[0].get("enabled")->b);
    ASSERT_TRUE(x.s.setEnabled("com.test.t", false).ok);
    EXPECT_TRUE(parse(x.s.themeJson()).get("vars")->o.empty());
}

TEST(PluginService, EnablingAnUnloadablePluginReportsAndSwitchesItOff) {
    Svc x;
    const std::string libData = "NOT A REAL LIBRARY";
    const std::string path = writeFile(x.root / "bad.hbplugin", makeZip({{"manifest.json", sourceManifest("com.test.bad", "libbad.so", libData)}, {"libbad.so", libData}}));
    ASSERT_TRUE(parse(x.s.confirmInstall(parse(x.s.beginInstall(path)).get("token")->s)).get("ok")->b);
    auto r = x.s.setEnabled("com.test.bad", true);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.message.empty());
    InstalledPlugin p;
    ASSERT_TRUE(x.s.manager().find("com.test.bad", p));
    EXPECT_FALSE(p.enabled);
    EXPECT_FALSE(p.loaded);
}
