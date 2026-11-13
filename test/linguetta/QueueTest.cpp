// Server queue on the device (contract section 8 "Visão local" and the queue rules): header
// and ficha parsing, due first, new cards by queueRank up to the local allowance, decks with
// their subdecks, sibling bury, teimosas (leechWaiting and local lapses) and desiredRetention.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "LocalView.h"
#include "Ops.h"
#include "Session.h"
#include "Sources.h"
#include "StdFs.h"
#include "Store.h"
#include "TestUtil.h"
#include "Text.h"

using namespace linguetta;

namespace {

constexpr const char* DECK_A = "aaaaaaaa-0000-4000-8000-000000000001";  // "Minhas palavras" (default)
constexpr const char* DECK_B = "aaaaaaaa-0000-4000-8000-000000000002";  // "Gramática"
constexpr const char* DECK_C = "aaaaaaaa-0000-4000-8000-000000000003";  // "Verbi", child of B
constexpr const char* DECK_X = "aaaaaaaa-0000-4000-8000-0000000000ff";  // not on the reader

std::string header(int allowance, int extraNew = 0, double retention = 0.9, int leechLapses = 8) {
  return std::string(R"({"t":"header","protocol":1,"language":"it","mode":"full","cursor":"c1",)") +
         R"("serverNow":"2026-11-11T10:00:00.000Z","deckName":"Linguetta · Italiano","total":9,"fichasTotal":2,)" +
         R"("settings":{"newFlashcardsPerDay":10,"utcOffsetMinutes":-180,"dayStartHour":4,"newOrder":"importance",)" +
         R"("desiredRetention":)" + std::to_string(retention) + R"(,"buryRelated":true,"leech":{"lapses":)" +
         std::to_string(leechLapses) + R"(,"action":"wait"}},)" +
         R"("today":{"dayStart":"2026-11-11T07:00:00.000Z","nextDayStart":"2026-11-12T07:00:00.000Z","newDoneToday":)" +
         std::to_string(10 + extraNew - allowance) + R"(,"extraNew":)" + std::to_string(extraNew) +
         R"(,"newAllowance":)" + std::to_string(allowance) + "}," +
         R"("decks":[{"id":")" + DECK_A + R"(","name":"Minhas palavras","default":true,"parentId":null},)" +
         R"({"id":")" + DECK_B + R"(","name":"Gramática","default":false,"parentId":null},)" +
         R"({"id":")" + DECK_C + R"(","name":"Verbi","default":false,"parentId":")" + DECK_B + R"("}],)" +
         R"("fsrs":{"version":"FSRS-7","desiredRetention":)" + std::to_string(retention) + "}}";
}

std::string card(const std::string& id, int state, const char* due, int lapses = 0) {
  const bool fresh = state == 0;
  return std::string(R"({"id":")") + id + R"(","createdAt":"2026-11-01T10:00:00.000Z","dueAt":")" + due +
         R"(","stability":)" + (fresh ? "0" : "5.5") + R"(,"stabilityFast":)" + (fresh ? "0" : "5.5") +
         R"(,"difficulty":)" + (fresh ? "0" : "5") + R"(,"state":)" + std::to_string(state) +
         R"(,"scheduleStatus":")" + (fresh ? "new" : "review") + R"(","lastReviewAt":)" +
         (fresh ? "null" : R"("2026-11-05T10:00:00.000Z")") + R"(,"reps":)" + (fresh ? "0" : "3") +
         R"(,"lapses":)" + std::to_string(lapses) + R"(,"scheduledDays":)" + (fresh ? "0" : "5") +
         R"(,"learningStep":0})";
}

struct ItemOpts {
  const char* deck = DECK_A;
  int rank = 0;  // 0: null
  int state = 0;
  const char* due = "2026-11-11T10:00:00.000Z";
  const char* status = "ativo";
  bool leech = false;
  const char* sibling = nullptr;
  int lapses = 0;
};

std::string item(const std::string& lemma, const ItemOpts& o) {
  return std::string(R"({"t":"item","id":"item-)") + lemma + R"(","lemma":")" + lemma + R"(","lemmaKey":")" + lemma +
         R"(","language":"it","sense":"s-)" + lemma + R"(","status":")" + o.status + R"(","deckId":")" + o.deck +
         R"(","queueRank":)" + (o.rank ? std::to_string(o.rank) : "null") + R"(,"leechWaiting":)" +
         (o.leech ? "true" : "false") +
         (o.sibling ? std::string(R"(,"siblingReviewedAt":")") + o.sibling + "\"" : std::string()) +
         R"(,"seenCount":1,"createdAt":"2026-11-01T10:00:00.000Z","flashcard":)" +
         card("card-" + lemma, o.state, o.due, o.lapses) + "}";
}

