#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "LocalView.h"
#include "Ops.h"
#include "StdFs.h"
#include "Store.h"
#include "TestUtil.h"
#include "Text.h"

using namespace linguetta;

namespace {

class SdCard : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/linguetta-test-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir = tmpl;
    fs = std::make_unique<StdFs>(dir);
    fs->mkdirs(paths::ROOT);
    fs->mkdirs(paths::BANK_DIR);
  }
  void TearDown() override { std::system(("rm -rf '" + dir + "'").c_str()); }

  void put(const std::string& path, const std::string& content) {
    std::ofstream out(fs->real(path), std::ios::binary);
    out << content;
  }
  std::string get(const std::string& path) {
    std::string out;
    readFile(*fs, path, out, 1 << 20);
    return out;
  }
  int64_t at(const char* iso) {
    int64_t ms = 0;
    parseIso(iso, ms);
    return ms;
  }

  std::string dir;
  std::unique_ptr<StdFs> fs;
};

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

}  // namespace

TEST_F(SdCard, StateRoundTripsTheFixture) {
  put(paths::STATE, readFixture("state.json"));
  SyncState st;
  ASSERT_TRUE(loadState(*fs, st));
  EXPECT_TRUE(st.paired());
  EXPECT_EQ(st.tokenHint, "iT1k");
  EXPECT_EQ(st.account.email, "matheus@example.com");
  ASSERT_EQ(st.account.studyLanguages.size(), 2u);
  EXPECT_EQ(st.account.studyLanguages[1], "en");
  EXPECT_EQ(st.language, "it");
  EXPECT_EQ(st.cursor, "v1.MjAyNi0xMS0xMVQyMTozMDowMi40MTdaOjg4MjEz");
  // A state.json without "cursors" (the contract fixture) seeds the language's cursor.
  EXPECT_EQ(st.cursors.at("it"), st.cursor);
  EXPECT_TRUE(st.lastError.empty());

  st.failures = 2;
  ASSERT_TRUE(saveState(*fs, st));
  SyncState again;
  ASSERT_TRUE(loadState(*fs, again));
  EXPECT_EQ(again.token, st.token);
  EXPECT_EQ(again.account.id, st.account.id);
  EXPECT_EQ(again.failures, 2);
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, get(paths::STATE)), DeserializationError::Ok);
  EXPECT_TRUE(doc["lastError"].isNull());
  EXPECT_EQ(doc["v"].as<int>(), 1);
  EXPECT_FALSE(fs->exists(std::string(paths::STATE) + ".tmp"));
}

