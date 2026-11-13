#pragma once
// Device-wide Linguetta state: the SD files (via HalFs), the sync state, one local view per
// study language (one synced deck each) and the trusted clock. Lazily loaded on first use; single-threaded (UI loop) only.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "linguetta/core/LocalView.h"
#include "linguetta/core/Store.h"
#include "linguetta/device/HalFs.h"

namespace linguetta {

class Service {
 public:
  static Service& get();

  Fs& fs() { return halFs; }
  SyncState& state();
  bool saveState();
  const Settings& settings();

  // Languages with a deck: the account's study languages, then any bank/outbox leftovers.
  std::vector<std::string> languages();
  std::string deckName(const std::string& language);  // "Linguetta · Italiano"
  // Language of save/seen ops for a word read in a book (see pickBlendLanguage).
  // koHash/name/language describe the book; the per-book choice is read from books.json.
  std::string blendLanguage(const std::string& lemma, const std::string& koHash, const std::string& bookName,
                            const std::string& bookLanguage);
  // Remembers the language picked for a book in the footer (books.json).
  bool setBookLanguage(const std::string& koHash, const std::string& language);
  std::vector<std::string> blendLanguages();

  LocalView& view(const std::string& language);
  void invalidateViews() {
    views.clear();
    gen++;
  }
  // Changes whenever bank items may have changed (new views, any op but `seen`).
  uint32_t generation() const { return gen; }

  // UTC ms when the clock is trustworthy (>= 2026), else 0: ops must not be recorded then.
  int64_t nowMs() const;
  std::string newId() const;
  // Offset of the device's clock zone (Settings > Clock) from UTC at nowMs.
  int64_t utcOffsetMs(int64_t nowMs) const;
  // Start of the study day: the bank's day (the account's time zone) once a snapshot of that
  // language is on the card ("" = any language's), else local midnight.
  int64_t dayStart(const std::string& language, int64_t nowMs);

  // Appends an op line to the outbox (fsync) and applies it to the view of its language.
  bool record(const std::string& line);

  // KOSync partial MD5 of a book, cached in /.linguetta/cache by path + size + mtime.
  std::string koHash(const std::string& path);

 private:
  void ensureLoaded();
  HalFs halFs;
  SyncState st;
  Settings cfg;
  bool loaded = false;
  uint32_t gen = 0;
  std::map<std::string, std::unique_ptr<LocalView>> views;
};

std::string languageName(const std::string& code);  // Portuguese name: "it" -> "Italiano"

}  // namespace linguetta
