#pragma once
// Builders for the three outbox operations (contract section 4). Every text field is cut to
// the contract limits (code points) before it is written; the device never edits a line later.

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Fsrs.h"

namespace linguetta {

namespace limits {
constexpr size_t LEMMA = 80, SENSE = 200, NOTE = 2000, IPA = 100, POS = 80, FORM = 80, SENTENCE = 500,
                 TITLE = 200, AUTHOR = 200, CHAPTER = 200, DEVICE_NAME = 60;
constexpr int EXTRA_NEW = 1000;  // extraNew.n
constexpr size_t DECK_NAME = 80, DECK_HASH = 128, DECK_ROWS = 500, FICHA_TEXT = 2000;
}

struct SaveInput {
  std::string language;
  std::string lemma, sense, note, ipa, pos;
  // encounter
  std::string form, sentence, encounterSense, chapter;
  int spine = -1;        // < 0: omitted
  double percent = -1;   // < 0: omitted
  // work
  std::string koHash, title, author, workLanguage;
};

// Returns "" when the lemma is empty after trimming (the server would reject it).
std::string buildSave(const std::string& clientId, int64_t atMs, const SaveInput& in);
// Exactly one of cardId / saveClientId must be non-empty. durationMs < 0: omitted.
std::string buildReview(const std::string& clientId, int64_t atMs, const std::string& language,
                        const std::string& cardId, const std::string& saveClientId, int rating, int64_t durationMs);
std::string buildSeen(const std::string& clientId, int64_t atMs, const std::string& language, const std::string& lemma);
// "+N novas hoje". n is clamped to 1..1000.
std::string buildExtraNew(const std::string& clientId, int64_t atMs, const std::string& language, int n);

struct DeckRow {
  std::string front, back;
};
// A CSV/TSV deck from the SD becomes a server deck. Name: cut to 80 code points, "::" removed.
// Rows without front are skipped; "" when nothing is left (or more than 500 rows).
std::string buildDeckImport(const std::string& clientId, int64_t atMs, const std::string& language,
                            const std::string& name, const std::string& hash, const std::vector<DeckRow>& rows);
// The deck name the server will see for a file name (without extension).
std::string deckImportName(const std::string& name);

// CardState <-> snapshot JSON (camelCase, section 7).
bool cardFromJson(JsonObjectConst o, CardState& out, std::string& id, int64_t& createdMs);

}  // namespace linguetta
