#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "LocalView.h"
#include "Ops.h"
#include "StdFs.h"
#include "Store.h"
#include "TestUtil.h"
#include "WordLens.h"

using namespace linguetta;
using namespace linguetta::lens;

namespace {

class Lens : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/linguetta-lens-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir = tmpl;
    fs = std::make_unique<StdFs>(dir);
    fs->mkdirs(paths::ROOT);
    fs->mkdirs(paths::BANK_DIR);
    put(bankPath("it"), readFixture("bank.jsonl"));
  }
  void TearDown() override { std::system(("rm -rf '" + dir + "'").c_str()); }

  void put(const std::string& path, const std::string& content) {
    std::ofstream out(fs->real(path), std::ios::binary);
    out << content;
  }

  Index build(Level level) {
    LocalView view;
    view.load(*fs, "it");
    Index idx;
    idx.build(*fs, view, level);
    return idx;
  }

  std::string dir;
  std::unique_ptr<StdFs> fs;
};

std::vector<Token> tokens(std::initializer_list<const char*> words) {
  std::vector<Token> out;
  for (const char* w : words) {
    Token t;
    if (w[0] == '|') {  // "|word": starts a paragraph
      t.paragraphStart = true;
      w++;
    }
    t.word = wordKey(w);
    out.push_back(t);
  }
  return out;
}

std::string glossAt(const Index& idx, const std::vector<Span>& spans, size_t i) {
  return idx.gloss(spans[i].gloss).text;
}

}  // namespace

TEST(WordLensKeys, StripsPunctuationAndLowers) {
  const WordKey w = wordKey("«Bella,");
  EXPECT_EQ(w.key, "bella");
  EXPECT_TRUE(w.breakBefore);
  EXPECT_TRUE(w.breakAfter);

  const WordKey plain = wordKey("Città");
  EXPECT_EQ(plain.key, "citt\xC3\xA0");
  EXPECT_FALSE(plain.breakBefore);
  EXPECT_FALSE(plain.breakAfter);

  EXPECT_EQ(wordKey("mare…").key, "mare");
  EXPECT_TRUE(wordKey("mare…").breakAfter);
  EXPECT_EQ(wordKey("—").key, "");
  EXPECT_EQ(wordKey("12").key, "");
}

TEST(WordLensKeys, ElidedArticle) {
  const WordKey w = wordKey("l\xE2\x80\x99" "acqua");
  EXPECT_EQ(w.key, "l'acqua");
  EXPECT_EQ(w.elided, "acqua");
  EXPECT_EQ(wordKey("un'onda").elided, "onda");
  EXPECT_EQ(wordKey("po'").elided, "");
}

TEST(WordLensKeys, ItemKeyNormalizesEveryWord) {
  EXPECT_EQ(itemKey("  Fare finta di "), "fare finta di");
  EXPECT_EQ(itemKey("Il Mare!"), "il mare");
}

TEST(WordLensLevels, StabilityBands) {
  ViewEntry e;
  e.card.state = 2;
  e.card.stability = 12;
  EXPECT_TRUE(shows(e, Level::Muita));
  EXPECT_TRUE(shows(e, Level::Media));
  EXPECT_FALSE(shows(e, Level::Pouca));
  EXPECT_FALSE(shows(e, Level::Off));
  e.card.stability = 45;
  EXPECT_TRUE(shows(e, Level::Muita));
  EXPECT_FALSE(shows(e, Level::Media));
  e.card.state = 3;  // relearning: always
  EXPECT_TRUE(shows(e, Level::Pouca));
  e.card.state = 0;
  EXPECT_TRUE(shows(e, Level::Pouca));
  e.known = true;
  EXPECT_FALSE(shows(e, Level::Muita));
}

TEST(WordLensLevels, CodesRoundTrip) {
  for (const Level l : {Level::Off, Level::Muita, Level::Media, Level::Pouca}) {
    Level back = Level::Off;
    ASSERT_TRUE(parseLevel(levelCode(l), back));
    EXPECT_EQ(back, l);
  }
  Level x;
  EXPECT_FALSE(parseLevel("lots", x));
  EXPECT_STREQ(levelLabel(Level::Media), "M\xC3\xA9" "dia");
}

