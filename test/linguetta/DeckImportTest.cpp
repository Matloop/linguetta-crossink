// CSV/TSV decks of the SD going up as deckImport ops (contract section 4): the op shape,
// the content hash, splitting, and the once-per-file bookkeeping in imports.json.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "DeckImport.h"
#include "Decks.h"
#include "Ops.h"
#include "Sha256.h"
#include "StdFs.h"
#include "Store.h"
#include "Sync.h"
#include "TestUtil.h"
#include "Text.h"

using namespace linguetta;

namespace {

class DeckImportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/linguetta-import-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir = tmpl;
    fs = std::make_unique<StdFs>(dir);
    fs->mkdirs(paths::ROOT);
    fs->mkdirs(paths::TSV_DIR);
  }
  void TearDown() override { std::system(("rm -rf '" + dir + "'").c_str()); }

  void put(const std::string& path, const std::string& content) {
    std::ofstream out(fs->real(path), std::ios::binary);
    out << content;
  }
  std::vector<std::string> outbox() {
    std::vector<std::string> out;
    forEachOutbox(*fs, [&](const OutboxLine& l) {
      out.push_back(l.json);
      return true;
    });
    return out;
  }
  std::function<std::string()> ids() {
    return [this] { return "imp-" + std::to_string(++next); };
  }

  std::string dir;
  std::unique_ptr<StdFs> fs;
  int next = 0;
  const int64_t NOW = 1794432600000;  // 2026-11-11T21:30:00Z
};

}  // namespace

TEST(Sha256, KnownVectors) {
  Sha256 a;
  EXPECT_EQ(a.hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  Sha256 b;
  b.update("abc");
  EXPECT_EQ(b.hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // Across a block boundary, fed in pieces.
  Sha256 c;
  const std::string msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  c.update(msg.substr(0, 10));
  c.update(msg.substr(10));
  EXPECT_EQ(c.hex(), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(DeckImportOps, BuildsTheOpOfTheContract) {
  const std::string line = buildDeckImport("c1", 1794432600000, "it", "Italiano::Básico", "sha256:ab",
                                           {{"ciao", "oi"}, {"", "sem frente"}, {"grazie", ""}});
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, line), DeserializationError::Ok);
  EXPECT_STREQ(doc["type"], "deckImport");
  EXPECT_STREQ(doc["clientId"], "c1");
  EXPECT_STREQ(doc["language"], "it");
  EXPECT_STREQ(doc["at"], "2026-11-11T21:30:00.000Z");
  EXPECT_STREQ(doc["name"], "Italiano Básico");  // "::" would make a subdeck
  EXPECT_STREQ(doc["hash"], "sha256:ab");
  ASSERT_EQ(doc["rows"].size(), 2u);  // the row without front is dropped
  EXPECT_STREQ(doc["rows"][0]["front"], "ciao");
  EXPECT_STREQ(doc["rows"][0]["back"], "oi");
  EXPECT_STREQ(doc["rows"][1]["front"], "grazie");

  EXPECT_EQ(buildDeckImport("c2", 0, "it", "x", "h", {{"", "a"}}), "");
  std::vector<DeckRow> many(501, DeckRow{"a", "b"});
  EXPECT_EQ(buildDeckImport("c3", 0, "it", "x", "h", many), "");
  EXPECT_EQ(deckImportName("  "), "Deck");
  EXPECT_EQ(codePointCount(deckImportName(std::string(120, 'a'))), 80u);
}

TEST(DeckImportOps, ExtraNew) {
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, buildExtraNew("x", 1794432600000, "it", 5)), DeserializationError::Ok);
  EXPECT_STREQ(doc["type"], "extraNew");
  EXPECT_EQ(doc["n"].as<int>(), 5);
  deserializeJson(doc, buildExtraNew("x", 0, "it", 5000));
  EXPECT_EQ(doc["n"].as<int>(), 1000);
}

TEST_F(DeckImportTest, HashIgnoresLineEndingsAndBom) {
  put("/flashcards/a.tsv", "ciao\toi\ngrazie\tobrigado\n");
  put("/flashcards/b.tsv", "\xEF\xBB\xBF" "ciao\toi\r\ngrazie\tobrigado\r\nciao\tdup\r\n");
  std::vector<DeckRow> a, b;
  ASSERT_TRUE(deckimport::readRows(*fs, "a", a));
  ASSERT_TRUE(deckimport::readRows(*fs, "b", b));
  EXPECT_EQ(b.size(), 2u);  // duplicate front dropped, as the local deck does
  EXPECT_EQ(deckimport::rowsHash(a), deckimport::rowsHash(b));
  EXPECT_EQ(deckimport::rowsHash(a).rfind("sha256:", 0), 0u);
  EXPECT_EQ(deckimport::rowsHash(a).size(), 7u + 64u);
}

