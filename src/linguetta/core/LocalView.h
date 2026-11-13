#pragma once
// "Visão local" (contract section 8): bank/<language>.jsonl + outbox.jsonl, for one study language.
// Keeps a compact in-memory index (lemma key, card state, bank line offset); item details are
// read back from the bank line on demand. Built by one streaming pass over each file.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Fs.h"
#include "Fsrs.h"

namespace linguetta {

// A deck of the language that goes to the reader (header.decks, tree order).
struct DeckInfo {
  std::string id, name;
  int parent = -1;  // index of the parent deck in BankHeader::decks (one level only)
  bool isDefault = false;
};

enum class LeechAction : uint8_t { Wait, Tag, Suspend };

struct BankHeader {
  bool present = false;
  std::string language, deckName, cursor;
  int newPerDay = 10;  // settings.newFlashcardsPerDay
  int64_t dayStartMs = 0, nextDayStartMs = 0;
  int newDoneToday = 0;
  int extraNew = 0;       // today.extraNew
  int newAllowance = -1;  // today.newAllowance; -1: bank from before the server queue
  double desiredRetention = fsrs::DEFAULT_RETENTION;
  bool buryRelated = true;
  int leechLapses = 0;  // 0: off
  LeechAction leechAction = LeechAction::Wait;
  std::vector<DeckInfo> decks;
  int defaultDeck = -1;
  int fichasTotal = 0;
  bool fsrsApproximate = false;  // server scheduler parameters differ from ours
  // The queue comes from the server (queueRank + newAllowance, contract section 11).
  bool serverQueue() const { return newAllowance >= 0; }
  // A teimosa leaves the queue (wait or suspend) instead of only being tagged.
  bool leechLeavesQueue() const { return leechLapses > 0 && leechAction != LeechAction::Tag; }
};

struct Encounter {
  std::string form, sentence, sense, work, chapter;
};

struct ItemDetails {
  std::string lemma, sense, note, ipa, pos;
  // Fichas: lemma = front, sense = back (text only).
  bool ficha = false;
  bool cloze = false;  // kind "lacuna"
  Encounter encounter;  // most recent (local saves win over the bank)
  bool hasEncounter = false;
};

// Deck slot of an entry: an index into BankHeader::decks, or one of these.
constexpr int16_t DECK_NONE = -1;     // bank without decks (older server)
constexpr int16_t DECK_OFF = -2;      // deckId outside header.decks: shown, not reviewed
constexpr int DECK_ALL = -1;          // queue()/counts(): every deck ("Estudar tudo")

struct ViewEntry {
  std::string key;  // lowerKey(lemma); empty for fichas
  int64_t bankOffset = -1;  // -1: local item (from an outbox save)
  bool known = false;       // status "conhecido": in the bank, not in the deck
  std::string cardId;       // bank card id; empty for local items
  std::string saveClientId; // local items: the save that created them (also the fuzz seed)
  int64_t cardCreatedMs = 0;
  CardState card;
  int seenCount = 0;
  bool ficha = false;        // a "ficha" line (not in the lemma index)
  bool active = true;        // status "ativo" (the only one the queue takes)
  bool leechWaiting = false; // teimosa waiting for an encounter (server flag, or local lapses)
  int16_t deck = DECK_NONE;
  int queueRank = 0;          // 0: null (not new, or not today)
  int64_t siblingMs = -1;     // siblingReviewedAt
  int64_t lastSaveMs = -1;    // newest local save of the lemma with a sentence
  std::shared_ptr<ItemDetails> local;  // local item fields / newest local encounter
  const std::string& seed() const { return cardId.empty() ? saveClientId : cardId; }
};

class LocalView {
 public:
  // Rebuilds everything from the SD files: bank/<language>.jsonl and the outbox ops of `language`.
  void load(Fs& fs, const std::string& language);
  // Applies one operation line just appended to the outbox (same rules as load).
  void applyOp(const std::string& jsonLine);

  const BankHeader& header() const { return hdr; }
  const std::string& language() const { return lang; }
  size_t size() const { return entries.size(); }
  const ViewEntry& entry(size_t i) const { return entries[i]; }
  int find(const std::string& lemma) const;  // index or -1 (case-insensitive)

  bool details(Fs& fs, size_t i, ItemDetails& out) const;

  // The current "day" per the header (after nextDayStart: 24 h days from it).
  int64_t dayStart(int64_t nowMs) const;
  // End of that study day: review cards due before it are in today's queue (fsrs::isDue).
  int64_t nextDayStart(int64_t nowMs) const;
  int newDoneToday(int64_t nowMs) const;

  // Deck queue (section 11): due (oldest first) then new in queueRank order, up to the local
  // budget. `deck`: DECK_ALL, or a deck index (the deck and its children).
  std::vector<int> queue(int64_t nowMs, int deck = DECK_ALL) const;
  struct Counts {
    int due = 0, fresh = 0;
    int total() const { return due + fresh; }
  };
  Counts counts(int64_t nowMs, int deck = DECK_ALL) const;
  // New flashcards that may still start today (the local budget, section 8).
  int newAllowance(int64_t nowMs) const;
  // Today's new limit: newFlashcardsPerDay + the day's extras.
  int newLimit(int64_t nowMs) const;
  // New flashcards waiting beyond today's budget (what "+5 hoje" would bring in).
  int newBeyondBudget(int64_t nowMs, int deck = DECK_ALL) const;
  // Cards of a deck and its children (DECK_ALL: every card the reader studies).
  int cardCount(int deck) const;
  bool inDeck(const ViewEntry& e, int deck) const;

 private:
  struct PendingReview {
    int entry;
    int64_t atMs;
    int rating;
    size_t order;
  };
  void parseHeader(const std::string& line);
  void addBankLine(const std::string& line, int64_t offset);
  int16_t deckIndex(const char* deckId) const;
  bool reviewable(const ViewEntry& e) const;
  bool buried(const ViewEntry& e, int64_t dayStart) const;
  int extraToday(int64_t nowMs) const;
  // Returns the entry index a review targets, or -1.
  int applyOpParsed(const std::string& line, std::vector<PendingReview>* deferred);
  void applyReview(int entry, int64_t atMs, int rating);

  std::string lang;
  BankHeader hdr;
  std::vector<ViewEntry> entries;
  std::unordered_map<std::string, int> byKey, byCardId, bySaveId;
  std::vector<int64_t> newReviewTimes;  // reviews of cards that were new at the time
  struct Extra {
    int64_t atMs;
    int n;
  };
  std::vector<Extra> extras;  // extraNew ops of the outbox
  size_t opOrder = 0;
};

}  // namespace linguetta
