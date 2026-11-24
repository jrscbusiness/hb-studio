#include <gtest/gtest.h>

#include "captions/committer.hpp"

using namespace hb::captions;

TEST(CaptionsCommitter, GrowingPartialsEmitEachChange) {
    CaptionCommitter c;
    auto u = c.onPartial(1, "bom");
    ASSERT_TRUE(u.emit);
    EXPECT_EQ(u.text, "bom");
    u = c.onPartial(1, "bom dia");
    ASSERT_TRUE(u.emit);
    EXPECT_EQ(u.text, "bom dia");
    EXPECT_EQ(c.stableWords(), 1u);   // "bom" agreed by two hypotheses
}

TEST(CaptionsCommitter, PunctuationAndCaseFlipsDoNotFlicker) {
    CaptionCommitter c;
    c.onPartial(1, "olá, pessoal");
    EXPECT_FALSE(c.onPartial(1, "Olá pessoal").emit);
    EXPECT_FALSE(c.onPartial(1, "olá pessoal.").emit);
    EXPECT_EQ(c.shown(), "olá, pessoal");   // first spelling kept
    const auto u = c.onPartial(1, "Olá pessoal tudo bem");
    ASSERT_TRUE(u.emit);
    EXPECT_EQ(u.text, "olá, pessoal tudo bem");   // agreed prefix keeps its shown spelling
}

TEST(CaptionsCommitter, ShorterPrefixHypothesisIsNotRetracted) {
    CaptionCommitter c;
    c.onPartial(1, "hoje vamos falar de futebol");
    EXPECT_FALSE(c.onPartial(1, "hoje vamos falar").emit);
    EXPECT_EQ(c.shown(), "hoje vamos falar de futebol");
}

TEST(CaptionsCommitter, TailRewriteIsShownButStablePrefixKept) {
    CaptionCommitter c;
    c.onPartial(1, "eu quero uma casa");
    const auto u = c.onPartial(1, "eu quero uma caça ao tesouro");
    ASSERT_TRUE(u.emit);
    EXPECT_EQ(u.text, "eu quero uma caça ao tesouro");
}

TEST(CaptionsCommitter, FinalCommitsAndResets) {
    CaptionCommitter c;
    c.onPartial(1, "boa noite");
    auto f = c.onFinal(1, "Boa noite a todos");
    ASSERT_TRUE(f.emit);
    EXPECT_EQ(f.text, "Boa noite a todos");
    EXPECT_TRUE(c.shown().empty());
    // new utterance starts clean
    EXPECT_TRUE(c.onPartial(2, "boa").emit);
}

TEST(CaptionsCommitter, DuplicateMultiWordFinalSuppressedShortOnesAllowed) {
    CaptionCommitter c;
    EXPECT_TRUE(c.onFinal(1, "Obrigado por assistir").emit);
    EXPECT_FALSE(c.onFinal(2, "obrigado por assistir!").emit);
    EXPECT_TRUE(c.onFinal(3, "Amém").emit);
    EXPECT_TRUE(c.onFinal(4, "Amém").emit);
}

TEST(CaptionsCommitter, EmptyFinalClearsShownPartialOnly) {
    CaptionCommitter c;
    c.onPartial(1, "algo");
    auto f = c.onFinal(1, "");
    EXPECT_TRUE(f.emit);
    EXPECT_TRUE(f.text.empty());
    EXPECT_FALSE(c.onFinal(2, "").emit);   // nothing shown, nothing to clear
    EXPECT_FALSE(c.onPartial(3, "").emit);
}
