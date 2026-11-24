#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "teleprompter/presenter_page.hpp"

using namespace hb::teleprompter;

TEST(TpPage, ExtractsTemplateContent) {
    const std::string html = "<body><template id=\"tpPresenterPage\"><!doctype html><p>x</p></template>after";
    EXPECT_EQ(extractPresenterPage(html), "<!doctype html><p>x</p>");
}

TEST(TpPage, MissingOrUnterminatedYieldsEmpty) {
    EXPECT_TRUE(extractPresenterPage("<html></html>").empty());
    EXPECT_TRUE(extractPresenterPage("<template id=\"tpPresenterPage\">never closed").empty());
}

// The real embedded UI must carry a usable presenter page (this is what the Windows host loads).
TEST(TpPage, ShippedIndexHtmlHasAPresenterPage) {
    std::ifstream f(std::string(HB_SOURCE_DIR) + "/ui/index.html", std::ios::binary);
    ASSERT_TRUE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto page = extractPresenterPage(ss.str());
    ASSERT_FALSE(page.empty());
    EXPECT_EQ(page.rfind("<!doctype html>", 0), 0u);
    EXPECT_NE(page.find("chrome.webview"), std::string::npos);
    EXPECT_LT(page.size(), 200u * 1024u);   // NavigateToString is capped at 2 MB; keep it tiny
}
