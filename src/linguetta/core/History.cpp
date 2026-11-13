#include "History.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>

#include "Ops.h"
#include "Store.h"
#include "Text.h"

namespace linguetta {

namespace {
constexpr size_t MAX_LINE = 64 * 1024;

bool parseSession(const std::string& line, HistorySession& out, bool withCards) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
  const char* deck = doc["deck"];
  out.startMs = doc["start"] | static_cast<int64_t>(0);
  if (!deck || out.startMs <= 0) return false;
  out.deck = deck;
  out.durationMs = doc["ms"] | static_cast<int64_t>(0);
  JsonArrayConst cards = doc["cards"].as<JsonArrayConst>();
  out.cardCount = static_cast<int>(cards.size());
  out.cards.clear();
  if (withCards) {
    for (JsonObjectConst c : cards) {
      HistoryCard hc;
      hc.lemma = c["l"] | "";
      hc.sense = c["s"] | "";
      hc.grade = c["g"] | 0;
      out.cards.push_back(std::move(hc));
    }
  }
  return true;
}
}  // namespace

bool appendHistory(Fs& fs, const HistorySession& s) {
  JsonDocument doc;
  doc["deck"] = truncateCodePoints(s.deck, 120);
  doc["start"] = s.startMs;
  doc["ms"] = s.durationMs;
  JsonArray cards = doc["cards"].to<JsonArray>();
  for (size_t i = 0; i < s.cards.size() && i < HISTORY_MAX_CARDS; i++) {
    JsonObject c = cards.add<JsonObject>();
    c["l"] = truncateCodePoints(s.cards[i].lemma, limits::LEMMA);
    c["s"] = truncateCodePoints(s.cards[i].sense, 80);
    c["g"] = s.cards[i].grade;
  }
  std::string line;
  serializeJson(doc, line);
  fs.mkdirs(paths::ROOT);
  return appendLine(fs, paths::HISTORY, line);
}

std::vector<HistorySession> loadHistory(Fs& fs, int64_t fromMs, int64_t toMs) {
  std::vector<HistorySession> out;
  auto f = fs.open(paths::HISTORY, OpenMode::Read);
  if (!f) return out;
  LineReader reader(*f, MAX_LINE);
  std::string line;
  bool complete = false;
  while (reader.next(line, complete)) {
    if (!complete || reader.overlong() || line.empty()) continue;
    HistorySession s;
    if (!parseSession(line, s, false)) continue;
    if (s.startMs < fromMs || s.startMs >= toMs) continue;
    s.offset = static_cast<int64_t>(reader.lineOffset());
    out.push_back(std::move(s));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const HistorySession& a, const HistorySession& b) { return a.startMs > b.startMs; });
  return out;
}

bool loadHistorySession(Fs& fs, int64_t offset, HistorySession& out) {
  if (offset < 0) return false;
  auto f = fs.open(paths::HISTORY, OpenMode::Read);
  if (!f || !f->seek(static_cast<uint64_t>(offset))) return false;
  LineReader reader(*f, MAX_LINE);
  std::string line;
  bool complete = false;
  if (!reader.next(line, complete) || !complete || reader.overlong()) return false;
  if (!parseSession(line, out, true)) return false;
  out.offset = offset;
  return true;
}

std::string sessionDayLabel(int64_t startMs, int64_t nowMs, int64_t utcOffsetMs) {
  const CivilTime s = toCivil(startMs + utcOffsetMs);
  const int64_t startDay = fromCivil(s.year, s.month, s.day);
  char time[8];
  snprintf(time, sizeof time, "%02d:%02d", s.hour, s.minute);
  std::string day;
  if (nowMs > 0) {
    const CivilTime n = toCivil(nowMs + utcOffsetMs);
    const int64_t today = fromCivil(n.year, n.month, n.day);
    if (startDay == today) day = "Hoje";
    if (startDay == today - 86400000LL) day = "Ontem";
  }
  if (day.empty()) {
    char buf[12];
    snprintf(buf, sizeof buf, "%02d/%02d", s.day, s.month);
    day = buf;
  }
  return day + " \xC2\xB7 " + time;
}

std::string monthLabel(int year, int month) {
  static const char* const NAMES[12] = {"JANEIRO", "FEVEREIRO", "MAR\xC3\x87O", "ABRIL",    "MAIO",     "JUNHO",
                                        "JULHO",   "AGOSTO",    "SETEMBRO",       "OUTUBRO", "NOVEMBRO", "DEZEMBRO"};
  if (month < 1 || month > 12) return std::to_string(year);
  return std::string(NAMES[month - 1]) + " " + std::to_string(year);
}

int64_t monthStartMs(int year, int month, int64_t utcOffsetMs) {
  while (month < 1) {
    month += 12;
    year--;
  }
  while (month > 12) {
    month -= 12;
    year++;
  }
  return fromCivil(year, month, 1) - utcOffsetMs;
}

}  // namespace linguetta
