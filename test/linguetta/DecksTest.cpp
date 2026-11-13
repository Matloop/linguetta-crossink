// Local TSV decks, review sessions (both deck kinds) and the local history.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "Decks.h"
#include "History.h"
#include "LocalView.h"
#include "Session.h"
#include "Sources.h"
#include "StdFs.h"
#include "Store.h"
#include "TestUtil.h"
#include "Text.h"

using namespace linguetta;

namespace {

constexpr int64_t MIN = 60 * 1000;

class Sd : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/linguetta-decks-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir = tmpl;
    fs = std::make_unique<StdFs>(dir);
    fs->mkdirs(paths::ROOT);
    fs->mkdirs(paths::BANK_DIR);
    fs->mkdirs(paths::TSV_DIR);
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
  static int64_t at(const char* iso) {
    int64_t ms = 0;
    EXPECT_TRUE(parseIso(iso, ms));
    return ms;
  }
  std::string dir;
  std::unique_ptr<StdFs> fs;
};

}  // namespace

TEST(Tsv, ParsesLines) {
  std::string f, b;
  EXPECT_TRUE(parseTsvLine("\xEF\xBB\xBFgatto\tgato\r", f, b));
  EXPECT_EQ(f, "gatto");
  EXPECT_EQ(b, "gato");
  EXPECT_TRUE(parseTsvLine(" cane \t cachorro \textra", f, b));
  EXPECT_EQ(f, "cane");
  EXPECT_EQ(b, "cachorro");
  EXPECT_TRUE(parseTsvLine("solo\t", f, b));
  EXPECT_EQ(b, "");
  EXPECT_FALSE(parseTsvLine("", f, b));
  EXPECT_FALSE(parseTsvLine("# comment\tx", f, b));
  EXPECT_FALSE(parseTsvLine("no tab here", f, b));
  EXPECT_FALSE(parseTsvLine("\tback only", f, b));
}

TEST_F(Sd, ListsTsvDecksSorted) {
  put("/flashcards/b deck.tsv", "a\tb\n");
  put("/flashcards/Alfa.TSV", "a\tb\n");
  put("/flashcards/notes.txt", "x");
  put("/flashcards/._hidden.tsv", "x");
  const auto decks = listTsvDecks(*fs);
  ASSERT_EQ(decks.size(), 2u);
  EXPECT_EQ(decks[0], "Alfa");
  EXPECT_EQ(decks[1], "b deck");
}

TEST_F(Sd, TsvDeckQueueGradeAndReload) {
  std::string tsv;
  for (int i = 0; i < 30; i++) tsv += "w" + std::to_string(i) + "\tp" + std::to_string(i) + "\n";
  tsv += "w0\tduplicate\n";
  put("/flashcards/It.tsv", tsv);

  const int64_t day = at("2026-11-11T03:00:00Z");
  const int64_t now = at("2026-11-11T12:00:00Z");
  TsvDeck deck;
  ASSERT_TRUE(deck.load(*fs, "It"));
  EXPECT_EQ(deck.size(), 30u);
  EXPECT_EQ(deck.card(0).back, "p0");
  EXPECT_EQ(deck.seed(3), "tsv:It:w3");

  // The account's newFlashcardsPerDay (from the bank header) caps the new cards.
  auto q = deck.queue(now, day, 20);
  ASSERT_EQ(q.size(), 20u);
  EXPECT_EQ(q[0], 0);  // new cards in file order

  ASSERT_TRUE(deck.grade(*fs, 0, 1, now));  // Errei on a new card: learning, due in 1 min
  ASSERT_TRUE(deck.grade(*fs, 1, 4, now));  // Easy: review, days away
  EXPECT_EQ(deck.newDoneToday(day), 2);
  EXPECT_EQ(deck.card(0).card.state, 1);
  EXPECT_GT(deck.card(1).card.dueMs, now + fsrs::MS_PER_DAY);

  // Same FSRS as the Linguetta deck, seeded by "tsv:<deck>:<front>".
  CardState expect = fsrs::review(fsrs::newCard(now), "tsv:It:w1", 4, now);
  EXPECT_EQ(deck.card(1).card.dueMs, expect.dueMs);

  // 11 minutes later: w0 is due; 18 new left for today.
  q = deck.queue(now + 11 * MIN, day, 20);
  ASSERT_EQ(q.size(), 1u + 18u);
  EXPECT_EQ(q[0], 0);

  // Reload replays the log.
  TsvDeck again;
  ASSERT_TRUE(again.load(*fs, "It"));
  EXPECT_EQ(again.card(1).card.dueMs, deck.card(1).card.dueMs);
  EXPECT_EQ(again.card(0).card.state, deck.card(0).card.state);
  EXPECT_EQ(again.newDoneToday(day), 2);
  // Next day: a fresh allowance.
  EXPECT_EQ(again.newDoneToday(day + fsrs::MS_PER_DAY), 0);

  // A torn last log line is skipped, and the next append starts on a fresh line.
  const std::string log = TsvDeck::logPath("It");
  put(log, get(log) + "{\"f\":\"w2\",\"g\"");
  TsvDeck torn;
  ASSERT_TRUE(torn.load(*fs, "It"));
  EXPECT_EQ(torn.card(2).firstReviewMs, -1);
  ASSERT_TRUE(torn.grade(*fs, 2, 1, now + MIN));
  TsvDeck fixed;
  ASSERT_TRUE(fixed.load(*fs, "It"));
  EXPECT_EQ(fixed.card(2).firstReviewMs, now + MIN);
}