TEST_F(SdCard, SettingsOverrideTheServer) {
  EXPECT_EQ(loadSettings(*fs).server, DEFAULT_SERVER);
  put(paths::SETTINGS_FILE, R"({"server":"http://192.168.0.10:5173/","deviceName":"Meu X4"})");
  const Settings s = loadSettings(*fs);
  EXPECT_EQ(s.server, "http://192.168.0.10:5173");
  EXPECT_EQ(s.deviceName, "Meu X4");
  put(paths::SETTINGS_FILE, R"({"server":"ftp://nope"})");
  EXPECT_EQ(loadSettings(*fs).server, DEFAULT_SERVER);
}

TEST_F(SdCard, OutboxIgnoresATornLastLine) {
  const std::string fixture = readFixture("outbox.jsonl");
  put(paths::OUTBOX, fixture + "{\"clientId\":\"x\",\"type\":\"se");
  std::vector<std::string> ids;
  forEachOutbox(*fs, [&](const OutboxLine& l) {
    ids.push_back(l.clientId);
    return true;
  });
  ASSERT_EQ(ids.size(), 9u);
  EXPECT_EQ(ids.front(), "d4b3a2c1-0f9e-4d8c-b7a6-958473625100");
  EXPECT_EQ(ids.back(), "d4b3a2c1-0f9e-4d8c-b7a6-958473625108");
  const OutboxCounts c = countOutbox(*fs);
  EXPECT_EQ(c.saves, 3);
  EXPECT_EQ(c.reviews, 3);
  EXPECT_EQ(c.seen, 1);
  EXPECT_EQ(c.other, 2);  // extraNew + deckImport
  EXPECT_EQ(c.total(), 9);

  // Appending after a torn line starts on a fresh line, so the new op survives.
  ASSERT_TRUE(appendOutbox(*fs, buildSeen("new-id", at("2026-11-12T10:00:00Z"), "it", "bello")));
  ids.clear();
  forEachOutbox(*fs, [&](const OutboxLine& l) {
    ids.push_back(l.clientId);
    return true;
  });
  ASSERT_EQ(ids.size(), 10u);
  EXPECT_EQ(ids.back(), "new-id");
}

TEST_F(SdCard, OutboxRewriteDropsResolvedOps) {
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  ASSERT_TRUE(rewriteOutboxWithout(*fs, {"d4b3a2c1-0f9e-4d8c-b7a6-958473625100", "d4b3a2c1-0f9e-4d8c-b7a6-958473625105"}));
  const auto kept = lines(get(paths::OUTBOX));
  ASSERT_EQ(kept.size(), 7u);
  EXPECT_NE(kept[0].find("625101"), std::string::npos);
  // Lines are kept byte for byte.
  EXPECT_EQ(kept[0], lines(readFixture("outbox.jsonl"))[1]);
}

TEST_F(SdCard, RejectedKeepsTheLast100) {
  std::vector<std::string> many;
  for (int i = 0; i < 130; i++) many.push_back("{\"n\":" + std::to_string(i) + "}");
  appendRejected(*fs, many);
  const auto kept = lines(get(paths::REJECTED));
  ASSERT_EQ(kept.size(), 100u);
  EXPECT_EQ(kept.front(), "{\"n\":30}");
}

TEST(Ops, SaveCutsToContractLimits) {
  SaveInput in;
  in.language = "it";
  in.lemma = "  " + std::string(90, 'a') + " ";
  in.sense = "sorrir";
  in.note = "verbo";
  in.form = "Sorridere";
  in.sentence = std::string(600, 'x');
  in.encounterSense = "sorrir";
  in.chapter = "Primeiro dia";
  in.spine = 4;
  in.percent = 12.54;
  in.koHash = "a3f1c9e2b7d4068f15e2c3b4a5d6e7f8";
  in.title = "O nome da rosa";
  in.author = "Umberto Eco";
  const std::string line = buildSave("cid", 1762848930000LL, in);
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, line), DeserializationError::Ok);
  EXPECT_EQ(doc["type"], "save");
  EXPECT_EQ(doc["lemma"].as<std::string>().size(), 80u);
  EXPECT_EQ(doc["encounter"]["sentence"].as<std::string>().size(), 500u);
  EXPECT_DOUBLE_EQ(doc["encounter"]["position"]["percent"].as<double>(), 12.5);
  EXPECT_EQ(doc["encounter"]["position"]["spine"], 4);
  EXPECT_EQ(doc["work"]["kind"], "livro");
  EXPECT_EQ(doc["work"]["koHash"], "a3f1c9e2b7d4068f15e2c3b4a5d6e7f8");
  EXPECT_TRUE(doc["ipa"].isNull());
  EXPECT_EQ(line.find('\n'), std::string::npos);

  in.lemma = "   ";
  EXPECT_EQ(buildSave("cid", 0, in), "");
}

TEST(Ops, ReviewHasExactlyOneTarget) {
  JsonDocument doc;
  deserializeJson(doc, buildReview("c", 0, "it", "", "save-1", 3, 6400));
  EXPECT_TRUE(doc["flashcardId"].isNull());
  EXPECT_TRUE(doc["cardId"].isNull());
  EXPECT_EQ(doc["saveClientId"], "save-1");
  EXPECT_EQ(doc["rating"], 3);
  EXPECT_EQ(doc["durationMs"], 6400);
}

