#include "Session.h"

#include <algorithm>
#include <cstdio>

namespace linguetta {

void ReviewSession::start(CardSource& source, const std::string& deck, std::vector<int> queue, int64_t nowMs) {
  src = &source;
  deckName = deck;
  order = std::move(queue);
  repeat.assign(order.size(), false);
  pos = 0;
  initial = static_cast<int>(order.size());
  firstCount = 0;
  std::fill(std::begin(byGrade), std::end(byGrade), 0);
  started = lastAt = shownAt = nowMs;
  log.clear();
  leechCards.clear();
  justLeech = -1;
}

int ReviewSession::position() const {
  if (done()) return initial;
  return repeat[pos] ? std::min(initial, firstCount) : std::min(initial, firstCount + 1);
}

bool ReviewSession::answer(int rating, int64_t nowMs) {
  if (done() || !src || rating < 1 || rating > 4) return false;
  const int card = order[pos];
  const int64_t duration = std::clamp<int64_t>(nowMs - shownAt, 0, MAX_DURATION_MS);
  if (!src->grade(card, rating, nowMs, duration)) return false;
  if (!repeat[pos]) {
    firstCount++;
    byGrade[rating]++;
  }
  HistoryCard h;
  src->label(card, h.lemma, h.sense);
  h.grade = rating;
  log.push_back(std::move(h));
  lastAt = nowMs;
  pos++;
  justLeech = -1;
  // A teimosa waits for an encounter instead of coming back.
  if (src->leech(card)) {
    justLeech = card;
    if (std::find(leechCards.begin(), leechCards.end(), card) == leechCards.end()) leechCards.push_back(card);
    return true;
  }
  // Learning steps (1 min / 10 min) come back in this session, after the remaining cards.
  // A card still "new" after the grade was not updated (its review could not be applied):
  // repeating it would loop forever, so it leaves the session.
  const CardState& st = src->state(card);
  if (st.state > 0 && st.dueMs - nowMs <= SESSION_REPEAT_MS) {
    order.push_back(card);
    repeat.push_back(true);
  }
  return true;
}

HistorySession ReviewSession::history() const {
  HistorySession h;
  h.deck = deckName;
  h.startMs = started;
  h.durationMs = durationMs();
  h.cards = log;
  h.cardCount = static_cast<int>(log.size());
  return h;
}

std::string formatDuration(int64_t ms) {
  const int64_t s = std::max<int64_t>(0, ms / 1000);
  char buf[32];
  if (s < 60) {
    snprintf(buf, sizeof buf, "%d s", static_cast<int>(s));
  } else if (s < 3600) {
    snprintf(buf, sizeof buf, "%d min %d s", static_cast<int>(s / 60), static_cast<int>(s % 60));
  } else {
    snprintf(buf, sizeof buf, "%d h %02d min", static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60));
  }
  return buf;
}

}  // namespace linguetta
