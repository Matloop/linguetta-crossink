#pragma once
// Local flashcard decks: SD /flashcards/*.tsv (one card per line "front<TAB>back") or *.csv
// ("front","back", optional "Front","Back" header row).
// Scheduled with the same FSRS as the Linguetta deck (the account's new-per-day and retention
// from the bank header). Once the account exists, each file goes up as a deckImport op on the
// next sync (DeckImport.h) and the reader studies it as a server deck. Each deck keeps an
// append-only review log in /.linguetta/decks/<deck>.jsonl ({"f":front,"g":grade,"at":ms});
// the card states are rebuilt from it on load, like the server rebuilds cards from review logs.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Fs.h"
#include "Fsrs.h"

namespace linguetta {

constexpr size_t TSV_MAX_CARDS = 5000;

struct TsvCard {
  std::string front, back;
  CardState card;
  int64_t firstReviewMs = -1;  // -1: never reviewed (new)
};

// Parses one TSV line. False for blank lines, '#' comments and lines without a tab or
// with an empty front. Only the first two columns are used; a UTF-8 BOM and CR are dropped.
bool parseTsvLine(const std::string& line, std::string& front, std::string& back);
bool parseCsvLine(const std::string& line, std::string& front, std::string& back);

// Deck names (file names without ".tsv") in /flashcards, sorted case-insensitively.
std::vector<std::string> listTsvDecks(Fs& fs);

// Streams the rows of /flashcards/<name>.tsv (or .csv) in file order, skipping a "front,back"
// header row; `row` returns false for a row it drops (it then does not count towards
// TSV_MAX_CARDS). False when the file cannot be read.
bool readDeckRows(Fs& fs, const std::string& name,
                  const std::function<bool(const std::string& front, const std::string& back)>& row);

class TsvDeck {
 public:
  // Reads /flashcards/<name>.tsv and replays the deck's review log. Duplicate fronts keep
  // the first line. False when the TSV file cannot be read.
  bool load(Fs& fs, const std::string& name);
  // desiredRetention for the reviews (the account's, from the bank header).
  void setRetention(double r) { retention = r; }

  const std::string& name() const { return deckName; }
  size_t size() const { return cards.size(); }
  const TsvCard& card(size_t i) const { return cards[i]; }
  std::string seed(size_t i) const { return "tsv:" + deckName + ":" + cards[i].front; }

  int newDoneToday(int64_t dayStartMs) const;
  // Due cards (oldest first), then new cards in file order up to the daily allowance.
  std::vector<int> queue(int64_t nowMs, int64_t dayStartMs, int newPerDay) const;
  int dueCount(int64_t nowMs, int64_t dayStartMs, int newPerDay) const {
    return static_cast<int>(queue(nowMs, dayStartMs, newPerDay).size());
  }

  // Appends the review to the log (fsync) and updates the card. False if the write failed
  // (the card is then left unchanged).
  bool grade(Fs& fs, size_t i, int rating, int64_t nowMs);

  static std::string tsvPath(const std::string& name);
  static std::string csvPath(const std::string& name);
  static std::string logPath(const std::string& name);

 private:
  void apply(size_t i, int rating, int64_t atMs);
  std::string deckName;
  double retention = fsrs::DEFAULT_RETENTION;
  std::vector<TsvCard> cards;
  std::unordered_map<std::string, size_t> byFront;
};

}  // namespace linguetta
