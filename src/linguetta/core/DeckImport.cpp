#include "DeckImport.h"

#include <ArduinoJson.h>

#include <unordered_set>

#include "Decks.h"
#include "Sha256.h"
#include "Store.h"
#include "Sync.h"
#include "Text.h"

namespace linguetta {
namespace deckimport {

namespace {

const char* stateName(State s) {
  switch (s) {
    case State::Queued:
      return "queued";
    case State::Imported:
      return "imported";
    case State::Rejected:
      return "rejected";
    default:
      return "";
  }
}

State stateFrom(const char* s) {
  const std::string v = s ? s : "";
  if (v == "queued") return State::Queued;
  if (v == "imported") return State::Imported;
  if (v == "rejected") return State::Rejected;
  return State::None;
}

// JSON bytes a row adds to the op (quotes and escapes counted generously).
size_t rowBytes(const DeckRow& r) {
  size_t n = 24;  // {"front":"","back":""},
  for (const std::string* s : {&r.front, &r.back}) {
    for (char c : *s) n += (c == '"' || c == '\\' || static_cast<uint8_t>(c) < 0x20) ? 6 : 1;
  }
  return n;
}

}  // namespace

bool readRows(Fs& fs, const std::string& deck, std::vector<DeckRow>& rows) {
  rows.clear();
  std::unordered_set<std::string> fronts;
  return readDeckRows(fs, deck, [&](const std::string& front, const std::string& back) {
    if (!fronts.insert(front).second) return false;
    // The server cuts at 2000 code points; cutting here keeps the hash of what is sent.
    rows.push_back({truncateCodePoints(front, limits::FICHA_TEXT), truncateCodePoints(back, limits::FICHA_TEXT)});
    return true;
  });
}

std::string rowsHash(const std::vector<DeckRow>& rows) {
  Sha256 h;
  for (const DeckRow& r : rows) {
    h.update(r.front);
    h.update("\t", 1);
    h.update(r.back);
    h.update("\n", 1);
  }
  return "sha256:" + h.hex();
}

std::vector<Part> split(const std::string& deck, const std::vector<DeckRow>& rows) {
  std::vector<Part> parts;
  if (rows.empty()) return parts;
  size_t bytes = 0;
  for (const DeckRow& r : rows) {
    const size_t b = rowBytes(r);
    if (parts.empty() || parts.back().rows.size() >= limits::DECK_ROWS || bytes + b > MAX_OP_BYTES) {
      parts.emplace_back();
      bytes = 0;
    }
    parts.back().rows.push_back(r);
    bytes += b;
  }
  const std::string hash = rowsHash(rows);
  const std::string base = deckImportName(deck);
  if (parts.size() == 1) {
    parts[0].name = base;
    parts[0].hash = hash;
    return parts;
  }
  for (size_t i = 0; i < parts.size(); i++) {
    const std::string suffix = " " + std::to_string(i + 1);
    parts[i].name = truncateCodePoints(base, limits::DECK_NAME - suffix.size()) + suffix;
    parts[i].hash = hash + "." + std::to_string(i + 1);
  }
  return parts;
}

std::map<std::string, Entry> load(Fs& fs) {
  std::map<std::string, Entry> out;
  std::string text;
  if (!readFile(fs, FILE_PATH, text)) return out;
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return out;
  for (JsonPairConst kv : doc["files"].as<JsonObjectConst>()) {
    Entry e;
    e.state = stateFrom(kv.value()["state"]);
    e.hash = kv.value()["hash"] | "";
    for (JsonVariantConst v : kv.value()["ops"].as<JsonArrayConst>()) e.ops.push_back(v | "");
    for (JsonVariantConst v : kv.value()["deckIds"].as<JsonArrayConst>()) e.deckIds.push_back(v | "");
    out[kv.key().c_str()] = std::move(e);
  }
  return out;
}

bool save(Fs& fs, const std::map<std::string, Entry>& entries) {
  JsonDocument doc;
  JsonObject files = doc["files"].to<JsonObject>();
  for (const auto& kv : entries) {
    JsonObject o = files[kv.first].to<JsonObject>();
    o["state"] = stateName(kv.second.state);
    o["hash"] = kv.second.hash;
    JsonArray ops = o["ops"].to<JsonArray>();
    for (const auto& id : kv.second.ops) ops.add(id);
    if (!kv.second.deckIds.empty()) {
      JsonArray ids = o["deckIds"].to<JsonArray>();
      for (const auto& id : kv.second.deckIds) ids.add(id);
    }
  }
  std::string text;
  serializeJson(doc, text);
  fs.mkdirs(paths::ROOT);
  return writeFileAtomic(fs, FILE_PATH, text);
}

int queue(Fs& fs, const std::string& language, int64_t nowMs, const std::function<std::string()>& newId) {
  if (language.empty() || !newId) return 0;
  std::map<std::string, Entry> entries = load(fs);
  int queued = 0;
  for (const std::string& deck : listTsvDecks(fs)) {
    if (entries.count(deck) && entries[deck].state != State::None) continue;  // each file goes once
    std::vector<DeckRow> rows;
    if (!readRows(fs, deck, rows) || rows.empty()) continue;
    Entry e;
    e.hash = rowsHash(rows);
    bool ok = true;
    for (const Part& part : split(deck, rows)) {
      const std::string id = newId();
      const std::string line = buildDeckImport(id, nowMs, language, part.name, part.hash, part.rows);
      if (line.empty() || !appendOutbox(fs, line)) {
        ok = false;
        break;
      }
      e.ops.push_back(id);
    }
    // A part that could not be written leaves the parts before it queued (the server then
    // rejects nothing, the deck is just shorter); with none written the file is tried again.
    (void)ok;
    if (e.ops.empty()) continue;
    e.state = State::Queued;
    entries[deck] = std::move(e);
    queued++;
  }
  if (queued) save(fs, entries);
  return queued;
}

int settle(Fs& fs, const std::map<std::string, DeckImportResult>& results) {
  if (results.empty()) return 0;
  std::map<std::string, Entry> entries = load(fs);
  int imported = 0;
  bool changed = false;
  for (auto& kv : entries) {
    Entry& e = kv.second;
    if (e.state != State::Queued) continue;
    bool all = true, failed = false;
    std::vector<std::string> deckIds;
    for (const auto& id : e.ops) {
      const auto it = results.find(id);
      if (it == results.end()) {
        all = false;
        continue;
      }
      if (!it->second.ok) failed = true;
      if (!it->second.deckId.empty()) deckIds.push_back(it->second.deckId);
    }
    if (failed) {
      e.state = State::Rejected;
      changed = true;
    } else if (all) {
      e.state = State::Imported;
      e.deckIds = deckIds;
      changed = true;
      imported++;
    }
  }
  if (changed) save(fs, entries);
  return imported;
}

std::set<std::string> imported(Fs& fs) {
  std::set<std::string> out;
  for (const auto& kv : load(fs)) {
    if (kv.second.state == State::Imported) out.insert(kv.first);
  }
  return out;
}

}  // namespace deckimport
}  // namespace linguetta
