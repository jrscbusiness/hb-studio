#include <gtest/gtest.h>

#include "captions/text_normalizer.hpp"

using namespace hb::captions;

TEST(CaptionsText, CleanStripsAnnotationsAndCollapsesSpaces) {
    EXPECT_EQ(cleanText("  olá   mundo \n"), "olá mundo");
    EXPECT_EQ(cleanText("[BLANK_AUDIO]"), "");
    EXPECT_EQ(cleanText("(música) boa noite [aplausos] a todos"), "boa noite a todos");
    EXPECT_EQ(cleanText("\xE2\x99\xAA la la \xE2\x99\xAB"), "la la");
    EXPECT_EQ(cleanText("texto (sem fechar"), "texto");   // unterminated annotation is dropped
    EXPECT_EQ(cleanText(std::string("a\x01" "b")), "ab");
    EXPECT_EQ(cleanText(""), "");
}

TEST(CaptionsText, CapitalizesAndPunctuatesFinals) {
    NormalizeOptions o;
    EXPECT_EQ(normalizeCaption("bom dia a todos", o, true), "Bom dia a todos.");
    EXPECT_EQ(normalizeCaption("bom dia a todos", o, false), "Bom dia a todos");   // partial: no full stop
    EXPECT_EQ(normalizeCaption("já é hora? sim. agora", o, true), "Já é hora? Sim. Agora.");
    EXPECT_EQ(normalizeCaption("água pura", o, true), "Água pura.");               // Latin-1 capital
    EXPECT_EQ(normalizeCaption("já terminou!", o, true), "Já terminou!");
    EXPECT_EQ(normalizeCaption("espere\xE2\x80\xA6", o, true), "Espere\xE2\x80\xA6");
    EXPECT_EQ(normalizeCaption("123 itens", o, true), "123 itens.");
    o.capitalize = false;
    o.finalPunctuation = false;
    EXPECT_EQ(normalizeCaption("bom dia", o, true), "bom dia");
}

TEST(CaptionsText, ProfanityFilterMasksWholeWordsOnly) {
    NormalizeOptions o;
    o.profanityFilter = true;
    EXPECT_EQ(normalizeCaption("que merda, cara", o, true), "Que m****, cara.");
    EXPECT_EQ(normalizeCaption("PORRA!", o, false), "P****!");
    EXPECT_EQ(normalizeCaption("Otário", o, false), "O*****");           // accented, capital
    EXPECT_EQ(normalizeCaption("computador e pútrido", o, false), "Computador e pútrido");   // no substring masking
    o.profanityFilter = false;
    EXPECT_EQ(normalizeCaption("que merda", o, false), "Que merda");
}

TEST(CaptionsText, WrapsByCodePoints) {
    const auto l = wrapLines("um dois três quatro cinco seis", 12);
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l[0], "um dois três");   // 12 code points even though more bytes
    EXPECT_EQ(l[1], "quatro cinco");
    EXPECT_EQ(l[2], "seis");
    for (const auto& s : l) EXPECT_LE(utf8Length(s), 12u);
}

TEST(CaptionsText, WrapHardSplitsLongWordsWithoutBreakingUtf8) {
    const auto l = wrapLines("ççççççççççççç", 5);   // 14 x 2-byte chars
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(utf8Length(l[0]), 5u);
    EXPECT_EQ(utf8Length(l[1]), 5u);
    EXPECT_EQ(utf8Length(l[2]), 4u);
    EXPECT_EQ(l[0], "ççççç");
}

TEST(CaptionsText, PaginateGroupsLines) {
    const auto p = paginate("um dois três quatro cinco seis", 12, 2);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0], "um dois três\nquatro cinco");
    EXPECT_EQ(p[1], "seis");
    EXPECT_TRUE(paginate("", 10, 2).empty());
    EXPECT_EQ(paginate("a b", 10, 0).size(), 1u);   // maxLines 0 treated as 1
}

TEST(CaptionsText, Utf8LengthToleratesInvalidBytes) {
    EXPECT_EQ(utf8Length("a\xC3"), 2u);   // truncated sequence counts as one char per byte
    EXPECT_EQ(utf8Length("\xFF\xFE"), 2u);
    EXPECT_EQ(utf8Length(""), 0u);
}
