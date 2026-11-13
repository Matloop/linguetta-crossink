#include "Ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Text.h"

namespace linguetta {

namespace {

std::string cut(const std::string& s, size_t max) { return truncateCodePoints(trim(s), max); }

void setIf(JsonObject o, const char* key, const std::string& v) {
  if (!v.empty()) o[key] = v;
}

std::string toLine(const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool isHex32(const std::string& s) {
  if (s.size() != 32) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

std::string buildSave(const std::string& clientId, int64_t atMs, const SaveInput& in) {
  const std::string lemma = cut(in.lemma, limits::LEMMA);
  if (lemma.empty()) return "";
  JsonDocument doc;
  doc["clientId"] = clientId;
  doc["type"] = "save";
  doc["at"] = formatIso(atMs);
  doc["language"] = in.language;
  doc["lemma"] = lemma;
  JsonObject root = doc.as<JsonObject>();
  setIf(root, "sense", cut(in.sense, limits::SENSE));
  setIf(root, "note", cut(in.note, limits::NOTE));
  setIf(root, "ipa", cut(in.ipa, limits::IPA));
  setIf(root, "pos", cut(in.pos, limits::POS));

  JsonObject enc = doc["encounter"].to<JsonObject>();
  enc["form"] = cut(in.form.empty() ? in.lemma : in.form, limits::FORM);
  enc["sentence"] = cut(in.sentence, limits::SENTENCE);
  setIf(enc, "sense", cut(in.encounterSense, limits::SENSE));
  const std::string chapter = cut(in.chapter, limits::CHAPTER);
  if (!chapter.empty() || in.spine >= 0 || in.percent >= 0) {
    JsonObject pos = enc["position"].to<JsonObject>();
    setIf(pos, "chapter", chapter);
    if (in.spine >= 0) pos["spine"] = in.spine;
    if (in.percent >= 0) {
      double p = std::round(std::min(100.0, in.percent) * 10.0) / 10.0;
      pos["percent"] = p;
    }
  }

  JsonObject work = doc["work"].to<JsonObject>();
  work["kind"] = "livro";
  if (isHex32(in.koHash)) work["koHash"] = in.koHash;
  std::string title = cut(in.title, limits::TITLE);
  work["title"] = title.empty() ? std::string("Livro") : title;
  setIf(work, "author", cut(in.author, limits::AUTHOR));
  if (in.workLanguage.size() >= 2 && in.workLanguage.size() <= 3) work["language"] = in.workLanguage;
  return toLine(doc);
}

std::string buildReview(const std::string& clientId, int64_t atMs, const std::string& language,
                        const std::string& cardId, const std::string& saveClientId, int rating, int64_t durationMs) {
  JsonDocument doc;
  doc["clientId"] = clientId;
  doc["type"] = "review";
  doc["at"] = formatIso(atMs);
  doc["language"] = language;
  if (!cardId.empty()) {
    doc["flashcardId"] = cardId;
  } else {
    doc["saveClientId"] = saveClientId;
  }
  doc["rating"] = rating;
  if (durationMs >= 0) doc["durationMs"] = std::min<int64_t>(durationMs, 3600000);
  return toLine(doc);
}

std::string buildSeen(const std::string& clientId, int64_t atMs, const std::string& language, const std::string& lemma) {
  JsonDocument doc;
  doc["clientId"] = clientId;
  doc["type"] = "seen";
  doc["at"] = formatIso(atMs);
  doc["language"] = language;
  doc["lemma"] = cut(lemma, limits::LEMMA);
  return toLine(doc);
}

std::string buildExtraNew(const std::string& clientId, int64_t atMs, const std::string& language, int n) {
  JsonDocument doc;
  doc["clientId"] = clientId;
  doc["type"] = "extraNew";
  doc["at"] = formatIso(atMs);
  doc["language"] = language;
  doc["n"] = std::min(std::max(n, 1), limits::EXTRA_NEW);
  return toLine(doc);
}

std::string deckImportName(const std::string& name) {
  std::string out = trim(name);
  for (size_t at; (at = out.find("::")) != std::string::npos;) out.replace(at, 2, " ");
  out = trim(cut(out, limits::DECK_NAME));
  return out.empty() ? std::string("Deck") : out;
}

std::string buildDeckImport(const std::string& clientId, int64_t atMs, const std::string& language,
                            const std::string& name, const std::string& hash, const std::vector<DeckRow>& rows) {
  if (hash.empty() || hash.size() > limits::DECK_HASH) return "";
  JsonDocument doc;
  doc["clientId"] = clientId;
  doc["type"] = "deckImport";
  doc["at"] = formatIso(atMs);
  doc["language"] = language;
  doc["name"] = deckImportName(name);
  doc["hash"] = hash;
  JsonArray arr = doc["rows"].to<JsonArray>();
  size_t n = 0;
  for (const DeckRow& r : rows) {
    const std::string front = trim(r.front);
    if (front.empty()) continue;
    if (++n > limits::DECK_ROWS) return "";
    JsonObject o = arr.add<JsonObject>();
    o["front"] = cut(front, limits::FICHA_TEXT);
    const std::string back = trim(r.back);
    if (!back.empty()) o["back"] = cut(back, limits::FICHA_TEXT);
  }
  if (n == 0) return "";
  return toLine(doc);
}

bool cardFromJson(JsonObjectConst o, CardState& c, std::string& id, int64_t& createdMs) {
  if (o.isNull()) return false;
  id = o["id"] | "";
  createdMs = parseIsoOr(o["createdAt"], 0);
  if (!parseIso(o["dueAt"] | "", c.dueMs)) return false;
  c.stability = o["stability"] | 0.0;
  c.stabilityFast = o["stabilityFast"] | 0.0;
  c.difficulty = o["difficulty"] | 0.0;
  c.state = o["state"] | 0;
  c.setScheduleStatus(o["scheduleStatus"] | "new");
  c.lastReviewMs = parseIsoOr(o["lastReviewAt"], -1);
  c.reps = o["reps"] | 0;
  c.lapses = o["lapses"] | 0;
  c.scheduledDays = o["scheduledDays"] | 0.0;
  c.learningStep = o["learningStep"] | 0;
  return true;
}

}  // namespace linguetta
