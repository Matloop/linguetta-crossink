#pragma once
// FSRS-7 scheduler, a port of the server's ts-fsrs 6.0.0 DefaultScheduler
// (desiredRetention from the bank header, default 0.9; fuzz on, short-term steps 1m/10m, relearning 10m,
// allowModelOverride off,
// maximumInterval 36500, fractional days). Must pass test/linguetta/fixtures/fsrs-vectors.json.
// Pure C++: no Arduino dependencies, so it runs in host tests.

#include <cstdint>
#include <string>
#include <vector>

namespace linguetta {

enum class CardStatus : uint8_t { New = 0, Learning = 1, Review = 2, Relearning = 3 };

struct CardState {
  int64_t dueMs = 0;
  double stability = 0;
  double stabilityFast = 0;
  double difficulty = 0;
  int state = 0;               // 0 new, 1 learning, 2 review, 3 relearning
  int64_t lastReviewMs = -1;   // -1 = null
  int reps = 0;
  int lapses = 0;
  double scheduledDays = 0;
  int learningStep = 0;

  // "new" | "learning" | "review" (relearning is also "learning")
  const char* scheduleStatus() const;
  // Stored status as received; state()+this pair is what the server keeps.
  uint8_t scheduleStatusCode = 0;  // 0 new, 1 learning, 2 review
  bool setScheduleStatus(const char* s);
};

namespace fsrs {

constexpr int64_t MS_PER_DAY = 86400000LL;
// The account's desiredRetention arrives in the bank header (settings.desiredRetention);
// this is the server default, used when a bank does not say.
constexpr double DEFAULT_RETENTION = 0.9;

CardState newCard(int64_t createdMs);

// Applies one rating (1 Again, 2 Hard, 3 Good, 4 Easy) at nowMs. `seedId` is the
// card id (or the saveClientId for a card that only exists locally); it seeds the fuzz.
CardState review(const CardState& card, const std::string& seedId, int grade, int64_t nowMs,
                 double retention = DEFAULT_RETENTION);

// Replays a log from a new card; entries are sorted by time (stable), like the server.
struct LogEntry {
  int grade;
  int64_t atMs;
};
CardState replay(const std::string& cardId, int64_t createdMs, std::vector<LogEntry> logs);

// Interval (days) each grade would get now, for the grade-button labels.
void previewDays(const CardState& card, const std::string& seedId, int64_t nowMs, double out[4],
                 double retention = DEFAULT_RETENTION);

// "1 min", "10 min", "3 h", "4 d", "2 meses", "1,5 anos" (same as the app's formatInterval).
std::string formatInterval(double days);

// Due for today's queue (the app's isDue; CONTRACT, "Fila do baralho no aparelho"):
// learning/relearning steps are minutes and fall due at the instant; a review card (days)
// falls due for the whole study day it lands on (dueMs < dayEndMs, next day start), as in Anki.
inline bool isDue(const CardState& c, int64_t nowMs, int64_t dayEndMs) {
  if (c.state == 0) return false;
  return c.state == 2 ? c.dueMs < dayEndMs : c.dueMs <= nowMs;
}

}  // namespace fsrs
}  // namespace linguetta