TEST_F(Sd, MissingTsvFails) {
  TsvDeck deck;
  EXPECT_FALSE(deck.load(*fs, "nope"));
}

TEST_F(Sd, SessionRepeatsLearningCardsAndSummarises) {
  put("/flashcards/D.tsv", "a\tA\nb\tB\nc\tC\n");
  TsvDeck deck;
  ASSERT_TRUE(deck.load(*fs, "D"));
  TsvSource src(deck, *fs);
  const int64_t t0 = at("2026-11-11T12:00:00Z");
  ReviewSession s;
  s.start(src, "D", deck.queue(t0, t0 - MIN, 10), t0);
  EXPECT_EQ(s.total(), 3);
  EXPECT_EQ(s.position(), 1);
  EXPECT_EQ(s.current(), 0);

  s.shown(t0);
  ASSERT_TRUE(s.answer(1, t0 + 5000));  // a: Errei -> back in 1 min, repeated in-session
  EXPECT_EQ(s.position(), 2);
  ASSERT_TRUE(s.answer(4, t0 + 9000));   // b: Fácil -> days away
  ASSERT_TRUE(s.answer(3, t0 + 12000));  // c: Bom on a new card -> 10-minute step, repeated
  EXPECT_FALSE(s.done());
  EXPECT_EQ(s.current(), 0);  // the repeat of a
  EXPECT_EQ(s.position(), 3);
  ASSERT_TRUE(s.answer(1, t0 + 70000));  // a again: Errei -> 1 min, to the end
  EXPECT_EQ(s.current(), 2);  // c's step
  // Bom on whatever comes up until both graduate (Bom on the last step -> days away).
  int64_t t = t0 + 90000;
  int repeats = 0;
  while (!s.done() && repeats < 10) {
    ASSERT_TRUE(s.answer(3, t));
    t += 11 * MIN;
    repeats++;
  }
  ASSERT_TRUE(s.done());

  EXPECT_EQ(s.cards(), 3);
  EXPECT_EQ(s.answers(), 4 + repeats);
  EXPECT_EQ(s.firstGrades(1), 1);
  EXPECT_EQ(s.firstGrades(3), 1);
  EXPECT_EQ(s.firstGrades(4), 1);
  EXPECT_GE(s.durationMs(), 90000);
  EXPECT_FALSE(s.answer(3, t + 100000));

  const HistorySession h = s.history();
  EXPECT_EQ(h.deck, "D");
  ASSERT_EQ(h.cards.size(), static_cast<size_t>(4 + repeats));
  EXPECT_EQ(h.cards[0].lemma, "a");
  EXPECT_EQ(h.cards[0].sense, "A");
  EXPECT_EQ(h.cards[0].grade, 1);
}

TEST_F(Sd, LinguettaSessionAppendsReviewOps) {
  put(bankPath("it"), readFixture("bank.jsonl"));
  LocalView view;
  view.load(*fs, "it");
  const int64_t now = at("2026-11-11T22:00:00Z");
  const auto queue = view.queue(now);
  ASSERT_FALSE(queue.empty());
  int n = 0;
  ViewSource src(
      view, *fs,
      [&](const std::string& line) {
        if (!appendOutbox(*fs, line)) return false;
        view.applyOp(line);
        return true;
      },
      [&] { return "00000000-0000-4000-8000-00000000000" + std::to_string(n++); });
  ReviewSession s;
  s.start(src, view.header().deckName, queue, now);
  const int first = s.current();
  const CardState before = view.entry(first).card;
  s.shown(now);
  ASSERT_TRUE(s.answer(3, now + 6400));
  EXPECT_NE(view.entry(first).card.reps, before.reps);

  std::vector<std::string> lines;
  forEachOutbox(*fs, [&](const OutboxLine& l) {
    lines.push_back(l.json);
    return true;
  });
  ASSERT_EQ(lines.size(), 1u);
  JsonDocument op;
  ASSERT_EQ(deserializeJson(op, lines[0]), DeserializationError::Ok);
  EXPECT_STREQ(op["type"], "review");
  EXPECT_STREQ(op["language"], "it");
  EXPECT_EQ(op["rating"].as<int>(), 3);
  EXPECT_EQ(op["durationMs"].as<int>(), 6400);
  EXPECT_EQ(std::string(op["flashcardId"] | ""), view.entry(first).cardId);

  // The same op replayed from the outbox gives the same card.
  LocalView reloaded;
  reloaded.load(*fs, "it");
  EXPECT_EQ(reloaded.entry(first).card.dueMs, view.entry(first).card.dueMs);

  // No trusted clock: nothing is recorded.
  EXPECT_FALSE(src.grade(first, 3, 0, 0));
}

