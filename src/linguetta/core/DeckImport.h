#pragma once
// CSV/TSV decks of the SD go up to the account (contract section 4, `deckImport`): on sync,
// each file in /flashcards that was never sent becomes one or more deckImport ops in the
// outbox; once the server applied them all, the file is "imported" and the reader studies it
// as a server deck (the local deck is hidden). Bookkeeping in /.linguetta/imports.json.
//
// The hash is SHA-256 over the parsed rows (front TAB back LF, after the same parsing and
// de-duplication as the local deck), so line endings or a BOM do not change it. A file with
// more rows than one op allows is split; each part gets its own hash (".2", ".3"...) and name.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Fs.h"
#include "Ops.h"

namespace linguetta {

struct DeckImportResult;

namespace deckimport {

constexpr const char* FILE_PATH = "/.linguetta/imports.json";
// Rows of one op stay under this many JSON bytes (the outbox and push read 16 KB lines).
constexpr size_t MAX_OP_BYTES = 12000;

struct Part {
  std::string name, hash;
  std::vector<DeckRow> rows;
};

// Rows of a deck file: de-duplicated by front, at most TSV_MAX_CARDS. False if unreadable.
bool readRows(Fs& fs, const std::string& deck, std::vector<DeckRow>& rows);
// "sha256:<hex>" of the rows.
std::string rowsHash(const std::vector<DeckRow>& rows);
// Splits the rows into ops of at most 500 rows and MAX_OP_BYTES; names and hashes per part.
std::vector<Part> split(const std::string& deck, const std::vector<DeckRow>& rows);

enum class State { None, Queued, Imported, Rejected };

struct Entry {
  State state = State::None;
  std::string hash;
  std::vector<std::string> ops;  // clientIds of the parts, in order
  std::vector<std::string> deckIds;
};

std::map<std::string, Entry> load(Fs& fs);
bool save(Fs& fs, const std::map<std::string, Entry>& entries);

// Appends the ops of every deck file not queued yet (language: the deck's study language).
// Returns how many files were queued.
int queue(Fs& fs, const std::string& language, int64_t nowMs, const std::function<std::string()>& newId);
// Records the push results of a successful sync: a file whose parts were all applied (or
// duplicate) becomes Imported; any rejected or dropped part makes it Rejected (shown locally,
// not sent again). Returns how many files became Imported.
int settle(Fs& fs, const std::map<std::string, DeckImportResult>& results);
// Deck files already imported (hidden from the local deck list).
std::set<std::string> imported(Fs& fs);

}  // namespace deckimport
}  // namespace linguetta