std::string ficha(const std::string& name, const char* kind, const char* front, const char* back, int rank,
                  const char* deck = DECK_B) {
  return std::string(R"({"t":"ficha","id":"ficha-)") + name + R"(","language":"it","kind":")" + kind +
         R"(","front":")" + front + R"(","back":")" + back + R"(","status":"ativo","deckId":")" + deck +
         R"(","queueRank":)" + (rank ? std::to_string(rank) : "null") +
         R"(,"leechWaiting":false,"createdAt":"2026-11-01T10:00:00.000Z","flashcard":)" +
         card("card-f-" + name, 0, "2026-11-11T10:00:00.000Z") + "}";
}

class QueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/linguetta-queue-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir = tmpl;
    fs = std::make_unique<StdFs>(dir);
    fs->mkdirs(paths::BANK_DIR);
  }
  void TearDown() override { std::system(("rm -rf '" + dir + "'").c_str()); }

  static int64_t at(const char* iso) {
    int64_t ms = 0;
    parseIso(iso, ms);
    return ms;
  }
  void writeBank(const std::string& hdr, const std::vector<std::string>& lines) {
    std::ofstream out(fs->real(bankPath("it")), std::ios::binary);
    out << hdr << "\n";
    for (const auto& l : lines) out << l << "\n";
    out << R"({"t":"end","count":)" << lines.size() << "}\n";
  }
  // Lemma (or ficha front) of each queue entry, in order.
  std::vector<std::string> names(const LocalView& v, const std::vector<int>& q) {
    std::vector<std::string> out;
    for (int i : q) {
      ItemDetails d;
      v.details(*fs, i, d);
      out.push_back(d.lemma);
    }
    return out;
  }
  // A small bank: two due, one teimosa, one buried, one suspended, one outside the reader,
  // new cards ranked across decks, one new not starting today and two fichas.
  std::vector<std::string> standard() {
    ItemOpts dueA{DECK_A, 0, 2, "2026-11-11T08:00:00.000Z"};
    ItemOpts dueC{DECK_C, 0, 2, "2026-11-10T08:00:00.000Z"};
    ItemOpts leech{DECK_A, 0, 2, "2026-11-10T08:00:00.000Z"};
    leech.leech = true;
    leech.lapses = 8;
    ItemOpts buried{DECK_A, 0, 2, "2026-11-10T09:00:00.000Z"};
    buried.sibling = "2026-11-11T08:30:00.000Z";
    ItemOpts suspended{DECK_A, 0, 2, "2026-11-10T09:00:00.000Z"};
    suspended.status = "suspenso";
    ItemOpts off{DECK_X, 0, 2, "2026-11-10T09:00:00.000Z"};
    return {
        item("antico", dueA),
        item("correre", dueC),
        item("ostinato", leech),
        item("sepolto", buried),
        item("sospeso", suspended),
        item("fuori", off),
        item("nuovo3", {DECK_A, 3}),
        item("nuovo1", {DECK_C, 1}),
        item("domani", {DECK_A, 0}),
        ficha("f2", "frente", "Qual o plural de uomo?", "uomini", 2),
        ficha("f5", "lacuna", "Io [...] italiano.", "Io parlo italiano. — falar", 5, DECK_A),
    };
  }

  std::string dir;
  std::unique_ptr<StdFs> fs;
  const int64_t NOW = at("2026-11-11T12:00:00.000Z");
};

}  // namespace

