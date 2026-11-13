#pragma once
// Word Lens: a short gloss over the words of the book that are in the bank (Kindle Word Wise,
// glossed from the bank instead of a dictionary pack). Platform-free part: the help level, the
// matching keys of page words, the gloss index built from the local view, and the once-a-day
// `seen` bookkeeping. Drawing and taps live in src/linguetta/reader/WordLensReader.cpp.

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Fs.h"
#include "LocalView.h"

namespace linguetta::lens {

constexpr const char* STATE_FILE = "/.linguetta/wordlens.json";
constexpr size_t MAX_WORDS = 4;           // longest item matched as a sequence ("il mare")
constexpr float MIN_LINE_COMPRESSION = 1.6f;  // line spacing floor while Word Lens is active

// How much help: which items get a gloss. New / learning / relearning cards (and offline
// saves with no card yet) always do; reviewed cards by stability; Conhecido never.
enum class Level : uint8_t { Off = 0, Muita = 1, Media = 2, Pouca = 3 };
constexpr Level DEFAULT_LEVEL = Level::Media;
constexpr double MEDIA_MAX_STABILITY = 30.0;  // days
constexpr double POUCA_MAX_STABILITY = 7.0;

const char* levelLabel(Level level);  // "Desligado", "Muita", "Média", "Pouca"
const char* levelCode(Level level);   // "off", "muita", "media", "pouca"
bool parseLevel(const std::string& code, Level& out);
bool shows(const ViewEntry& e, Level level);

// One page word reduced for matching.
struct WordKey {
  std::string key;      // lower-case, edge punctuation stripped, ’ -> '; "" if no letters
  std::string elided;   // part after the last apostrophe ("un'onda" -> "onda"), or ""
  bool breakBefore = false;  // leading punctuation: a sequence cannot run into this word
  bool breakAfter = false;   // trailing punctuation: a sequence cannot run past this word
};
WordKey wordKey(const std::string& text);
// A bank lemma / form as an index key: every space-separated part through wordKey().key.
std::string itemKey(const std::string& text);

struct Gloss {
  int entry = -1;     // LocalView index
  std::string lemma;  // as written
  std::string text;   // the sense shown over the word
};

struct Token {
  WordKey word;
  bool paragraphStart = false;
};

struct Span {
  size_t first = 0, count = 0;  // tokens covered
  int gloss = -1;
};

class Index {
 public:
  // Glosses for the view's items at `level`: the item sense, else its latest encounter's
  // sense; no sense at all: no gloss. Keys are the lemma, the snapshot forms and every
  // encounter form, local (outbox) ones included.
  void build(Fs& fs, const LocalView& view, Level level);
  void clear();
  bool empty() const { return glosses.empty(); }
  size_t size() const { return glosses.size(); }
  const Gloss& gloss(int i) const { return glosses[static_cast<size_t>(i)]; }
  int find(const std::string& key) const;  // gloss index or -1

  // Longest match first, left to right; sequences stay within a paragraph and do not cross
  // punctuation.
  std::vector<Span> match(const std::vector<Token>& tokens) const;

 private:
  void add(const std::string& key, int gloss);
  std::vector<Gloss> glosses;
  std::unordered_map<std::string, int> byKey;
  size_t longest = 1;
};

// Level plus the `seen` ops already recorded today (one per item per book per day).
struct State {
  Level level = DEFAULT_LEVEL;
  int64_t day = 0;              // study day the set belongs to
  std::set<std::string> seen;   // "<book>|<item key>"

  // True the first time an item is seen in `book` on `day` (the set resets on a new day).
  bool markSeen(int64_t dayStart, const std::string& book, const std::string& key);
};
State loadState(Fs& fs);  // defaults when missing or invalid
bool saveState(Fs& fs, const State& s);

}  // namespace linguetta::lens
