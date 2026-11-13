#pragma once
// SD layout from the sync contract (section 8): /.linguetta/{outbox.jsonl, state.json,
// rejected.jsonl, cache/}, with the bank split per study language: bank/<language>.jsonl
// (one synced deck per language; the contract's bank.jsonl format, one file each). Plus
// device-only files that never sync: settings.json (server URL override), history.jsonl and
// decks/ (local TSV deck state).

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Fs.h"

namespace linguetta {

namespace paths {
constexpr const char* ROOT = "/.linguetta";
constexpr const char* BANK_DIR = "/.linguetta/bank";
constexpr const char* LEGACY_BANK = "/.linguetta/bank.jsonl";  // single-language layout (migrated)
constexpr const char* LEGACY_BANK_TMP = "/.linguetta/bank.jsonl.tmp";
constexpr const char* OUTBOX = "/.linguetta/outbox.jsonl";
constexpr const char* STATE = "/.linguetta/state.json";
constexpr const char* REJECTED = "/.linguetta/rejected.jsonl";
constexpr const char* CACHE = "/.linguetta/cache";
constexpr const char* SETTINGS_FILE = "/.linguetta/settings.json";
constexpr const char* HISTORY = "/.linguetta/history.jsonl";
constexpr const char* BOOKS = "/.linguetta/books.json";
constexpr const char* DECK_STATE_DIR = "/.linguetta/decks";
constexpr const char* TSV_DIR = "/flashcards";
}  // namespace paths

// "/.linguetta/bank/it.jsonl". "" if the code is not a plausible language tag.
std::string bankPath(const std::string& language);
// Languages that have a bank file on the card.
std::vector<std::string> bankLanguages(Fs& fs);
// Language of a bank file's header line; false if there is no valid header.
bool readBankHeader(Fs& fs, const std::string& path, std::string& language);

constexpr const char* DEFAULT_SERVER = "https://linguetta-app.vercel.app";

struct Account {
  std::string id, email, nativeLanguage;
  std::vector<std::string> studyLanguages;
};

// state.json (contract section 8). Unknown fields are not preserved; the file is device-owned.
struct SyncState {
  std::string server = DEFAULT_SERVER;
  std::string token, tokenHint, deviceName = "X4 Pro";
  Account account;
  std::string language;  // first study language (informational; every one of them syncs)
  std::string cursor;    // cursor of `language`, kept for the contract's state.json shape
  // Snapshot cursor per study language (device field "cursors"); missing = full snapshot.
  std::map<std::string, std::string> cursors;
  std::string lastSyncAt;
  std::string lastSyncStatus;
  std::string lastError;
  // Device-only backoff bookkeeping (contract: 5xx -> 30 s, 2 min, 10 min; 429 -> Retry-After).
  int failures = 0;
  int64_t retryAtMs = 0;

  bool paired() const { return !token.empty(); }
};

// settings.json: {"server": "...", "deviceName": "...", "language": "it"}; all optional.
// `language`: blend books whose words are in no bank are saved in this language.
struct Settings {
  std::string server = DEFAULT_SERVER;
  std::string deviceName = "X4 Pro";
  std::string language;
};

bool loadState(Fs& fs, SyncState& out);  // false if missing/invalid (out keeps defaults)
bool saveState(Fs& fs, const SyncState& s);
Settings loadSettings(Fs& fs);

// Moves the single-language /.linguetta/bank.jsonl to bank/<its language>.jsonl (or drops it
// when that file already exists or it has no header) and forgets every cursor, so the next
// sync downloads full snapshots. True if anything changed.
bool migrateLegacyBank(Fs& fs, SyncState& st);

// Decks to show: the account's study languages (in order), then any other language that has
// a bank file or outbox ops on the card.
std::vector<std::string> deckLanguages(Fs& fs, const SyncState& st);

// What the device knows about a blend book when a word in it is tapped.
struct BlendBook {
  std::string language;  // OPF dc:language: usually the BASE language of a blend book ("pt")
  std::string name;      // file name and title: "O Hobbit - blend PT-IT"
  std::string choice;    // the language picked for this book on the device (books.json), or ""
};

// Target language spelled in a blend book's name as a BASE-TARGET pair ("PT-IT", "pt_it"):
// the second code when it is a study language other than the native one; "" otherwise.
std::string blendLanguageFromName(const SyncState& st, const std::string& name);

// Language of the ops (save / seen) for a word tapped in a book. #lb links carry no language,
// so, in order: the language picked for this book on the device; a BASE-TARGET pair in its
// name; the book's dc:language if it is a study language other than the native one ("it-IT"
// matches "it"); the study language whose deck already has the lemma (hasLemma); settings.json
// "language" if it is a study language; the first study language that is not the native one;
// the first study language; "it".
std::string pickBlendLanguage(const SyncState& st, const BlendBook& book, const std::string& settingsLanguage,
                              const std::function<bool(const std::string&)>& hasLemma);

// Study languages a blend word can be saved in (the non-native ones, else all of them).
std::vector<std::string> blendLanguages(const SyncState& st);

// books.json: {"<koHash>": "it", ...}, the language picked per blend book.
std::string loadBookLanguage(Fs& fs, const std::string& koHash);
bool saveBookLanguage(Fs& fs, const std::string& koHash, const std::string& language);

// ---- outbox ----

// Appends one JSON line and syncs it. The line must not contain '\n'.
bool appendOutbox(Fs& fs, const std::string& jsonLine);

struct OutboxLine {
  std::string json;
  std::string clientId;
};
// Calls fn for every valid line in file order; a final incomplete/invalid line is ignored.
// Returning false from fn stops early.
void forEachOutbox(Fs& fs, const std::function<bool(const OutboxLine&)>& fn);

struct OutboxCounts {
  int saves = 0, seen = 0, reviews = 0, other = 0;
  int total() const { return saves + seen + reviews + other; }
};
OutboxCounts countOutbox(Fs& fs);

// Rewrites the outbox without the given client ids (atomic). Lines appended meanwhile stay.
bool rewriteOutboxWithout(Fs& fs, const std::set<std::string>& resolved);

// rejected.jsonl keeps at most 100 lines (oldest dropped).
void appendRejected(Fs& fs, const std::vector<std::string>& lines);

}  // namespace linguetta