TEST_F(SdCard, LocalViewMergesBankAndOutbox) {
  put(bankPath("it"), readFixture("bank.jsonl"));
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  LocalView view;
  view.load(*fs, "it");
  EXPECT_EQ(view.language(), "it");
  EXPECT_EQ(view.header().deckName, "Linguetta \xC2\xB7 Italiano");
  EXPECT_EQ(view.header().newPerDay, 10);
  EXPECT_FALSE(view.header().fsrsApproximate);
  // 5 bank items + 1 ficha; the save with an empty lemma creates nothing; the other saves hit
  // bank items.
  EXPECT_EQ(view.size(), 6u);

  const int bello = view.find("Bello");
  ASSERT_GE(bello, 0);
  EXPECT_EQ(view.entry(bello).seenCount, 7);  // 6 in the bank + 1 seen
  ItemDetails d;
  ASSERT_TRUE(view.details(*fs, bello, d));
  EXPECT_EQ(d.lemma, "bello");
  EXPECT_EQ(d.sense, "bonito");
  EXPECT_EQ(d.encounter.chapter, "Pr\xC3\xB3logo");  // local encounter wins

  const int ricordarsi = view.find("ricordarsi");
  ASSERT_GE(ricordarsi, 0);
  EXPECT_TRUE(view.entry(ricordarsi).known);
  ASSERT_TRUE(view.details(*fs, ricordarsi, d));
  EXPECT_EQ(d.ipa, "/ri.kor\xCB\x88" "dar.si/");
  EXPECT_EQ(d.encounter.work, "Kaguya");

  // The review by saveClientId targets the bank item that save hit (sorridere).
  const int sorridere = view.find("sorridere");
  ASSERT_GE(sorridere, 0);
  EXPECT_EQ(view.entry(sorridere).card.reps, 2);
  EXPECT_EQ(view.entry(sorridere).card.lastReviewMs, at("2026-11-11T08:16:00Z"));

  const int mare = view.find("il mare");
  EXPECT_EQ(view.entry(mare).card.reps, 3);

  // Queue at sync time: sorridere (learning, due) first, then the new cards in queueRank
  // order: fare finta di (1), the ficha (2).
  const int64_t now = at("2026-11-11T21:30:03Z");
  const auto q = view.queue(now);
  ASSERT_EQ(q.size(), 3u);
  EXPECT_EQ(q[0], sorridere);
  EXPECT_EQ(q[1], view.find("fare finta di"));
  EXPECT_TRUE(view.entry(q[2]).ficha);
  EXPECT_EQ(view.newDoneToday(now), 1);
  // newAllowance 9 + the outbox's extraNew 5 (no offline new review today).
  EXPECT_EQ(view.newAllowance(now), 14);
  EXPECT_EQ(view.newLimit(now), 15);
  EXPECT_EQ(view.dayStart(now), at("2026-11-11T07:00:00Z"));
  // After nextDayStart the count resets and days are 24 h from it.
  EXPECT_EQ(view.dayStart(at("2026-11-13T09:00:00Z")), at("2026-11-13T07:00:00Z"));
  EXPECT_EQ(view.newDoneToday(at("2026-11-12T08:00:00Z")), 0);
  EXPECT_EQ(view.newAllowance(at("2026-11-12T08:00:00Z")), 10);
}

TEST_F(SdCard, LocalItemsFromOfflineSaves) {
  LocalView view;
  view.load(*fs, "it");
  EXPECT_EQ(view.size(), 0u);
  const int64_t t0 = at("2026-11-12T10:00:00Z");
  SaveInput in;
  in.language = "it";
  in.lemma = "Cavallo";
  in.sense = "cavalo";
  in.form = "cavalli";
  in.sentence = "I cavalli corrono.";
  const std::string save = buildSave("save-1", t0, in);
  ASSERT_TRUE(appendOutbox(*fs, save));
  view.applyOp(save);
  ASSERT_EQ(view.size(), 1u);
  const int i = view.find("cavallo");
  ASSERT_EQ(i, 0);
  EXPECT_EQ(view.entry(i).card.state, 0);
  EXPECT_EQ(view.entry(i).card.dueMs, t0);
  EXPECT_EQ(view.queue(t0).size(), 1u);

  const std::string review = buildReview("r-1", t0 + 60000, "it", "", "save-1", 3, 5000);
  ASSERT_TRUE(appendOutbox(*fs, review));
  view.applyOp(review);
  EXPECT_EQ(view.entry(i).card.reps, 1);
  EXPECT_EQ(view.newDoneToday(t0 + 120000), 1);

  // A rebuild from the files gives the same card.
  LocalView again;
  again.load(*fs, "it");
  ASSERT_EQ(again.size(), 1u);
  EXPECT_EQ(again.entry(0).card.dueMs, view.entry(i).card.dueMs);
  ItemDetails d;
  ASSERT_TRUE(again.details(*fs, 0, d));
  EXPECT_EQ(d.lemma, "Cavallo");
  EXPECT_EQ(d.encounter.form, "cavalli");

  // Other languages in the outbox stay out of this view.
  in.language = "en";
  in.lemma = "horse";
  ASSERT_TRUE(appendOutbox(*fs, buildSave("save-2", t0, in)));
  again.load(*fs, "it");
  EXPECT_EQ(again.size(), 1u);
}

