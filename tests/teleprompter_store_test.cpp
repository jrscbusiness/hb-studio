#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>

#include "teleprompter/script.hpp"
#include "teleprompter/store.hpp"

using namespace hb::teleprompter;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path p;
    TempDir() {
        static int n = 0;
        p = fs::temp_directory_path() / ("hb_tp_store_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                         "_" + std::to_string(++n) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(p);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(p, ec); }
};
}  // namespace

TEST(TpStore, SanitizeName) {
    EXPECT_EQ(sanitizeName("  Meu   roteiro\t\n1 "), "Meu roteiro 1");
    EXPECT_EQ(sanitizeName(""), "Sem título");
    EXPECT_EQ(sanitizeName("   "), "Sem título");
    EXPECT_EQ(sanitizeName("../../evil"), "../../evil");   // harmless: names never become file names
    const auto longName = sanitizeName(std::string(200, 'x'));
    EXPECT_EQ(longName.size(), kMaxNameBytes);
    // never cut inside a UTF-8 sequence
    std::string acc;
    for (int i = 0; i < 60; ++i) acc += "ã";
    const auto cut = sanitizeName(acc);
    EXPECT_LE(cut.size(), kMaxNameBytes);
    EXPECT_EQ(cut.size() % 2, 0u);
}

TEST(TpStore, IdValidation) {
    EXPECT_TRUE(isValidId("s1"));
    EXPECT_TRUE(isValidId("s123456789"));
    EXPECT_FALSE(isValidId(""));
    EXPECT_FALSE(isValidId("s"));
    EXPECT_FALSE(isValidId("1"));
    EXPECT_FALSE(isValidId("../s1"));
    EXPECT_FALSE(isValidId("s1.txt"));
    EXPECT_FALSE(isValidId("s12345678901"));
}

TEST(TpStore, CreateLoadSaveRenameDelete) {
    TempDir d;
    ScriptStore st(d.p);
    const auto id = st.create("Abertura", "Olá mundo");
    ASSERT_FALSE(id.empty());
    std::string t;
    ASSERT_TRUE(st.load(id, t));
    EXPECT_EQ(t, "Olá mundo");
    ASSERT_TRUE(st.save(id, "novo texto aqui"));
    ASSERT_TRUE(st.load(id, t));
    EXPECT_EQ(t, "novo texto aqui");
    ASSERT_TRUE(st.rename(id, "Encerramento"));
    auto l = st.list();
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].name, "Encerramento");
    EXPECT_EQ(l[0].words, 3);
    ASSERT_TRUE(st.remove(id));
    EXPECT_TRUE(st.list().empty());
    EXPECT_FALSE(st.load(id, t));
    EXPECT_FALSE(st.remove(id));
}

TEST(TpStore, PersistsAcrossInstancesWithAccents) {
    TempDir d;
    std::string id;
    {
        ScriptStore a(d.p);
        id = a.create("Coração & Ação 😀", "x");
    }
    ScriptStore b(d.p);
    const auto l = b.list();
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].id, id);
    EXPECT_EQ(l[0].name, "Coração & Ação 😀");
}

TEST(TpStore, DuplicateNamesGetSuffixAndSortIsCaseInsensitive) {
    TempDir d;
    ScriptStore st(d.p);
    st.create("b", "");
    st.create("B", "");
    st.create("a", "");
    const auto l = st.list();
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l[0].name, "a");
    EXPECT_EQ(l[1].name, "b");
    EXPECT_EQ(l[2].name, "B (2)");
}

TEST(TpStore, IdsAreNotReusedWhileHigherExist) {
    TempDir d;
    ScriptStore st(d.p);
    const auto a = st.create("a");
    const auto b = st.create("b");
    EXPECT_NE(a, b);
    st.remove(a);
    const auto c = st.create("c");
    EXPECT_NE(c, b);
}

TEST(TpStore, ImportNormalisesEncoding) {
    TempDir d;
    ScriptStore st(d.p);
    const auto id = st.importText("antigo", "cora\xE7\xE3o\r\nlinha 2");
    ASSERT_FALSE(id.empty());
    std::string t;
    ASSERT_TRUE(st.load(id, t));
    EXPECT_EQ(t, "coração\nlinha 2");
}

TEST(TpStore, RejectsOversizeAndBadIds) {
    TempDir d;
    ScriptStore st(d.p);
    EXPECT_TRUE(st.create("big", std::string(kMaxScriptBytes + 1, 'a')).empty());
    const auto id = st.create("ok");
    EXPECT_FALSE(st.save(id, std::string(kMaxScriptBytes + 1, 'a')));
    EXPECT_FALSE(st.save("../x", "t"));
    EXPECT_FALSE(st.save("s999", "t"));   // saving never creates by itself
    std::string t;
    EXPECT_FALSE(st.load("..\\x", t));
}

TEST(TpStore, EnforcesScriptLimit) {
    TempDir d;
    ScriptStore st(d.p);
    for (std::size_t i = 0; i < kMaxScripts; ++i) ASSERT_FALSE(st.create("n" + std::to_string(i)).empty());
    EXPECT_TRUE(st.create("one too many").empty());
}

TEST(TpStore, SurvivesLostIndexAndStaleIndexLines) {
    TempDir d;
    ScriptStore st(d.p);
    const auto a = st.create("Nome");
    st.create("Outro");
    fs::remove(d.p / "index.tsv");
    auto l = st.list();
    ASSERT_EQ(l.size(), 2u);   // adopted from the .txt files, named by id
    {
        std::ofstream f(d.p / "index.tsv", std::ios::binary | std::ios::trunc);
        f << a << "\tNome\ns77\tFantasma\nlixo sem tab\n../x\tbad\n";
    }
    l = st.list();
    bool sawGhost = false;
    for (const auto& i : l) sawGhost |= i.id == "s77";
    EXPECT_FALSE(sawGhost);
    EXPECT_EQ(l.size(), 2u);
}
