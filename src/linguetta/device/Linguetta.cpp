#include "Linguetta.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#ifndef SIMULATOR
#include <esp_random.h>
#endif
#include <sys/time.h>

#include <cstring>
#include <ctime>

#include "linguetta/core/Text.h"

namespace linguetta {

namespace {
constexpr const char* KOHASH_CACHE = "/.linguetta/cache/kohash.jsonl";
}

Service& Service::get() {
  static Service instance;
  return instance;
}

void Service::ensureLoaded() {
  if (loaded) return;
  loaded = true;
  halFs.mkdirs(paths::ROOT);
  cfg = loadSettings(halFs);
  loadState(halFs, st);
  if (migrateLegacyBank(halFs, st)) LOG_INF("LGT", "Single bank.jsonl moved to bank/<language>.jsonl");
  // A server override that differs from the paired one means a different account space:
  // treat the device as unpaired (the token belongs to the other server).
  // The old Vercel domain is the same deployment (rebrand), so moving off it keeps the pairing.
  if (st.server == "https://linguarudo-app.vercel.app" && cfg.server == DEFAULT_SERVER) st.server = cfg.server;
  if (st.server != cfg.server) {
    LOG_INF("LGT", "Server changed (%s -> %s); unpaired", st.server.c_str(), cfg.server.c_str());
    st.server = cfg.server;
    st.token.clear();
    st.tokenHint.clear();
    st.cursor.clear();
    st.cursors.clear();
  }
  if (st.deviceName.empty() || st.deviceName == "X4 Pro") st.deviceName = cfg.deviceName;
}

SyncState& Service::state() {
  ensureLoaded();
  return st;
}

bool Service::saveState() {
  ensureLoaded();
  return linguetta::saveState(halFs, st);
}

const Settings& Service::settings() {
  ensureLoaded();
  return cfg;
}

std::vector<std::string> Service::languages() {
  ensureLoaded();
  return deckLanguages(halFs, st);
}

std::string Service::deckName(const std::string& language) {
  LocalView& v = view(language);
  if (!v.header().deckName.empty()) return v.header().deckName;
  return "Linguetta \xC2\xB7 " + languageName(language);
}

std::string Service::blendLanguage(const std::string& lemma, const std::string& koHash, const std::string& bookName,
                                   const std::string& bookLanguage) {
  ensureLoaded();
  const BlendBook book{bookLanguage, bookName, loadBookLanguage(halFs, koHash)};
  return pickBlendLanguage(st, book, cfg.language,
                           [this, &lemma](const std::string& l) { return view(l).find(lemma) >= 0; });
}

bool Service::setBookLanguage(const std::string& koHash, const std::string& language) {
  return saveBookLanguage(halFs, koHash, language);
}

std::vector<std::string> Service::blendLanguages() {
  ensureLoaded();
  return linguetta::blendLanguages(st);
}

LocalView& Service::view(const std::string& language) {
  ensureLoaded();
  auto& slot = views[language];
  if (!slot) {
    const unsigned long t0 = millis();
    slot.reset(new LocalView());
    slot->load(halFs, language);
    LOG_INF("LGT", "Local view %s: %u items in %lu ms", language.c_str(), static_cast<unsigned>(slot->size()),
            millis() - t0);
  }
  return *slot;
}

int64_t Service::nowMs() const {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const int64_t ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  return ms >= MIN_TRUSTED_MS ? ms : 0;
}

int64_t Service::utcOffsetMs(int64_t nowMs) const {
  const time_t t = static_cast<time_t>(nowMs / 1000);
  struct tm local {};
  if (!localtime_r(&t, &local)) return 0;
  const int64_t wall = fromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) +
                       (local.tm_hour * 3600LL + local.tm_min * 60LL + local.tm_sec) * 1000;
  return wall - static_cast<int64_t>(t) * 1000;
}

int64_t Service::dayStart(const std::string& language, int64_t nowMs) {
  for (const auto& l : language.empty() ? languages() : std::vector<std::string>{language}) {
    LocalView& v = view(l);
    if (v.header().present && v.header().dayStartMs > 0) return v.dayStart(nowMs);
  }
  const int64_t off = utcOffsetMs(nowMs);
  const int64_t local = nowMs + off;
  return local - (local % fsrs::MS_PER_DAY) - off;
}

std::string Service::newId() const {
  uint8_t bytes[16];
#ifdef SIMULATOR
  for (auto& b : bytes) b = static_cast<uint8_t>(rand());
#else
  esp_fill_random(bytes, sizeof(bytes));
#endif
  return uuidFromBytes(bytes);
}

bool Service::record(const std::string& line) {
  if (line.empty()) return false;
  ensureLoaded();
  if (!appendOutbox(halFs, line)) {
    LOG_ERR("LGT", "Outbox append failed");
    return false;
  }
  // Only a loaded view needs the op; an unloaded one reads it from the outbox later.
  JsonDocument filter;
  filter["language"] = true;
  filter["type"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, line, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
    if (strcmp(doc["type"] | "", "seen") != 0) gen++;
    const auto it = views.find(doc["language"] | "");
    if (it != views.end() && it->second) it->second->applyOp(line);
  }
  return true;
}

std::string Service::koHash(const std::string& path) {
  uint64_t size = 0;
  uint32_t mtime = 0;
  if (!halFs.stat(path, size, mtime)) return "";
  if (auto f = halFs.open(KOHASH_CACHE, OpenMode::Read)) {
    LineReader reader(*f, 1024);
    std::string line;
    bool complete;
    while (reader.next(line, complete)) {
      if (!complete) continue;
      JsonDocument doc;
      if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
      if (path == (doc["p"] | "") && (doc["s"] | static_cast<uint64_t>(0)) == size && (doc["m"] | 0u) == mtime) {
        return doc["h"] | "";
      }
    }
  }
  const std::string hash = KOReaderDocumentId::calculate(path);
  if (hash.size() != 32) return "";
  halFs.mkdirs(paths::CACHE);
  JsonDocument doc;
  doc["p"] = path;
  doc["s"] = size;
  doc["m"] = mtime;
  doc["h"] = hash;
  std::string line;
  serializeJson(doc, line);
  line.push_back('\n');
  if (auto f = halFs.open(KOHASH_CACHE, OpenMode::Append)) f->writeStr(line);
  return hash;
}

std::string languageName(const std::string& code) {
  static const struct {
    const char* code;
    const char* name;
  } NAMES[] = {{"it", "Italiano"},          {"en", "Ingl\xC3\xAAs"},  {"es", "Espanhol"},
               {"fr", "Franc\xC3\xAAs"},     {"de", "Alem\xC3\xA3o"},  {"ja", "Japon\xC3\xAAs"},
               {"pt", "Portugu\xC3\xAAs"},   {"zh", "Chin\xC3\xAAs"},  {"ko", "Coreano"},
               {"ru", "Russo"},              {"nl", "Holand\xC3\xAAs"}};
  for (const auto& n : NAMES) {
    if (code == n.code) return n.name;
  }
  return code;
}

}  // namespace linguetta