TEST_F(Sd, HistoryByMonthAndDetail) {
  const int64_t offset = -180 * MIN;  // UTC-3
  HistorySession a;
  a.deck = "Linguetta \xC2\xB7 Italiano";
  a.startMs = at("2026-11-11T14:34:00Z");
  a.durationMs = 289000;
  a.cards = {{"legno", "madeira", 3}, {"stufa", "estufa", 1}};
  HistorySession b;
  b.deck = "Italiano 30";
  b.startMs = at("2026-11-01T02:30:00Z");  // still October 31 at UTC-3
  b.cards = {{"x", "y", 4}};
  HistorySession c = a;
  c.startMs = at("2026-11-12T10:00:00Z");
  ASSERT_TRUE(appendHistory(*fs, a));
  ASSERT_TRUE(appendHistory(*fs, b));
  ASSERT_TRUE(appendHistory(*fs, c));

  const int64_t nov = monthStartMs(2026, 11, offset);
  EXPECT_EQ(nov, at("2026-11-01T03:00:00Z"));
  EXPECT_EQ(monthStartMs(2026, 13, offset), at("2027-01-01T03:00:00Z"));
  EXPECT_EQ(monthStartMs(2026, 0, 0), at("2025-12-01T00:00:00Z"));

  auto list = loadHistory(*fs, nov, monthStartMs(2026, 12, offset));
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0].startMs, c.startMs);  // newest first
  EXPECT_EQ(list[1].cardCount, 2);
  EXPECT_TRUE(list[1].cards.empty());

  auto oct = loadHistory(*fs, monthStartMs(2026, 10, offset), nov);
  ASSERT_EQ(oct.size(), 1u);
  EXPECT_EQ(oct[0].deck, "Italiano 30");

  HistorySession detail;
  ASSERT_TRUE(loadHistorySession(*fs, list[1].offset, detail));
  EXPECT_EQ(detail.deck, a.deck);
  EXPECT_EQ(detail.durationMs, 289000);
  ASSERT_EQ(detail.cards.size(), 2u);
  EXPECT_EQ(detail.cards[1].lemma, "stufa");
  EXPECT_EQ(detail.cards[1].grade, 1);
  EXPECT_FALSE(loadHistorySession(*fs, -1, detail));
}

TEST(Session, FormatsDurations) {
  EXPECT_EQ(formatDuration(38000), "38 s");
  EXPECT_EQ(formatDuration(289000), "4 min 49 s");
  EXPECT_EQ(formatDuration(3900000), "1 h 05 min");
}

TEST(Text, FindCaseless) {
  EXPECT_EQ(findCaseless("Era uma vez um pedaço de Legno.", "legno"), 26u);  // "ç" is two bytes
  EXPECT_EQ(findCaseless("\xC3\x89 bello", "\xC3\xA9"), 0u);
  EXPECT_EQ(findCaseless("abc", "x"), std::string::npos);
  EXPECT_EQ(findCaseless("abc", ""), std::string::npos);
}

TEST(History, Labels) {
  int64_t start = 0, now = 0;
  ASSERT_TRUE(parseIso("2026-11-11T14:34:00Z", start));
  ASSERT_TRUE(parseIso("2026-11-11T20:00:00Z", now));
  const int64_t brt = -180LL * 60 * 1000;
  EXPECT_EQ(sessionDayLabel(start, now, brt), "Hoje \xC2\xB7 11:34");
  EXPECT_EQ(sessionDayLabel(start, now + 86400000LL, brt), "Ontem \xC2\xB7 11:34");
  EXPECT_EQ(sessionDayLabel(start, now + 3 * 86400000LL, brt), "11/11 \xC2\xB7 11:34");
  // 01:30 UTC on the 12th is still the 11th at UTC-3.
  ASSERT_TRUE(parseIso("2026-11-12T01:30:00Z", start));
  EXPECT_EQ(sessionDayLabel(start, now, brt), "Hoje \xC2\xB7 22:30");
  EXPECT_EQ(sessionDayLabel(start, 0, 0), "12/11 \xC2\xB7 01:30");
  EXPECT_EQ(monthLabel(2026, 3), "MAR\xC3\x87O 2026");
}

