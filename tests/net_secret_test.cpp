#include <gtest/gtest.h>

#include "net/secret_store.hpp"

using namespace hb::net;

TEST(NetSecretStore, FakeRoundTrip) {
    FakeSecretStore s;
    std::string blob, plain;
    ASSERT_TRUE(s.protect("sk-secret", blob));
    EXPECT_EQ(blob.find("sk-secret"), std::string::npos);
    ASSERT_TRUE(s.unprotect(blob, plain));
    EXPECT_EQ(plain, "sk-secret");
}

TEST(NetSecretStore, FakeFailureHooksAndForeignBlobs) {
    FakeSecretStore s;
    std::string blob, plain;
    s.failProtect = true;
    EXPECT_FALSE(s.protect("x", blob));
    s.failProtect = false;
    EXPECT_FALSE(s.unprotect("not-a-blob", plain));
    ASSERT_TRUE(s.protect("x", blob));
    s.failUnprotect = true;
    EXPECT_FALSE(s.unprotect(blob, plain));
}

#ifndef _WIN32
TEST(NetSecretStore, NoDpapiOffWindows) { EXPECT_EQ(makeDpapiSecretStore(), nullptr); }
#endif
