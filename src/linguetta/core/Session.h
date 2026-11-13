#pragma once
// One review session over a deck (the synced Linguetta deck or a local TSV deck): the queue,
// same-session repeats for cards that come due again within 20 minutes, the per-grade summary
// and the history entry. The deck itself records each answer (outbox op or deck log).

#include <cstdint>
#include <string>
#include <vector>

#include "Fsrs.h"
#include "History.h"

namespace linguetta {

class CardSource {
 public:
  virtual ~CardSource() = default;
  virtual const CardState& state(int i) const = 0;
  virtual std::string seed(int i) const = 0;
  // Front text and short back text, for the history.
  virtual void label(int i, std::string& lemma, std::string& sense) const = 0;
  // Records the answer and updates the card. False when it could not be stored.
  virtual bool grade(int i, int rating, int64_t nowMs, int64_t durationMs) = 0;
  // A teimosa waiting for an encounter (settings.leech): out of the queue.
  virtual bool leech(int) const { return false; }
};

constexpr int64_t SESSION_REPEAT_MS = 20 * 60 * 1000;
constexpr int64_t MAX_DURATION_MS = 3600000;  // contract: durationMs 0..3600000

class ReviewSession {
 public:
  void start(CardSource& source, const std::string& deck, std::vector<int> queue, int64_t nowMs);

  bool done() const { return pos >= order.size(); }
  int current() const { return done() ? -1 : order[pos]; }
  // "4 / 18": the first-time cards seen so far, out of the initial queue.
  int position() const;
  int total() const { return initial; }
  // Answers still to give, learning-step repeats included (what the header shows).
  int remaining() const { return static_cast<int>(order.size()) - pos; }
  // The front became visible (answer time is measured from here).
  void shown(int64_t nowMs) { shownAt = nowMs; }
  // Grades the current card and moves on. False if the deck could not store it.
  bool answer(int rating, int64_t nowMs);
  // The card just answered became a teimosa: it left the session (shown once as a notice).
  int lastLeech() const { return justLeech; }
  // Cards that became teimosas in this session, in order.
  const std::vector<int>& leeches() const { return leechCards; }

  // Summary: first answer per card.
  int cards() const { return firstCount; }
  int firstGrades(int rating) const { return rating >= 1 && rating <= 4 ? byGrade[rating] : 0; }
  int answers() const { return static_cast<int>(log.size()); }
  int64_t startMs() const { return started; }
  int64_t durationMs() const { return lastAt > started ? lastAt - started : 0; }
  HistorySession history() const;

 private:
  CardSource* src = nullptr;
  std::string deckName;
  std::vector<int> order;   // initial queue, then repeats appended
  std::vector<bool> repeat;  // order[k] is a repeat
  size_t pos = 0;
  int initial = 0, firstCount = 0;
  int byGrade[5] = {0, 0, 0, 0, 0};
  int64_t started = 0, lastAt = 0, shownAt = 0;
  std::vector<HistoryCard> log;
  std::vector<int> leechCards;
  int justLeech = -1;
};

// "4 min 49 s", "38 s", "1 h 05 min"
std::string formatDuration(int64_t ms);

}  // namespace linguetta