TEST(Csv, ParsesLines) {
  std::string f, b;
  EXPECT_TRUE(parseCsvLine("\"a\",\"a (prepositio) / para\"\r", f, b));
  EXPECT_EQ(f, "a");
  EXPECT_EQ(b, "a (prepositio) / para");
  EXPECT_TRUE(parseCsvLine("\"di, de\",\"diz \"\"oi\"\"\",extra", f, b));
  EXPECT_EQ(f, "di, de");
  EXPECT_EQ(b, "diz \"oi\"");
  EXPECT_TRUE(parseCsvLine("cane,cachorro", f, b));
  EXPECT_EQ(b, "cachorro");
  EXPECT_FALSE(parseCsvLine("solo", f, b));
  EXPECT_FALSE(parseCsvLine("", f, b));
}

TEST_F(Sd, LoadsCsvDeckSkippingHeader) {
  put("/flashcards/Italiano-30.csv", "\"Front\",\"Back\"\n\"che\",\"que\"\n\"di\",\"de\"\n");
  put("/flashcards/Dup.tsv", "x\ty\n");
  put("/flashcards/Dup.csv", "\"z\",\"w\"\n");
  const auto decks = listTsvDecks(*fs);
  ASSERT_EQ(decks.size(), 2u);
  TsvDeck deck;
  ASSERT_TRUE(deck.load(*fs, "Italiano-30"));
  ASSERT_EQ(deck.size(), 2u);
  EXPECT_EQ(deck.card(0).front, "che");
  EXPECT_EQ(deck.card(1).back, "de");
  TsvDeck dup;
  ASSERT_TRUE(dup.load(*fs, "Dup"));
  EXPECT_EQ(dup.card(0).front, "x");
}

// Banks and outboxes written before the server renamed card -> flashcard still load,
// new review ops say flashcardId, and a session on them ends.
TEST_F(Sd, LegacyCardKeysLoadAndSessionEnds) {
  std::string bank = readFixture("bank.jsonl");
  for (size_t p; (p = bank.find("\"flashcard\":")) != std::string::npos;) bank.replace(p, 12, "\"card\":");
  put(bankPath("it"), bank);
  const std::string sorridereId = "9c4b1d77-3e2f-4a60-b8c5-2d7e9f0a1b06";
  put(paths::OUTBOX, "{\"clientId\":\"11111111-1111-4111-8111-111111111111\",\"type\":\"review\",\"at\":\"2026-11-11T21:40:00.000Z\","
                     "\"language\":\"it\",\"cardId\":\"" + sorridereId + "\",\"rating\":3}\n");
  LocalView view;
  view.load(*fs, "it");
  const int sorridere = view.find("sorridere");
  ASSERT_GE(sorridere, 0);
  EXPECT_EQ(view.entry(sorridere).cardId, sorridereId);
  EXPECT_EQ(view.entry(sorridere).card.reps, 2);  // 1 in the bank + the legacy review

  const int64_t now = at("2026-11-12T22:00:00Z");
  int n = 0;
  ViewSource src(
      view, *fs,
      [&](const std::string& line) {
        if (!appendOutbox(*fs, line)) return false;
        view.applyOp(line);
        return true;
      },
      [&] { return "00000000-0000-4000-8000-0000000000" + std::to_string(10 + n++); });
  ReviewSession s;
  s.start(src, "it", view.queue(now), now);
  ASSERT_GT(s.total(), 0);
  int64_t t = now;
  int answers = 0;
  while (!s.done() && answers < 50) {
    ASSERT_TRUE(s.answer(answers % 3 == 0 ? 1 : 3, t += 2000));
    answers++;
  }
  EXPECT_TRUE(s.done());
  EXPECT_LT(answers, 50);
  EXPECT_NE(get(paths::OUTBOX).find("\"flashcardId\""), std::string::npos);
}

// An item without a flashcard is not queued, and cannot be graded.
TEST_F(Sd, ItemWithoutFlashcardIsNotGraded) {
  put(bankPath("it"),
      "{\"t\":\"header\",\"protocol\":1,\"language\":\"it\",\"mode\":\"full\",\"cursor\":\"c\",\"deckName\":\"D\"}\n"
      "{\"t\":\"item\",\"id\":\"i1\",\"lemma\":\"casa\",\"language\":\"it\",\"status\":\"ativo\",\"createdAt\":\"2026-11-01T00:00:00.000Z\"}\n");
  LocalView view;
  view.load(*fs, "it");
  ViewSource src(view, *fs, [](const std::string&) { return true; }, [] { return std::string("id"); });
  const int64_t now = at("2026-11-12T22:00:00Z");
  EXPECT_TRUE(view.queue(now).empty());
  EXPECT_FALSE(src.grade(0, 3, now, 1000));
}
