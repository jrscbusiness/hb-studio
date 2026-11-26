// Guards the security invariants of the Windows-only sources, which no Linux test can execute:
// TLS verification must never be relaxed anywhere in src/net.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace {
std::string slurp(const std::string& name) {
    std::ifstream f(std::string(HB_NET_SRC_DIR) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST(NetSourceGuard, WinHttpNeverRelaxesCertificateChecks) {
    const std::string src = slurp("http_win.cpp");
    ASSERT_FALSE(src.empty());
    // The header comment names the forbidden constants, so look only at code after the includes.
    const std::string code = src.substr(src.find("namespace hb::net"));
    EXPECT_EQ(code.find("SECURITY_FLAG_IGNORE"), std::string::npos);
    EXPECT_EQ(code.find("WINHTTP_OPTION_SECURITY_FLAGS"), std::string::npos);
    EXPECT_EQ(code.find("WINHTTP_OPTION_CLIENT_CERT_CONTEXT"), std::string::npos);
    EXPECT_NE(code.find("REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP"), std::string::npos);
}