TEST_F(DeckImportTest, BigDecksSplitIntoNumberedParts) {
  std::vector<DeckRow> rows;
  for (int i = 0; i < 1200; i++) rows.push_back({"w" + std::to_string(i), "b"});
  const auto parts = deckimport::split("Parole", rows);
  ASSERT_GE(parts.size(), 3u);
  size_t total = 0;
  for (const auto& p : parts) {
    EXPECT_LE(p.rows.size(), 500u);
    total += p.rows.size();
  }
  EXPECT_EQ(total, rows.size());
  EXPECT_EQ(parts[0].rows[0].front, "w0");
  EXPECT_EQ(parts[1].name, "Parole 2");
  const std::string h = deckimport::rowsHash(rows);
  EXPECT_EQ(parts[2].hash, h + ".3");

  // Long rows split by bytes so each op fits the 16 KB outbox line.
  std::vector<DeckRow> fat(40, DeckRow{std::string(500, 'x'), std::string(500, 'y')});
  const auto byBytes = deckimport::split("Lunghe", fat);
  EXPECT_GT(byBytes.size(), 1u);
  for (const auto& p : byBytes) {
    EXPECT_LT(buildDeckImport("c", 0, "it", p.name, p.hash, p.rows).size(), 16000u);
  }
  EXPECT_EQ(deckimport::split("Uno", {{"a", "b"}})[0].name, "Uno");
}

TEST_F(DeckImportTest, EachFileIsQueuedOnceAndHiddenWhenImported) {
  put("/flashcards/Viaggio.csv", "front,back\nciao,oi\ngrazie,obrigado\n");
  put("/flashcards/vuoto.tsv", "");
  EXPECT_EQ(deckimport::queue(*fs, "it", NOW, ids()), 1);
  auto ops = outbox();
  ASSERT_EQ(ops.size(), 1u);
  JsonDocument doc;
  deserializeJson(doc, ops[0]);
  EXPECT_STREQ(doc["type"], "deckImport");
  EXPECT_STREQ(doc["name"], "Viaggio");
  EXPECT_EQ(doc["rows"].size(), 2u);
  EXPECT_EQ(countOutbox(*fs).other, 1);

  // Queued: not again, not even before the sync settles it.
  EXPECT_EQ(deckimport::queue(*fs, "it", NOW, ids()), 0);
  EXPECT_EQ(outbox().size(), 1u);
  EXPECT_TRUE(deckimport::imported(*fs).empty());

  // A result for another op changes nothing; the applied result imports the file.
  std::map<std::string, DeckImportResult> results;
  results["other"] = DeckImportResult{true, "d0", true, 1};
  EXPECT_EQ(deckimport::settle(*fs, results), 0);
  results["imp-1"] = DeckImportResult{true, "deck-9", true, 2};
  EXPECT_EQ(deckimport::settle(*fs, results), 1);
  EXPECT_EQ(deckimport::imported(*fs), (std::set<std::string>{"Viaggio"}));
  EXPECT_EQ(deckimport::load(*fs)["Viaggio"].deckIds, (std::vector<std::string>{"deck-9"}));

  // The listing still has the file; the reader hides it. A new file goes up on its own.
  put("/flashcards/Cibo.tsv", "pane\tpão\n");
  EXPECT_EQ(deckimport::queue(*fs, "it", NOW, ids()), 1);
  EXPECT_EQ(outbox().size(), 2u);
}

TEST_F(DeckImportTest, RejectedPartMarksTheFileRejected) {
  std::string big;
  for (int i = 0; i < 600; i++) big += "w" + std::to_string(i) + "\tb\n";
  put("/flashcards/Grande.tsv", big);
  ASSERT_EQ(deckimport::queue(*fs, "it", NOW, ids()), 1);
  ASSERT_EQ(outbox().size(), 2u);
  std::map<std::string, DeckImportResult> results;
  results["imp-1"] = DeckImportResult{true, "d1", true, 500};
  EXPECT_EQ(deckimport::settle(*fs, results), 0);  // the second part has no result yet
  EXPECT_EQ(deckimport::load(*fs)["Grande"].state, deckimport::State::Queued);
  results["imp-2"] = DeckImportResult{false, "", false, 0};
  EXPECT_EQ(deckimport::settle(*fs, results), 0);
  EXPECT_EQ(deckimport::load(*fs)["Grande"].state, deckimport::State::Rejected);
  EXPECT_TRUE(deckimport::imported(*fs).empty());
  EXPECT_EQ(deckimport::queue(*fs, "it", NOW, ids()), 0);  // not sent again
}

TEST_F(DeckImportTest, NoLanguageOrIdMakerQueuesNothing) {
  put("/flashcards/a.tsv", "ciao\toi\n");
  EXPECT_EQ(deckimport::queue(*fs, "", NOW, ids()), 0);
  EXPECT_EQ(deckimport::queue(*fs, "it", NOW, nullptr), 0);
  EXPECT_TRUE(outbox().empty());
}