TEST_F(SdCard, LineReaderSkipsOverlongLines) {
  put("/x.jsonl", "a\n" + std::string(100, 'b') + "\nc\nd");
  auto f = fs->open("/x.jsonl", OpenMode::Read);
  LineReader r(*f, 10);
  std::string line;
  bool complete;
  ASSERT_TRUE(r.next(line, complete));
  EXPECT_EQ(line, "a");
  ASSERT_TRUE(r.next(line, complete));
  EXPECT_TRUE(r.overlong());
  ASSERT_TRUE(r.next(line, complete));
  EXPECT_EQ(line, "c");
  EXPECT_EQ(r.lineOffset(), 103u);
  ASSERT_TRUE(r.next(line, complete));
  EXPECT_EQ(line, "d");
  EXPECT_FALSE(complete);
  EXPECT_FALSE(r.next(line, complete));
}

// ---- per-language banks ----

TEST_F(SdCard, BankPathsAreOnePerLanguage) {
  EXPECT_EQ(bankPath("it"), "/.linguetta/bank/it.jsonl");
  EXPECT_EQ(bankPath("pt-BR"), "/.linguetta/bank/pt-BR.jsonl");
  EXPECT_EQ(bankPath(""), "");
  EXPECT_EQ(bankPath("../x"), "");
  put(bankPath("it"), readFixture("bank.jsonl"));
  put(bankPath("en"), "{\"t\":\"header\",\"language\":\"en\"}\n");
  put(bankPath("it") + ".tmp", "x");
  EXPECT_EQ(bankLanguages(*fs), (std::vector<std::string>{"en", "it"}));
}

TEST_F(SdCard, CursorsRoundTripPerLanguage) {
  SyncState st;
  st.token = "t";
  st.language = "en";
  st.cursors = {{"en", "c-en"}, {"it", "c-it"}};
  ASSERT_TRUE(saveState(*fs, st));
  SyncState again;
  ASSERT_TRUE(loadState(*fs, again));
  EXPECT_EQ(again.cursors, st.cursors);
}

TEST_F(SdCard, LegacyBankMovesToItsLanguageAndForgetsTheCursor) {
  put(paths::LEGACY_BANK, readFixture("bank.jsonl"));
  put(paths::LEGACY_BANK_TMP, "partial");
  SyncState st;
  st.language = "it";
  st.cursor = "c";
  st.cursors["it"] = "c";
  EXPECT_TRUE(migrateLegacyBank(*fs, st));
  EXPECT_FALSE(fs->exists(paths::LEGACY_BANK));
  EXPECT_FALSE(fs->exists(paths::LEGACY_BANK_TMP));
  std::string moved;
  ASSERT_TRUE(readFile(*fs, bankPath("it"), moved));
  EXPECT_EQ(moved, readFixture("bank.jsonl"));
  EXPECT_TRUE(st.cursors.empty());
  EXPECT_EQ(st.cursor, "");
  EXPECT_FALSE(migrateLegacyBank(*fs, st));

  // When the per-language file already exists, the old one is just dropped.
  put(paths::LEGACY_BANK, "{\"t\":\"header\",\"language\":\"it\"}\n");
  EXPECT_TRUE(migrateLegacyBank(*fs, st));
  ASSERT_TRUE(readFile(*fs, bankPath("it"), moved));
  EXPECT_EQ(moved, readFixture("bank.jsonl"));
  EXPECT_FALSE(fs->exists(paths::LEGACY_BANK));
}