TEST_F(QueueTest, HeaderCarriesDecksAllowanceRetentionAndLeech) {
  writeBank(header(4, 2, 0.85, 6), standard());
  LocalView v;
  v.load(*fs, "it");
  const BankHeader& h = v.header();
  EXPECT_TRUE(h.serverQueue());
  EXPECT_EQ(h.newPerDay, 10);
  EXPECT_EQ(h.newAllowance, 4);
  EXPECT_EQ(h.extraNew, 2);
  EXPECT_NEAR(h.desiredRetention, 0.85, 1e-6);
  EXPECT_TRUE(h.buryRelated);
  EXPECT_EQ(h.leechLapses, 6);
  EXPECT_TRUE(h.leechLeavesQueue());
  EXPECT_EQ(h.fichasTotal, 2);
  ASSERT_EQ(h.decks.size(), 3u);
  EXPECT_EQ(h.decks[0].name, "Minhas palavras");
  EXPECT_TRUE(h.decks[0].isDefault);
  EXPECT_EQ(h.defaultDeck, 0);
  EXPECT_EQ(h.decks[2].parent, 1);
  EXPECT_EQ(v.newLimit(NOW), 12);
}

TEST_F(QueueTest, FichaLinesAreCardsOutsideTheLemmaIndex) {
  writeBank(header(9), standard());
  LocalView v;
  v.load(*fs, "it");
  ASSERT_EQ(v.size(), 11u);
  int f = -1;
  for (size_t i = 0; i < v.size(); i++) {
    if (v.entry(i).cardId == "card-f-f5") f = static_cast<int>(i);
  }
  ASSERT_GE(f, 0);
  EXPECT_TRUE(v.entry(f).ficha);
  EXPECT_TRUE(v.entry(f).key.empty());
  EXPECT_EQ(v.entry(f).queueRank, 5);
  ItemDetails d;
  ASSERT_TRUE(v.details(*fs, f, d));
  EXPECT_TRUE(d.ficha);
  EXPECT_TRUE(d.cloze);
  EXPECT_EQ(d.lemma, "Io [...] italiano.");
  EXPECT_EQ(d.sense, "Io parlo italiano. — falar");
  // Word Lens and saves only see items.
  EXPECT_EQ(v.find("Io [...] italiano."), -1);
  EXPECT_GE(v.find("antico"), 0);
}

TEST_F(QueueTest, DueFirstThenNewByRankUpToTheAllowance) {
  writeBank(header(9), standard());
  LocalView v;
  v.load(*fs, "it");
  // Due by due date; then ranks 1, 2, 3, 5. Teimosa, buried, suspended, off-reader and the
  // unranked new card stay out.
  EXPECT_EQ(names(v, v.queue(NOW)), (std::vector<std::string>{"correre", "antico", "nuovo1",
                                                              "Qual o plural de uomo?", "nuovo3",
                                                              "Io [...] italiano."}));
  const LocalView::Counts c = v.counts(NOW);
  EXPECT_EQ(c.due, 2);
  EXPECT_EQ(c.fresh, 4);
  EXPECT_EQ(v.newBeyondBudget(NOW), 0);
}

TEST_F(QueueTest, AllowanceCutsTheRankedNewAndExtraNewRaisesIt) {
  writeBank(header(2), standard());
  LocalView v;
  v.load(*fs, "it");
  EXPECT_EQ(v.counts(NOW).fresh, 2);
  EXPECT_EQ(v.newBeyondBudget(NOW), 2);

  // A new card reviewed offline spends the budget.
  const int n1 = v.find("nuovo1");
  const std::string r = buildReview("r1", NOW, "it", v.entry(n1).cardId, "", 3, 4000);
  ASSERT_TRUE(appendOutbox(*fs, r));
  v.applyOp(r);
  EXPECT_EQ(v.newAllowance(NOW + 1000), 1);
  EXPECT_EQ(names(v, v.queue(NOW + 1000)).back(), "Qual o plural de uomo?");

  // "+5 hoje" is an outbox op that counts at once.
  const std::string x = buildExtraNew("x1", NOW + 2000, "it", 5);
  ASSERT_TRUE(appendOutbox(*fs, x));
  v.applyOp(x);
  EXPECT_EQ(v.newAllowance(NOW + 3000), 6);
  EXPECT_EQ(v.newLimit(NOW + 3000), 15);
  EXPECT_EQ(v.newBeyondBudget(NOW + 3000), 0);

  // Rebuilt from the files it is the same.
  LocalView again;
  again.load(*fs, "it");
  EXPECT_EQ(again.newAllowance(NOW + 3000), 6);

  // After nextDayStart the day resets to newFlashcardsPerDay (and the extras were yesterday's).
  EXPECT_EQ(again.newAllowance(at("2026-11-12T08:00:00.000Z")), 10);
}

