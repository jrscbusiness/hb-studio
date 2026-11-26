#include <gtest/gtest.h>

#include "app/command_router.hpp"
#include "app/json_escape.hpp"

using namespace hb::app;

TEST(CommandSplit, SplitsAndKeepsTheRestInTheLastPart) {
    EXPECT_EQ(splitCommand("cut", 8), (std::vector<std::string>{"cut"}));
    EXPECT_EQ(splitCommand("preview|3", 8), (std::vector<std::string>{"preview", "3"}));
    EXPECT_EQ(splitCommand("ptzname|1|2|a|b|c", 4), (std::vector<std::string>{"ptzname", "1", "2", "a|b|c"}));
    EXPECT_EQ(splitCommand("xmitsave|{\"a\":\"x|y\"}", 2), (std::vector<std::string>{"xmitsave", "{\"a\":\"x|y\"}"}));
}

TEST(CommandSplit, EmptyAndEdgeCases) {
    EXPECT_EQ(splitCommand("", 8), (std::vector<std::string>{""}));
    EXPECT_EQ(splitCommand("a||b", 8), (std::vector<std::string>{"a", "", "b"}));
    EXPECT_EQ(splitCommand("a|", 8), (std::vector<std::string>{"a", ""}));
    EXPECT_EQ(splitCommand("a|b|c", 1), (std::vector<std::string>{"a|b|c"}));
    EXPECT_EQ(commandName("tbar|0.5"), "tbar");
    EXPECT_EQ(commandName("cut"), "cut");
    EXPECT_EQ(commandName(""), "");
}

TEST(CommandRouter, DispatchesRegisteredCommandsOnly) {
    CommandRouter r;
    int calls = 0;
    std::string seen;
    r.add("preview", [&](const Command& c) { ++calls; seen = c.name() + ":" + std::to_string(c.num(1)); }, 2);
    EXPECT_TRUE(r.dispatch("preview|3"));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, "preview:3");
    EXPECT_FALSE(r.dispatch("unknown|1"));
    EXPECT_FALSE(r.dispatch("previews|1"));   // exact name match, no prefix
    EXPECT_FALSE(r.dispatch(""));
    EXPECT_EQ(calls, 1);
}

TEST(CommandRouter, TooFewPartsIsConsumedButHandlerNotCalled) {
    CommandRouter r;
    int calls = 0;
    r.add("tbar", [&](const Command&) { ++calls; }, 2);
    EXPECT_TRUE(r.dispatch("tbar"));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(r.dispatch("tbar|0.5"));
    EXPECT_EQ(calls, 1);
}

TEST(CommandRouter, PerCommandPartLimitAndArgHelpers) {
    CommandRouter r;
    std::vector<std::string> got;
    int missing = 0;
    r.add("ptzname", [&](const Command& c) { got = c.a; missing = c.num(9); }, 4, 4);
    EXPECT_TRUE(r.dispatch("ptzname|1|2|name|with|bars"));
    EXPECT_EQ(got, (std::vector<std::string>{"ptzname", "1", "2", "name|with|bars"}));
    EXPECT_EQ(missing, -1);
}

TEST(CommandRouter, NumUsesAtoiSemanticsAndStrIsSafe) {
    CommandRouter r;
    int n1 = 0, n2 = 0, n3 = 0;
    std::string s = "unset";
    r.add("x", [&](const Command& c) { n1 = c.num(1); n2 = c.num(2); n3 = c.num(3); s = c.str(7); });
    r.dispatch("x|12|abc|-4");
    EXPECT_EQ(n1, 12);
    EXPECT_EQ(n2, 0);
    EXPECT_EQ(n3, -4);
    EXPECT_EQ(s, "");
}

TEST(CommandRouter, ReRegistrationReplacesAndRouterIsCopyable) {
    CommandRouter r;
    int which = 0;
    r.add("a", [&](const Command&) { which = 1; });
    r.add("a", [&](const Command&) { which = 2; });
    EXPECT_TRUE(r.has("a"));
    EXPECT_FALSE(r.has("b"));
    r.dispatch("a");
    EXPECT_EQ(which, 2);
    const CommandRouter copy = r;   // std::function targets are copyable
    which = 0;
    copy.dispatch("a");
    EXPECT_EQ(which, 2);
}

TEST(JsonEscape, MatchesTheLegacyWindowsHelper) {
    EXPECT_EQ(jsonEscape("plain"), "plain");
    EXPECT_EQ(jsonEscape("a\"b\\c"), "a\\\"b\\\\c");
    EXPECT_EQ(jsonEscape("l1\nl2\tx\r"), "l1\\nl2\\tx");
    EXPECT_EQ(jsonEscape(std::string("\x01", 1)), "\\u0001");
    EXPECT_EQ(jsonEscape("ação · 日本"), "ação · 日本");
}
