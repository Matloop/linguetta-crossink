#include <gtest/gtest.h>

#include <cmath>

#include "Fsrs.h"
#include "Text.h"
#include "TestUtil.h"

using namespace linguetta;

namespace {

void expectNear(double expected, double actual, double tol, const std::string& what) {
  const double diff = std::fabs(expected - actual);
  const double allowed = std::max(tol, tol * std::fabs(expected));
  EXPECT_LE(diff, allowed) << what << " expected " << expected << " got " << actual;
}

void expectCard(JsonObjectConst e, const CardState& c, const std::string& where, double tol, int64_t dueTol) {
  int64_t due = 0;
  ASSERT_TRUE(parseIso(e["due_at"], due));
  EXPECT_LE(std::llabs(due - c.dueMs), dueTol) << where << " due_at " << e["due_at"].as<const char*>() << " got "
                                                << formatIso(c.dueMs);
  expectNear(e["stability"], c.stability, tol, where + " stability");
  expectNear(e["stability_fast"], c.stabilityFast, tol, where + " stability_fast");
  expectNear(e["difficulty"], c.difficulty, tol, where + " difficulty");
  expectNear(e["scheduled_days"], c.scheduledDays, tol, where + " scheduled_days");
  EXPECT_EQ(e["state"].as<int>(), c.state) << where;
  EXPECT_STREQ(e["schedule_status"].as<const char*>(), c.scheduleStatus()) << where;
  EXPECT_EQ(e["reps"].as<int>(), c.reps) << where;
  EXPECT_EQ(e["lapses"].as<int>(), c.lapses) << where;
  EXPECT_EQ(e["learning_step"].as<int>(), c.learningStep) << where;
  if (e["last_review_at"].isNull()) {
    EXPECT_EQ(c.lastReviewMs, -1) << where;
  } else {
    EXPECT_EQ(formatIso(c.lastReviewMs), e["last_review_at"].as<std::string>()) << where;
  }
}

}  // namespace

TEST(FsrsVectors, EveryStepMatchesTheServer) {
  JsonDocument doc = loadJsonFixture("fsrs-vectors.json");
  const double tol = doc["tolerance"]["float"];
  const int64_t dueTol = doc["tolerance"]["dueAtMs"];
  JsonArrayConst cases = doc["cases"];
  ASSERT_GE(cases.size(), 5u);
  int checked = 0;
  for (JsonObjectConst k : cases) {
    const std::string name = k["name"];
    const std::string cardId = k["flashcardId"];
    int64_t created = 0;
    ASSERT_TRUE(parseIso(k["createdAt"], created));
    CardState card = fsrs::newCard(created);
    int i = 0;
    for (JsonObjectConst step : k["steps"].as<JsonArrayConst>()) {
      int64_t at = 0;
      ASSERT_TRUE(parseIso(step["reviewedAt"], at));
      card = fsrs::review(card, cardId, step["rating"], at);
      expectCard(step["expected"], card, name + " step " + std::to_string(++i), tol, dueTol);
      checked++;
    }
  }
  EXPECT_GT(checked, 25);
}

TEST(FsrsVectors, OutOfOrderReplaySortsByReviewTime) {
  JsonDocument doc = loadJsonFixture("fsrs-vectors.json");
  for (JsonObjectConst k : doc["cases"].as<JsonArrayConst>()) {
    if (k["arrivalOrder"].isNull()) continue;
    JsonArrayConst steps = k["steps"];
    std::vector<fsrs::LogEntry> logs;
    for (int idx : k["arrivalOrder"].as<JsonArrayConst>()) {
      int64_t at = 0;
      ASSERT_TRUE(parseIso(steps[idx]["reviewedAt"], at));
      logs.push_back({steps[idx]["rating"].as<int>(), at});
    }
    int64_t created = 0;
    ASSERT_TRUE(parseIso(k["createdAt"], created));
    const CardState card = fsrs::replay(k["flashcardId"].as<std::string>(), created, logs);
    expectCard(steps[steps.size() - 1]["expected"], card, "replay", doc["tolerance"]["float"],
               doc["tolerance"]["dueAtMs"]);
    return;
  }
  FAIL() << "no arrivalOrder case";
}

