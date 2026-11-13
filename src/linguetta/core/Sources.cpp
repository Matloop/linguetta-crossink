#include "Sources.h"

#include "Ops.h"

namespace linguetta {

void ViewSource::label(int i, std::string& lemma, std::string& sense) const {
  ItemDetails d;
  if (view.details(fs, i, d)) {
    lemma = d.lemma;
    sense = d.sense;
  } else {
    lemma = view.entry(i).key;
    sense.clear();
  }
}

bool ViewSource::grade(int i, int rating, int64_t nowMs, int64_t durationMs) {
  if (nowMs <= 0) return false;  // no trusted clock: nothing is recorded (contract section 4)
  const ViewEntry& e = view.entry(i);
  if (e.cardId.empty() && e.saveClientId.empty()) return false;  // the server would reject it
  const std::string line = buildReview(newId(), nowMs, view.language(), e.cardId,
                                       e.cardId.empty() ? e.saveClientId : std::string(), rating, durationMs);
  return !line.empty() && record(line);
}

}  // namespace linguetta