TEST_F(Lens, IndexFollowsTheLevel) {
  const Index muita = build(Level::Muita);
  EXPECT_EQ(muita.size(), 4u);  // everything but the Conhecido item
  EXPECT_LT(muita.find("ricordarsi"), 0);
  EXPECT_LT(muita.find("ricordo"), 0);

  const Index media = build(Level::Media);
  EXPECT_EQ(media.size(), 4u);  // stabilities 12.3 and 9.9 are under 30 days

  const Index pouca = build(Level::Pouca);
  EXPECT_EQ(pouca.size(), 2u);  // only the learning and the new card
  EXPECT_LT(pouca.find("il mare"), 0);
  EXPECT_LT(pouca.find("bella"), 0);
  EXPECT_GE(pouca.find("sorridere"), 0);
  EXPECT_GE(pouca.find("facevo finta di"), 0);

  EXPECT_TRUE(build(Level::Off).empty());
}

TEST_F(Lens, KeysCoverFormsAndEncounters) {
  const Index idx = build(Level::Muita);
  const int g = idx.find("bella");
  ASSERT_GE(g, 0);
  EXPECT_EQ(idx.gloss(g).lemma, "bello");
  EXPECT_EQ(idx.gloss(g).text, "bonito");
  EXPECT_EQ(idx.find("bello"), g);
  EXPECT_EQ(idx.gloss(idx.find("facevo finta di")).text, "fingir");
}

TEST_F(Lens, MatchesSequencesWithinAPhrase) {
  const Index idx = build(Level::Muita);
  auto toks = tokens({"Guardava", "il", "mare", "e", "Facevo", "finta", "di", "niente."});
  auto spans = idx.match(toks);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].first, 1u);
  EXPECT_EQ(spans[0].count, 2u);
  EXPECT_EQ(glossAt(idx, spans, 0), "o mar");
  EXPECT_EQ(spans[1].first, 4u);
  EXPECT_EQ(spans[1].count, 3u);
  EXPECT_EQ(glossAt(idx, spans, 1), "fingir");

  // Punctuation or a new paragraph breaks the sequence.
  EXPECT_TRUE(idx.match(tokens({"il,", "mare"})).empty());
  EXPECT_TRUE(idx.match(tokens({"il", "|mare"})).empty());
  EXPECT_TRUE(idx.match(tokens({"il", "«mare»"})).empty());

  // Single words with punctuation and an elided article.
  spans = idx.match(tokens({"«Bella!»", "l'bella"}));
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[1].first, 1u);
}

TEST_F(Lens, OfflineSaveGetsAGloss) {
  SaveInput in;
  in.language = "it";
  in.lemma = "onda";
  in.form = "onde";
  in.encounterSense = "ondas";
  in.sentence = "Le onde.";
  put(paths::OUTBOX, buildSave("c1", 1700000000000, in) + "\n");
  const Index idx = build(Level::Pouca);
  const int g = idx.find("onde");
  ASSERT_GE(g, 0);
  EXPECT_EQ(idx.gloss(g).text, "ondas");  // no item sense: the encounter's
  EXPECT_EQ(idx.find("onda"), g);
}

TEST_F(Lens, StateRoundTripAndDailyReset) {
  State st = loadState(*fs);
  EXPECT_EQ(st.level, DEFAULT_LEVEL);
  EXPECT_TRUE(st.markSeen(100, "book", "bello"));
  EXPECT_FALSE(st.markSeen(100, "book", "bello"));
  EXPECT_TRUE(st.markSeen(100, "other", "bello"));
  st.level = Level::Pouca;
  ASSERT_TRUE(saveState(*fs, st));

  State back = loadState(*fs);
  EXPECT_EQ(back.level, Level::Pouca);
  EXPECT_EQ(back.day, 100);
  EXPECT_FALSE(back.markSeen(100, "book", "bello"));
  EXPECT_TRUE(back.markSeen(200, "book", "bello"));  // new day
  EXPECT_EQ(back.seen.size(), 1u);
}