TEST_F(QueueTest, OfflineSaveComesAfterTheRankedNewCards) {
  writeBank(header(9), standard());
  LocalView v;
  v.load(*fs, "it");
  SaveInput in;
  in.language = "it";
  in.lemma = "salvato";
  in.sentence = "Ho salvato la parola.";
  const std::string save = buildSave("s1", NOW - 60000, in);
  ASSERT_TRUE(appendOutbox(*fs, save));
  v.applyOp(save);
  const auto q = names(v, v.queue(NOW));
  ASSERT_EQ(q.size(), 7u);
  EXPECT_EQ(q.back(), "salvato");
  // Saved into the default deck.
  EXPECT_EQ(v.entry(v.find("salvato")).deck, 0);
}

TEST_F(QueueTest, DeckSessionIncludesSubdecks) {
  writeBank(header(9), standard());
  LocalView v;
  v.load(*fs, "it");
  // Gramática (B) holds the ficha f2 and, through Verbi (C), correre and nuovo1.
  EXPECT_EQ(names(v, v.queue(NOW, 1)),
            (std::vector<std::string>{"correre", "nuovo1", "Qual o plural de uomo?"}));
  EXPECT_EQ(names(v, v.queue(NOW, 2)), (std::vector<std::string>{"correre", "nuovo1"}));
  EXPECT_EQ(v.cardCount(1), 3);
  // Minhas palavras: suspended and off-reader cards do not count; the teimosa and the buried
  // card do (they are in the deck, just not today).
  EXPECT_EQ(v.cardCount(0), 6);
  // A card outside the reader decks is in the bank but never in a queue.
  const int off = v.find("fuori");
  EXPECT_EQ(v.entry(off).deck, DECK_OFF);
}

TEST_F(QueueTest, BankWithoutDecksKeepsTheOldBehaviour) {
  // A header from before the server queue: no decks, no newAllowance.
  const std::string old =
      R"({"t":"header","protocol":1,"language":"it","mode":"full","cursor":"c0","deckName":"Linguetta · Italiano",)"
      R"("total":2,"settings":{"newCardsPerDay":1,"utcOffsetMinutes":-180,"dayStartHour":4},)"
      R"("today":{"dayStart":"2026-11-11T07:00:00.000Z","nextDayStart":"2026-11-12T07:00:00.000Z","newDoneToday":0},)"
      R"("fsrs":{"version":"FSRS-7","desiredRetention":0.9}})";
  std::ofstream out(fs->real(bankPath("it")), std::ios::binary);
  out << old << "\n"
      << R"({"t":"item","id":"i1","lemma":"uno","status":"ativo","createdAt":"2026-11-01T10:00:00.000Z","flashcard":)"
      << card("c1", 0, "2026-11-01T10:00:00.000Z") << "}\n"
      << R"({"t":"item","id":"i2","lemma":"due","status":"ativo","createdAt":"2026-11-02T10:00:00.000Z","flashcard":)"
      << card("c2", 0, "2026-11-02T10:00:00.000Z") << "}\n"
      << R"({"t":"end","count":2})" << "\n";
  out.close();
  LocalView v;
  v.load(*fs, "it");
  EXPECT_FALSE(v.header().serverQueue());
  EXPECT_EQ(v.header().newPerDay, 1);
  EXPECT_EQ(v.entry(0).deck, DECK_NONE);
  EXPECT_EQ(names(v, v.queue(NOW)), (std::vector<std::string>{"uno"}));
}

TEST_F(QueueTest, ReviewingTheSiblingTodayBuriesTheCard) {
  ItemOpts due{DECK_A, 0, 2, "2026-11-10T08:00:00.000Z"};
  due.sibling = "2026-11-10T20:00:00.000Z";  // yesterday: no bury
  ItemOpts fresh{DECK_A, 1};
  fresh.sibling = "2026-11-11T07:30:00.000Z";  // today: the new card waits
  ItemOpts learning{DECK_A, 0, 1, "2026-11-11T11:00:00.000Z"};
  learning.sibling = "2026-11-11T07:30:00.000Z";  // learning steps never wait
  writeBank(header(9), {item("ieri", due), item("oggi", fresh), item("passo", learning)});
  LocalView v;
  v.load(*fs, "it");
  EXPECT_EQ(names(v, v.queue(NOW)), (std::vector<std::string>{"ieri", "passo"}));
}