TEST(Fsrs, FormatInterval) {
  EXPECT_EQ(fsrs::formatInterval(1.0 / 1440), "1 min");
  EXPECT_EQ(fsrs::formatInterval(10.0 / 1440), "10 min");
  EXPECT_EQ(fsrs::formatInterval(0.0), "1 min");
  EXPECT_EQ(fsrs::formatInterval(5.0 / 24), "5 h");
  EXPECT_EQ(fsrs::formatInterval(3), "3 d");
  EXPECT_EQ(fsrs::formatInterval(40), "1 m\xC3\xAAs");
  EXPECT_EQ(fsrs::formatInterval(100), "3 meses");
  EXPECT_EQ(fsrs::formatInterval(400), "1,1 ano");
  EXPECT_EQ(fsrs::formatInterval(730), "2 anos");
}

TEST(Fsrs, PreviewOfNewCard) {
  const CardState c = fsrs::newCard(0);
  double days[4];
  fsrs::previewDays(c, "0b9f3c2e-6a51-4d7e-9c1a-3f2b8d4e5a01", 1000, days);
  EXPECT_NEAR(days[0], 1.0 / 1440, 1e-9);
  EXPECT_NEAR(days[1], 5.5 / 1440, 1e-9);  // Hard stays on the step (allowModelOverride off)
  EXPECT_NEAR(days[2], 10.0 / 1440, 1e-9);  // Good moves to the 10-minute step
}

TEST(Text, IsoRoundTrip) {
  int64_t ms = 0;
  ASSERT_TRUE(parseIso("2026-11-11T08:15:30.000Z", ms));
  EXPECT_EQ(formatIso(ms), "2026-11-11T08:15:30.000Z");
  ASSERT_TRUE(parseIso("2026-11-11T08:15:30Z", ms));
  EXPECT_EQ(formatIso(ms), "2026-11-11T08:15:30.000Z");
  ASSERT_TRUE(parseIso("1970-01-01T00:00:00.001Z", ms));
  EXPECT_EQ(ms, 1);
  EXPECT_FALSE(parseIso("nope", ms));
  EXPECT_EQ(toCivil(0).weekday, 4);
}

TEST(Text, TruncateAndLower) {
  EXPECT_EQ(truncateCodePoints("ação", 2), "a\xC3\xA7");
  EXPECT_EQ(codePointCount("ação"), 4u);
  EXPECT_EQ(truncateBytes("ação", 2), "a");
  EXPECT_EQ(lowerKey("Città ÈBello"), "città èbello");
  uint8_t b[16] = {};
  EXPECT_EQ(uuidFromBytes(b), "00000000-0000-4000-8000-000000000000");
}

TEST(Fsrs, IsDueReviewByDayLearningByInstant) {
  const int64_t now = 1000 * fsrs::MS_PER_DAY + 5 * 3600000LL;
  const int64_t dayEnd = 1001 * fsrs::MS_PER_DAY;
  CardState c;
  EXPECT_FALSE(fsrs::isDue(c, now, dayEnd));  // new
  c.state = 2;
  c.dueMs = dayEnd - 1;
  EXPECT_TRUE(fsrs::isDue(c, now, dayEnd));
  c.dueMs = dayEnd;
  EXPECT_FALSE(fsrs::isDue(c, now, dayEnd));
  for (int s : {1, 3}) {
    c.state = s;
    c.dueMs = now + 1;
    EXPECT_FALSE(fsrs::isDue(c, now, dayEnd));
    c.dueMs = now;
    EXPECT_TRUE(fsrs::isDue(c, now, dayEnd));
  }
}
