#include "Store.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <deque>

namespace linguetta {

// ---- Fs helpers ----

bool readFile(Fs& fs, const std::string& path, std::string& out, size_t maxBytes) {
  auto f = fs.open(path, OpenMode::Read);
  if (!f) return false;
  const uint64_t size = f->size();
  if (size > maxBytes) return false;
  out.resize(static_cast<size_t>(size));
  size_t got = 0;
  while (got < out.size()) {
    const int n = f->read(&out[got], out.size() - got);
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  out.resize(got);
  return true;
}

bool writeFileAtomic(Fs& fs, const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  {
    auto f = fs.open(tmp, OpenMode::Write);
    if (!f) return false;
    if (!f->writeStr(content) || !f->sync()) {
      f.reset();
      fs.remove(tmp);
      return false;
    }
  }
  return fs.replace(tmp, path);
}

LineReader::LineReader(File& f, size_t maxLine) : file(f), maxLine(maxLine) { buf.resize(4096); }

bool LineReader::fill() {
  if (eof) return false;
  if (pos > 0) {
    // keep the unread tail at the front
    memmove(buf.data(), buf.data() + pos, len - pos);
    consumed += pos;
    len -= pos;
    pos = 0;
  }
  if (len == buf.size()) buf.resize(buf.size() * 2);
  const int n = file.read(buf.data() + len, buf.size() - len);
  if (n <= 0) {
    eof = true;
    return false;
  }
  len += static_cast<size_t>(n);
  return true;
}

bool LineReader::next(std::string& line, bool& complete) {
  lastOverlong = false;
  line.clear();
  size_t scan = pos;
  while (true) {
    const char* start = buf.data() + scan;
    const void* nl = scan < len ? memchr(start, '\n', len - scan) : nullptr;
    if (nl) {
      const size_t end = static_cast<size_t>(static_cast<const char*>(nl) - buf.data());
      lineStart = consumed + pos;
      if (end - pos > maxLine) {
        lastOverlong = true;
      } else {
        line.assign(buf.data() + pos, end - pos);
      }
      pos = end + 1;
      complete = true;
      return true;
    }
    if (len - pos > maxLine) {
      // Overlong line: drop what we have and keep scanning for its end.
      lastOverlong = true;
      lineStart = consumed + pos;
      while (true) {
        consumed += len;
        pos = len = 0;
        if (!fill()) {
          complete = false;
          return true;
        }
        const void* nl2 = memchr(buf.data(), '\n', len);
        if (nl2) {
          pos = static_cast<size_t>(static_cast<const char*>(nl2) - buf.data()) + 1;
          complete = true;
          return true;
        }
      }
    }
    const size_t scanned = len - pos;
    if (!fill()) {
      if (len > pos) {
        lineStart = consumed + pos;
        line.assign(buf.data() + pos, len - pos);
        pos = len;
        complete = false;
        return true;
      }
      return false;
    }
    scan = pos + scanned;
  }
}

// ---- state / settings ----

namespace {

const char* str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : ""; }

}  // namespace

bool loadState(Fs& fs, SyncState& out) {
  std::string text;
  if (!readFile(fs, paths::STATE, text)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
  if (str(doc["server"])[0]) out.server = str(doc["server"]);
  out.token = str(doc["token"]);
  out.tokenHint = str(doc["tokenHint"]);
  if (str(doc["deviceName"])[0]) out.deviceName = str(doc["deviceName"]);
  JsonObjectConst acc = doc["account"];
  out.account.id = str(acc["id"]);
  out.account.email = str(acc["email"]);
  out.account.nativeLanguage = str(acc["nativeLanguage"]);
  out.account.studyLanguages.clear();
  for (JsonVariantConst l : acc["studyLanguages"].as<JsonArrayConst>()) out.account.studyLanguages.push_back(str(l));
  out.language = str(doc["language"]);
  out.cursor = str(doc["cursor"]);
  out.cursors.clear();
  for (JsonPairConst kv : doc["cursors"].as<JsonObjectConst>()) {
    if (str(kv.value())[0]) out.cursors[kv.key().c_str()] = str(kv.value());
  }
  // Written before per-language banks: the cursor belonged to bank.jsonl, which
  // migrateLegacyBank() turns into a full snapshot anyway.
  if (out.cursors.empty() && !out.language.empty() && !out.cursor.empty()) out.cursors[out.language] = out.cursor;
  out.lastSyncAt = str(doc["lastSyncAt"]);
  out.lastSyncStatus = str(doc["lastSyncStatus"]);
  out.lastError = str(doc["lastError"]);
  out.failures = doc["failures"] | 0;
  out.retryAtMs = doc["retryAtMs"] | static_cast<int64_t>(0);
  return true;
}

bool saveState(Fs& fs, const SyncState& s) {
  fs.mkdirs(paths::ROOT);
  JsonDocument doc;
  doc["v"] = 1;
  doc["protocol"] = 1;
  doc["server"] = s.server;
  auto orNull = [&doc](const char* key, const std::string& v) {
    if (v.empty()) {
      doc[key] = nullptr;
    } else {
      doc[key] = v;
    }
  };
  orNull("token", s.token);
  orNull("tokenHint", s.tokenHint);
  doc["deviceName"] = s.deviceName;
  if (!s.account.id.empty()) {
    JsonObject acc = doc["account"].to<JsonObject>();
    acc["id"] = s.account.id;
    acc["email"] = s.account.email;
    acc["nativeLanguage"] = s.account.nativeLanguage;
    JsonArray langs = acc["studyLanguages"].to<JsonArray>();
    for (const auto& l : s.account.studyLanguages) langs.add(l);
  } else {
    doc["account"] = nullptr;
  }
  orNull("language", s.language);
  orNull("cursor", s.cursor);
  if (!s.cursors.empty()) {
    JsonObject c = doc["cursors"].to<JsonObject>();
    for (const auto& kv : s.cursors) c[kv.first] = kv.second;
  }
  orNull("lastSyncAt", s.lastSyncAt);
  orNull("lastSyncStatus", s.lastSyncStatus);
  orNull("lastError", s.lastError);
  if (s.failures) doc["failures"] = s.failures;
  if (s.retryAtMs) doc["retryAtMs"] = s.retryAtMs;
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');
  return writeFileAtomic(fs, paths::STATE, out);
}

Settings loadSettings(Fs& fs) {
  Settings s;
  std::string text;
  if (!readFile(fs, paths::SETTINGS_FILE, text)) return s;
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return s;
  std::string server = str(doc["server"]);
  while (!server.empty() && server.back() == '/') server.pop_back();
  if (server.rfind("https://", 0) == 0 || server.rfind("http://", 0) == 0) s.server = server;
  if (str(doc["deviceName"])[0]) s.deviceName = str(doc["deviceName"]);
  s.language = str(doc["language"]);
  if (bankPath(s.language).empty()) s.language.clear();
  return s;
}

// ---- per-language banks ----

namespace {
std::string primarySubtag(const std::string& tag) {
  std::string out;
  for (const char c : tag) {
    if (c == '-' || c == '_') break;
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return out;
}
}  // namespace

std::string blendLanguageFromName(const SyncState& st, const std::string& name) {
  const std::string native = primarySubtag(st.account.nativeLanguage);
  auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  // Scan for <2-3 letters><'-' or '_'><2-3 letters>, bounded by non-letters.
  for (size_t i = 0; i < name.size(); ++i) {
    if (!alpha(name[i]) || (i > 0 && alpha(name[i - 1]))) continue;
    size_t j = i;
    while (j < name.size() && alpha(name[j])) ++j;
    if (j - i < 2 || j - i > 3 || j >= name.size() || (name[j] != '-' && name[j] != '_')) continue;
    size_t k = j + 1;
    while (k < name.size() && alpha(name[k])) ++k;
    if (k - (j + 1) < 2 || k - (j + 1) > 3) continue;
    const std::string target = primarySubtag(name.substr(j + 1, k - j - 1));
    if (!native.empty() && target == native) continue;
    for (const auto& l : st.account.studyLanguages) {
      if (primarySubtag(l) == target) return l;
    }
  }
  return "";
}

std::vector<std::string> blendLanguages(const SyncState& st) {
  const std::string native = primarySubtag(st.account.nativeLanguage);
  std::vector<std::string> out;
  for (const auto& l : st.account.studyLanguages) {
    if (native.empty() || primarySubtag(l) != native) out.push_back(l);
  }
  return out.empty() ? st.account.studyLanguages : out;
}

std::string pickBlendLanguage(const SyncState& st, const BlendBook& bookInfo, const std::string& settingsLanguage,
                              const std::function<bool(const std::string&)>& hasLemma) {
  const auto& study = st.account.studyLanguages;
  if (!bookInfo.choice.empty() && std::find(study.begin(), study.end(), bookInfo.choice) != study.end()) {
    return bookInfo.choice;
  }
  const std::string named = blendLanguageFromName(st, bookInfo.name);
  if (!named.empty()) return named;
  const std::string native = primarySubtag(st.account.nativeLanguage);
  auto foreign = [&](const std::string& l) { return native.empty() || primarySubtag(l) != native; };
  const std::string book = primarySubtag(bookInfo.language);
  if (!book.empty()) {
    for (const auto& l : study) {
      if (primarySubtag(l) == book && foreign(l)) return l;
    }
  }
  if (hasLemma) {
    for (const auto& l : study) {
      if (hasLemma(l)) return l;
    }
  }
  if (std::find(study.begin(), study.end(), settingsLanguage) != study.end()) return settingsLanguage;
  for (const auto& l : study) {
    if (foreign(l)) return l;
  }
  if (!study.empty()) return study.front();
  if (!settingsLanguage.empty()) return settingsLanguage;
  return st.language.empty() ? "it" : st.language;
}

std::string loadBookLanguage(Fs& fs, const std::string& koHash) {
  if (koHash.empty()) return "";
  std::string text;
  if (!readFile(fs, paths::BOOKS, text)) return "";
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return "";
  const std::string l = str(doc[koHash]);
  return bankPath(l).empty() ? "" : l;
}

bool saveBookLanguage(Fs& fs, const std::string& koHash, const std::string& language) {
  if (koHash.empty() || bankPath(language).empty()) return false;
  JsonDocument doc;
  std::string text;
  if (readFile(fs, paths::BOOKS, text) && deserializeJson(doc, text) != DeserializationError::Ok) doc.clear();
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
  doc[koHash] = language;
  std::string out;
  serializeJson(doc, out);
  out.push_back('\n');
  fs.mkdirs(paths::ROOT);
  return writeFileAtomic(fs, paths::BOOKS, out);
}

std::string bankPath(const std::string& language) {
  if (language.empty() || language.size() > 16) return "";
  for (const char c : language) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return "";
  }
  return std::string(paths::BANK_DIR) + "/" + language + ".jsonl";
}

std::vector<std::string> bankLanguages(Fs& fs) {
  std::vector<std::string> out;
  for (const auto& name : fs.list(paths::BANK_DIR)) {
    if (name.size() <= 6 || name.compare(name.size() - 6, 6, ".jsonl") != 0) continue;
    const std::string lang = name.substr(0, name.size() - 6);
    if (!bankPath(lang).empty()) out.push_back(lang);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool readBankHeader(Fs& fs, const std::string& path, std::string& language) {
  auto f = fs.open(path, OpenMode::Read);
  if (!f) return false;
  LineReader reader(*f, 16384);
  std::string line;
  bool complete = false;
  if (!reader.next(line, complete) || !complete || reader.overlong()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return false;
  if (std::string(str(doc["t"])) != "header") return false;
  language = str(doc["language"]);
  return true;
}

bool migrateLegacyBank(Fs& fs, SyncState& st) {
  fs.remove(paths::LEGACY_BANK_TMP);
  if (!fs.exists(paths::LEGACY_BANK)) return false;
  std::string lang;
  const bool header = readBankHeader(fs, paths::LEGACY_BANK, lang);
  const std::string target = header ? bankPath(lang) : "";
  fs.mkdirs(paths::BANK_DIR);
  if (target.empty() || fs.exists(target) || !fs.replace(paths::LEGACY_BANK, target)) fs.remove(paths::LEGACY_BANK);
  st.cursor.clear();
  st.cursors.clear();
  saveState(fs, st);
  return true;
}

std::vector<std::string> deckLanguages(Fs& fs, const SyncState& st) {
  std::vector<std::string> out;
  auto add = [&out](const std::string& l) {
    if (!bankPath(l).empty() && std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
  };
  for (const auto& l : st.account.studyLanguages) add(l);
  for (const auto& l : bankLanguages(fs)) add(l);
  JsonDocument filter;
  filter["language"] = true;
  forEachOutbox(fs, [&](const OutboxLine& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line.json, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
      add(str(doc["language"]));
    }
    return true;
  });
  return out;
}

// ---- outbox ----

bool appendLine(Fs& fs, const std::string& path, const std::string& jsonLine) {
  if (jsonLine.find('\n') != std::string::npos) return false;
  // A previous power cut may have left a partial last line: start ours on a fresh line.
  bool needNewline = false;
  {
    auto r = fs.open(path, OpenMode::Read);
    if (r) {
      const uint64_t size = r->size();
      if (size > 0 && r->seek(size - 1)) {
        char last = 0;
        needNewline = r->read(&last, 1) == 1 && last != '\n';
      }
    }
  }
  auto f = fs.open(path, OpenMode::Append);
  if (!f) return false;
  std::string line;
  line.reserve(jsonLine.size() + 2);
  if (needNewline) line.push_back('\n');
  line += jsonLine;
  line.push_back('\n');
  return f->writeStr(line) && f->sync();
}

bool appendOutbox(Fs& fs, const std::string& jsonLine) {
  fs.mkdirs(paths::ROOT);
  return appendLine(fs, paths::OUTBOX, jsonLine);
}

void forEachOutbox(Fs& fs, const std::function<bool(const OutboxLine&)>& fn) {
  auto f = fs.open(paths::OUTBOX, OpenMode::Read);
  if (!f) return;
  LineReader reader(*f);
  std::string line;
  bool complete = false;
  OutboxLine item;
  while (reader.next(line, complete)) {
    if (!complete || reader.overlong() || line.empty()) continue;
    // Only the client id is needed here; a deckImport line can carry hundreds of rows.
    JsonDocument filter;
    filter["clientId"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, line, DeserializationOption::Filter(filter)) != DeserializationError::Ok ||
        !doc.is<JsonObject>())
      continue;
    const char* id = doc["clientId"];
    if (!id || !*id) continue;
    item.clientId = id;
    item.json = line;
    if (!fn(item)) return;
  }
}

OutboxCounts countOutbox(Fs& fs) {
  OutboxCounts c;
  forEachOutbox(fs, [&c](const OutboxLine& l) {
    if (l.json.find("\"type\":\"save\"") != std::string::npos) {
      c.saves++;
    } else if (l.json.find("\"type\":\"review\"") != std::string::npos) {
      c.reviews++;
    } else if (l.json.find("\"type\":\"seen\"") != std::string::npos) {
      c.seen++;
    } else {
      c.other++;  // extraNew, deckImport
    }
    return true;
  });
  return c;
}

bool rewriteOutboxWithout(Fs& fs, const std::set<std::string>& resolved) {
  if (resolved.empty()) return true;
  const std::string tmp = std::string(paths::OUTBOX) + ".tmp";
  {
    auto out = fs.open(tmp, OpenMode::Write);
    if (!out) return false;
    bool ok = true;
    forEachOutbox(fs, [&](const OutboxLine& l) {
      if (resolved.count(l.clientId)) return true;
      ok = out->writeStr(l.json) && out->writeStr("\n");
      return ok;
    });
    if (!ok || !out->sync()) {
      out.reset();
      fs.remove(tmp);
      return false;
    }
  }
  return fs.replace(tmp, paths::OUTBOX);
}

void appendRejected(Fs& fs, const std::vector<std::string>& lines) {
  if (lines.empty()) return;
  std::deque<std::string> keep;
  if (auto f = fs.open(paths::REJECTED, OpenMode::Read)) {
    LineReader reader(*f);
    std::string line;
    bool complete;
    while (reader.next(line, complete)) {
      if (complete && !line.empty()) keep.push_back(line);
    }
  }
  for (const auto& l : lines) keep.push_back(l);
  while (keep.size() > 100) keep.pop_front();
  std::string out;
  for (const auto& l : keep) {
    out += l;
    out.push_back('\n');
  }
  writeFileAtomic(fs, paths::REJECTED, out);
}

}  // namespace linguetta