// Fixture account {en, it} (pt native) where only Italian has items.
TEST_F(SdCard, TwoLanguageAccountHasOneDeckPerLanguage) {
  put(bankPath("it"), readFixture("bank.jsonl"));
  put(bankPath("en"), "{\"t\":\"header\",\"protocol\":1,\"language\":\"en\",\"mode\":\"full\"}\n");
  SyncState st;
  st.account.nativeLanguage = "pt";
  st.account.studyLanguages = {"en", "it"};
  SaveInput in;
  in.language = "de";
  in.lemma = "Hund";
  ASSERT_TRUE(appendOutbox(*fs, buildSave("save-de", at("2026-11-12T10:00:00Z"), in)));
  EXPECT_EQ(deckLanguages(*fs, st), (std::vector<std::string>{"en", "it", "de"}));

  LocalView en, it, de;
  en.load(*fs, "en");
  it.load(*fs, "it");
  de.load(*fs, "de");
  EXPECT_EQ(en.size(), 0u);  // hidden in the deck list
  EXPECT_EQ(it.size(), 6u);  // 5 items + 1 ficha
  EXPECT_EQ(de.size(), 1u);  // an offline save is a deck before any sync
  EXPECT_EQ(it.find("Hund"), -1);
  EXPECT_EQ(en.find("bello"), -1);
}

TEST_F(SdCard, BlendLanguagePicking) {
  SyncState st;
  st.account.nativeLanguage = "pt";
  st.account.studyLanguages = {"en", "it"};
  auto inIt = [](const std::string& l) { return l == "it"; };
  auto nowhere = [](const std::string&) { return false; };
  // the book's language, when it is a study language
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"it-IT"}, "", nowhere), "it");
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"EN"}, "", inIt), "en");
  // blend books are in the base language (pt): the deck that has the lemma
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt-BR"}, "", inIt), "it");
  // settings.json "language", then the first study language that is not native
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt"}, "it", nowhere), "it");
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt"}, "fr", nowhere), "en");
  st.account.studyLanguages = {"pt", "it"};
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt"}, "", nowhere), "it");
  st.account.studyLanguages.clear();
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{""}, "", nowhere), "it");
}

TEST_F(SdCard, BlendLanguageFromBookName) {
  SyncState st;
  st.account.nativeLanguage = "pt";
  st.account.studyLanguages = {"en", "it"};
  auto nowhere = [](const std::string&) { return false; };
  // The bug: a PT-IT blend book (dc:language pt) saved Italian words in English.
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt", "O Hobbit - blend PT-IT"}, "", nowhere), "it");
  EXPECT_EQ(blendLanguageFromName(st, "hobbit_pt_it.epub"), "it");
  EXPECT_EQ(blendLanguageFromName(st, "Livro PT-EN"), "en");
  EXPECT_EQ(blendLanguageFromName(st, "IT-PT"), "");            // target is the native language
  EXPECT_EQ(blendLanguageFromName(st, "PT-DE"), "");            // not a study language
  EXPECT_EQ(blendLanguageFromName(st, "Anna Kariênina"), "");   // no pair
  EXPECT_EQ(blendLanguageFromName(st, "Spider-Man"), "");       // words, not codes
  // A choice made on the device wins over the name; unknown choices are ignored.
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt", "PT-IT", "en"}, "", nowhere), "en");
  EXPECT_EQ(pickBlendLanguage(st, BlendBook{"pt", "PT-IT", "fr"}, "", nowhere), "it");
  EXPECT_EQ(blendLanguages(st), (std::vector<std::string>{"en", "it"}));
}

TEST_F(SdCard, BookLanguageChoicePersists) {
  EXPECT_EQ(loadBookLanguage(*fs, "abc"), "");
  EXPECT_TRUE(saveBookLanguage(*fs, "abc", "it"));
  EXPECT_TRUE(saveBookLanguage(*fs, "def", "en"));
  EXPECT_EQ(loadBookLanguage(*fs, "abc"), "it");
  EXPECT_EQ(loadBookLanguage(*fs, "def"), "en");
  EXPECT_FALSE(saveBookLanguage(*fs, "", "it"));
}