TEST_F(QueueTest, LapsesReachingTheLimitMakeATeimosaUntilASaveWithSentence) {
  ItemOpts hard{DECK_A, 0, 2, "2026-11-10T08:00:00.000Z"};
  hard.lapses = 2;
  writeBank(header(9, 0, 0.9, 3), {item("difficile", hard)});
  LocalView v;
  v.load(*fs, "it");
  const int i = v.find("difficile");
  ASSERT_FALSE(v.entry(i).leechWaiting);

  ViewSource src(v, *fs, [&](const std::string& line) {
    v.applyOp(line);
    return appendOutbox(*fs, line);
  }, [] { return std::string("op-1"); });
  ReviewSession session;
  session.start(src, "it", v.queue(NOW), NOW);
  ASSERT_FALSE(session.done());
  ASSERT_TRUE(session.answer(1, NOW + 5000));  // third lapse
  EXPECT_EQ(session.lastLeech(), i);
  EXPECT_TRUE(session.done());  // an "Errei" teimosa is not repeated
  ASSERT_EQ(session.leeches().size(), 1u);
  EXPECT_TRUE(v.entry(i).leechWaiting);
  EXPECT_EQ(v.entry(i).card.lapses, 3);
  EXPECT_TRUE(v.queue(NOW + 10000).empty());

  // Saving the word again from a sentence brings it back.
  SaveInput in;
  in.language = "it";
  in.lemma = "difficile";
  in.sentence = "È un esercizio difficile.";
  const std::string save = buildSave("s1", NOW + 60000, in);
  v.applyOp(save);
  EXPECT_FALSE(v.entry(i).leechWaiting);
}

TEST_F(QueueTest, ServerTeimosaLeavesTheQueue) {
  writeBank(header(9), standard());
  LocalView v;
  v.load(*fs, "it");
  const int i = v.find("ostinato");
  EXPECT_TRUE(v.entry(i).leechWaiting);
  for (int q : v.queue(NOW)) EXPECT_NE(q, i);
}

TEST_F(QueueTest, DesiredRetentionChangesTheIntervals) {
  ItemOpts due{DECK_A, 0, 2, "2026-11-10T08:00:00.000Z"};
  writeBank(header(9, 0, 0.8), {item("ritenere", due)});
  LocalView low;
  low.load(*fs, "it");
  writeBank(header(9, 0, 0.95), {item("ritenere", due)});
  LocalView high;
  high.load(*fs, "it");
  const CardState a = fsrs::review(low.entry(0).card, "seed", 3, NOW, low.header().desiredRetention);
  const CardState b = fsrs::review(high.entry(0).card, "seed", 3, NOW, high.header().desiredRetention);
  EXPECT_GT(a.dueMs, b.dueMs);
}

// A review card is due for the whole study day it lands on (as in Anki); learning and
// relearning steps are minutes and wait for the instant.
TEST_F(QueueTest, ReviewIsDueForTheStudyDayLearningAtTheInstant) {
  writeBank(header(0), {
                           item("stamattina", {DECK_A, 0, 2, "2026-11-11T08:00:00.000Z"}),
                           item("stasera", {DECK_A, 0, 2, "2026-11-11T22:00:00.000Z"}),
                           item("domani", {DECK_A, 0, 2, "2026-11-12T07:00:00.000Z"}),
                           item("imparo", {DECK_A, 0, 1, "2026-11-11T12:10:00.000Z"}),
                           item("ricado", {DECK_A, 0, 3, "2026-11-11T12:10:00.000Z"}),
                       });
  LocalView v;
  v.load(*fs, "it");
  EXPECT_EQ(v.nextDayStart(NOW), at("2026-11-12T07:00:00.000Z"));
  EXPECT_EQ(names(v, v.queue(NOW)), (std::vector<std::string>{"stamattina", "stasera"}));
  EXPECT_EQ(v.counts(NOW).due, 2);

  EXPECT_EQ(names(v, v.queue(at("2026-11-11T12:10:00.000Z"))),
            (std::vector<std::string>{"stamattina", "imparo", "ricado", "stasera"}));

  // No sync since: the next study day starts at nextDayStart and steps by 24 h.
  const int64_t tomorrow = at("2026-11-12T08:00:00.000Z");
  EXPECT_EQ(v.nextDayStart(tomorrow), at("2026-11-13T07:00:00.000Z"));
  EXPECT_EQ(names(v, v.queue(tomorrow)),
            (std::vector<std::string>{"stamattina", "imparo", "ricado", "stasera", "domani"}));
}
