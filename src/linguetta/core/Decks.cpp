#include "Decks.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <functional>

#include "Store.h"
#include "Text.h"

namespace linguetta {

bool parseTsvLine(const std::string& raw, std::string& front, std::string& back) {
  std::string line = raw;
  if (line.size() >= 3 && static_cast<uint8_t>(line[0]) == 0xEF && static_cast<uint8_t>(line[1]) == 0xBB &&
      static_cast<uint8_t>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line.empty() || line[0] == '#') return false;
  const size_t tab = line.find('\t');
  if (tab == std::string::npos) return false;
  front = trim(line.substr(0, tab));
  const size_t next = line.find('\t', tab + 1);
  back = trim(line.substr(tab + 1, next == std::string::npos ? std::string::npos : next - tab - 1));
  return !front.empty();
}

// One CSV record per line: comma-separated, fields optionally in double quotes with "" as an
// escaped quote. Only the first two fields are used. Quoted newlines are not supported.
bool parseCsvLine(const std::string& raw, std::string& front, std::string& back) {
  std::string line = raw;
  if (line.size() >= 3 && static_cast<uint8_t>(line[0]) == 0xEF && static_cast<uint8_t>(line[1]) == 0xBB &&
      static_cast<uint8_t>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line.empty() || line[0] == '#') return false;
  std::vector<std::string> fields(1);
  bool quoted = false;
  for (size_t i = 0; i < line.size() && fields.size() <= 2; i++) {
    const char ch = line[i];
    if (quoted) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          fields.back() += '"';
          i++;
        } else {
          quoted = false;
        }
      } else {
        fields.back() += ch;
      }
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == ',') {
      fields.emplace_back();
    } else {
      fields.back() += ch;
    }
  }
  if (fields.size() < 2) return false;
  front = trim(fields[0]);
  back = trim(fields[1]);
  return !front.empty();
}

bool parseDeckLine(const std::string& line, bool csv, std::string& front, std::string& back) {
  return csv ? parseCsvLine(line, front, back) : parseTsvLine(line, front, back);
}

std::vector<std::string> listTsvDecks(Fs& fs) {
  std::vector<std::string> out;
  for (const auto& file : fs.list(paths::TSV_DIR)) {
    if (file.size() <= 4 || file[0] == '.') continue;
    const std::string ext = lowerKey(file.substr(file.size() - 4));
    if (ext != ".tsv" && ext != ".csv") continue;
    out.push_back(file.substr(0, file.size() - 4));
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lowerKey(a) < lowerKey(b); });
  // A deck present as both .tsv and .csv is listed once (the .tsv wins when loading).
  out.erase(std::unique(out.begin(), out.end(),
                        [](const std::string& a, const std::string& b) { return lowerKey(a) == lowerKey(b); }),
            out.end());
  return out;
}

std::string TsvDeck::tsvPath(const std::string& name) { return std::string(paths::TSV_DIR) + "/" + name + ".tsv"; }
std::string TsvDeck::csvPath(const std::string& name) { return std::string(paths::TSV_DIR) + "/" + name + ".csv"; }
std::string TsvDeck::logPath(const std::string& name) {
  return std::string(paths::DECK_STATE_DIR) + "/" + name + ".jsonl";
}

bool readDeckRows(Fs& fs, const std::string& name, const std::function<bool(const std::string&, const std::string&)>& row) {
  bool csv = false;
  auto f = fs.open(TsvDeck::tsvPath(name), OpenMode::Read);
  if (!f) {
    f = fs.open(TsvDeck::csvPath(name), OpenMode::Read);
    csv = true;
  }
  if (!f) return false;
  LineReader reader(*f, 4096);
  std::string line, front, back;
  bool complete = false;
  bool first = true;
  size_t taken = 0;
  while (taken < TSV_MAX_CARDS && reader.next(line, complete)) {
    if (reader.overlong() || !parseDeckLine(line, csv, front, back)) continue;
    const bool header = first && lowerKey(front) == "front" && lowerKey(back) == "back";
    first = false;
    if (header) continue;
    if (row(front, back)) taken++;
  }
  return true;
}

bool TsvDeck::load(Fs& fs, const std::string& name) {
  deckName = name;
  cards.clear();
  byFront.clear();
  const bool ok = readDeckRows(fs, name, [this](const std::string& front, const std::string& back) {
    if (byFront.count(front)) return false;
    byFront.emplace(front, cards.size());
    TsvCard c;
    c.front = front;
    c.back = back;
    cards.push_back(std::move(c));
    return true;
  });
  if (!ok) return false;
  auto log = fs.open(logPath(name), OpenMode::Read);
  if (!log) return true;
  LineReader reader(*log, 4096);
  std::string line;
  bool complete = false;
  while (reader.next(line, complete)) {
    if (!complete || reader.overlong() || line.empty()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
    const char* front = doc["f"];
    const int g = doc["g"] | 0;
    const int64_t at = doc["at"] | static_cast<int64_t>(0);
    if (!front || g < 1 || g > 4 || at <= 0) continue;
    const auto it = byFront.find(front);
    if (it != byFront.end()) apply(it->second, g, at);  // a card removed from the TSV is ignored
  }
  return true;
}

void TsvDeck::apply(size_t i, int rating, int64_t atMs) {
  TsvCard& c = cards[i];
  if (c.firstReviewMs < 0) {
    c.firstReviewMs = atMs;
    c.card = fsrs::newCard(atMs);
  }
  c.card = fsrs::review(c.card, seed(i), rating, atMs, retention);
}

int TsvDeck::newDoneToday(int64_t dayStartMs) const {
  int n = 0;
  for (const auto& c : cards) {
    if (c.firstReviewMs >= dayStartMs) n++;
  }
  return n;
}

std::vector<int> TsvDeck::queue(int64_t nowMs, int64_t dayStartMs, int newPerDay) const {
  std::vector<int> due, fresh;
  const int allowance = std::max(0, newPerDay - newDoneToday(dayStartMs));
  const int64_t dayEnd = dayStartMs + fsrs::MS_PER_DAY;
  for (size_t i = 0; i < cards.size(); i++) {
    if (cards[i].firstReviewMs >= 0) {
      if (fsrs::isDue(cards[i].card, nowMs, dayEnd)) due.push_back(static_cast<int>(i));
    } else if (static_cast<int>(fresh.size()) < allowance) {
      fresh.push_back(static_cast<int>(i));
    }
  }
  std::stable_sort(due.begin(), due.end(), [this](int a, int b) { return cards[a].card.dueMs < cards[b].card.dueMs; });
  due.insert(due.end(), fresh.begin(), fresh.end());
  return due;
}

bool TsvDeck::grade(Fs& fs, size_t i, int rating, int64_t nowMs) {
  if (i >= cards.size() || rating < 1 || rating > 4 || nowMs <= 0) return false;
  JsonDocument doc;
  doc["f"] = cards[i].front;
  doc["g"] = rating;
  doc["at"] = nowMs;
  std::string line;
  serializeJson(doc, line);
  fs.mkdirs(paths::DECK_STATE_DIR);
  if (!appendLine(fs, logPath(deckName), line)) return false;
  apply(i, rating, nowMs);
  return true;
}

}  // namespace linguetta
