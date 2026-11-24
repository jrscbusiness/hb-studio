#include <gtest/gtest.h>

#include "teleprompter/script.hpp"

using namespace hb::teleprompter;

TEST(TpScript, CountWords) {
    EXPECT_EQ(countWords(""), 0);
    EXPECT_EQ(countWords("  \n\t "), 0);
    EXPECT_EQ(countWords("um dois  três\nquatro"), 4);
    EXPECT_EQ(countWords("ação"), 1);
}

TEST(TpScript, ParagraphsAndCues) {
    const auto s = parseScript("# abertura\nOlá a todos\nvocês aqui\n\n\n#### pausa  \nSegundo bloco.\n");
    ASSERT_EQ(s.blocks.size(), 4u);
    EXPECT_EQ(s.blocks[0].kind, Block::Kind::Cue);
    EXPECT_EQ(s.blocks[0].text, "abertura");
    EXPECT_EQ(s.blocks[0].words, 0);
    EXPECT_EQ(s.blocks[1].kind, Block::Kind::Paragraph);
    EXPECT_EQ(s.blocks[1].text, "Olá a todos\nvocês aqui");
    EXPECT_EQ(s.blocks[1].words, 5);
    EXPECT_EQ(s.blocks[1].line, 2);
    EXPECT_EQ(s.blocks[2].kind, Block::Kind::Cue);
    EXPECT_EQ(s.blocks[2].text, "pausa");
    EXPECT_EQ(s.blocks[3].text, "Segundo bloco.");
    EXPECT_EQ(s.totalWords, 7);
    EXPECT_EQ(s.paragraphs, 2);
}

TEST(TpScript, CueBreaksParagraphAndCrlfAndLeadingSpace) {
    const auto s = parseScript("linha a\r\n   # nota\r\nlinha b\r\n");
    ASSERT_EQ(s.blocks.size(), 3u);
    EXPECT_EQ(s.blocks[0].text, "linha a");
    EXPECT_EQ(s.blocks[1].kind, Block::Kind::Cue);
    EXPECT_EQ(s.blocks[2].text, "linha b");
}

TEST(TpScript, EmptyAndOnlyCues) {
    EXPECT_TRUE(parseScript("").blocks.empty());
    const auto s = parseScript("# so nota");
    EXPECT_EQ(s.totalWords, 0);
    EXPECT_EQ(s.paragraphs, 0);
    EXPECT_TRUE(paragraphStarts(s).empty());
}

TEST(TpScript, ParagraphStartsAreWordFractions) {
    const auto s = parseScript("a b c d\n\n# x\n\ne f\n\ng h i j\n");   // 4 + 2 + 4 = 10 words
    const auto st = paragraphStarts(s);
    ASSERT_EQ(st.size(), 3u);
    EXPECT_DOUBLE_EQ(st[0], 0.0);
    EXPECT_DOUBLE_EQ(st[1], 0.4);
    EXPECT_DOUBLE_EQ(st[2], 0.6);
}

TEST(TpScript, Utf8Validation) {
    EXPECT_TRUE(isValidUtf8("ação ✓ 😀"));
    EXPECT_FALSE(isValidUtf8("a\xE7\xE3o"));       // Latin-1 bytes
    EXPECT_FALSE(isValidUtf8("\xE2\x82"));          // truncated
    EXPECT_FALSE(isValidUtf8("\xC0\x80"));          // overlong
    EXPECT_FALSE(isValidUtf8("\xED\xA0\x80"));      // surrogate
}

TEST(TpScript, NormalizeBomCrlfNul) {
    EXPECT_EQ(normalizeText(std::string("\xEF\xBB\xBF") + "a\r\nb\rc"), "a\nb\nc");
    EXPECT_EQ(normalizeText(std::string("a\0b", 3)), "ab");
}

TEST(TpScript, NormalizeLegacyEncodingToUtf8) {
    // "coração" in Windows-1252: ç = E7, ã = E3; and a curly quote 0x93.
    EXPECT_EQ(normalizeText("cora\xE7\xE3o \x93x"), "coração \xE2\x80\x9Cx");
    EXPECT_EQ(normalizeText("ação"), "ação");   // valid UTF-8 is untouched
}

TEST(TpScript, NormalizeTruncatesOnCharBoundary) {
    std::string big(kMaxScriptBytes - 1, 'a');
    big += "ã";   // 2-byte char straddling the limit
    big += "bbbb";
    const auto n = normalizeText(big);
    EXPECT_LE(n.size(), kMaxScriptBytes);
    EXPECT_TRUE(isValidUtf8(n));
}
