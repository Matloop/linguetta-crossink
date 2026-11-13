#include "Sync.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

#include "DeckImport.h"
#include "LocalView.h"
#include "Ops.h"
#include "Text.h"

namespace linguetta {

namespace {

constexpr size_t MAX_SNAPSHOT_LINE = 16384;   // section 7
constexpr int64_t BACKOFF_MS[3] = {30000, 120000, 600000};  // section 2: 30 s, 2 min, 10 min
constexpr int DEFAULT_RETRY_AFTER_S = 60;

const char* str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : ""; }

// A response JSON that carries a protocol other than 1 is not ours to read.
bool protocolOk(JsonVariantConst doc) { return doc["protocol"].isNull() || (doc["protocol"] | 0) == 1; }

SyncError deviceError(SyncErr kind, const char* code) {
  SyncError e;
  e.kind = kind;
  e.code = code;
  return e;
}

void readAccount(JsonObjectConst acc, Account& out) {
  out.id = str(acc["id"]);
  out.email = str(acc["email"]);
  out.nativeLanguage = str(acc["nativeLanguage"]);
  out.studyLanguages.clear();
  for (JsonVariantConst l : acc["studyLanguages"].as<JsonArrayConst>()) {
    if (str(l)[0]) out.studyLanguages.push_back(str(l));
  }
}

// Calls fn(t, id, line) for every complete line of a JSONL file.
template <typename Fn>
bool forEachTypedLine(Fs& fs, const std::string& path, Fn fn) {
  auto f = fs.open(path, OpenMode::Read);
  if (!f) return false;
  LineReader reader(*f, MAX_SNAPSHOT_LINE);
  std::string line;
  bool complete = false;
  JsonDocument filter;
  filter["t"] = true;
  filter["id"] = true;
  while (reader.next(line, complete)) {
    if (!complete || reader.overlong() || line.empty()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    if (!fn(std::string(str(doc["t"])), std::string(str(doc["id"])), line)) return false;
  }
  return true;
}

}  // namespace

// ---- errors ----

std::string errorText(const SyncError& e) {
  switch (e.kind) {
    case SyncErr::None:
      return "";
    case SyncErr::NotPaired:
      return "Aparelho n\xC3\xA3o pareado.";
    case SyncErr::Backoff:
      return "O servidor pediu uma pausa. Tente de novo daqui a pouco.";
    case SyncErr::Network:
      return "Sem conex\xC3\xA3o com o servidor.";
    case SyncErr::Unauthorized:
      return "Pareie de novo.";
    case SyncErr::Protocol:
      return "Atualize o firmware.";
    case SyncErr::RateLimited:
      return "Muitas tentativas. Espere um pouco.";
    case SyncErr::Server:
      return "O servidor falhou. Tente de novo mais tarde.";
    case SyncErr::BadRequest:
      return e.message.empty() ? "O servidor recusou o pedido." : e.message;
    case SyncErr::PageExpired:
      return "O download do banco expirou. Tente de novo.";
    case SyncErr::PayloadTooLarge:
      return "Uma opera\xC3\xA7\xC3\xA3o \xC3\xA9 grande demais para enviar.";
    case SyncErr::BadResponse:
      return "Resposta inesperada do servidor.";
    case SyncErr::Disk:
      return "N\xC3\xA3o deu para gravar no cart\xC3\xA3o.";
  }
  return "";
}

SyncError errorFromResponse(const HttpResponse& r, int64_t nowMs) {
  SyncError e;
  e.status = r.status;
  if (r.status < 0) return deviceError(SyncErr::Network, "network");
  JsonDocument doc;
  int bodyRetry = -1;
  if (!r.body.empty() && deserializeJson(doc, r.body) == DeserializationError::Ok && doc.is<JsonObject>()) {
    e.code = str(doc["code"]);
    e.message = str(doc["message"]);
    bodyRetry = doc["retryAfter"] | -1;
  }
  if (r.status == 401) {
    e.kind = SyncErr::Unauthorized;
  } else if (r.status == 426) {
    e.kind = SyncErr::Protocol;
  } else if (r.status == 429) {
    e.kind = SyncErr::RateLimited;
    const int s = r.retryAfterS >= 0 ? r.retryAfterS : (bodyRetry >= 0 ? bodyRetry : DEFAULT_RETRY_AFTER_S);
    e.retryAtMs = nowMs + static_cast<int64_t>(s) * 1000;
  } else if (r.status >= 500) {
    e.kind = SyncErr::Server;
  } else if (r.status == 400) {
    e.kind = SyncErr::BadRequest;
  } else if (r.status == 410) {
    e.kind = SyncErr::PageExpired;
  } else if (r.status == 413) {
    e.kind = SyncErr::PayloadTooLarge;
  } else {
    e.kind = SyncErr::BadResponse;
  }
  if (e.code.empty()) e.code = "http_" + std::to_string(r.status);
  return e;
}

std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// ---- pairing ----

SyncError pairStart(Http& http, const std::string& server, const std::string& deviceName,
                    const std::string& firmware, PairStart& out) {
  JsonDocument req;
  req["protocol"] = 1;
  std::string name = truncateCodePoints(trim(deviceName), limits::DEVICE_NAME);
  if (name.empty()) name = "X4 Pro";
  req["deviceName"] = name;
  req["deviceModel"] = "xteink-x4-pro";
  if (!firmware.empty()) req["firmware"] = truncateCodePoints(firmware, 60);
  std::string body;
  serializeJson(req, body);
  const HttpResponse r = http.postJson(server + "/api/v1/sync/pair/start", body, "");
  if (r.status != 200) return errorFromResponse(r, 0);
  JsonDocument doc;
  if (!r.complete || deserializeJson(doc, r.body) != DeserializationError::Ok || !doc.is<JsonObject>())
    return deviceError(SyncErr::BadResponse, "bad_response");
  if (!protocolOk(doc)) return deviceError(SyncErr::Protocol, "protocol_unsupported");
  out.deviceCode = str(doc["deviceCode"]);
  out.userCode = str(doc["userCode"]);
  out.verificationUri = str(doc["verificationUri"]);
  out.verificationUriComplete = str(doc["verificationUriComplete"]);
  if (out.verificationUriComplete.empty()) out.verificationUriComplete = out.verificationUri;
  out.expiresIn = doc["expiresIn"] | 600;
  out.interval = std::max(1, doc["interval"] | 5);
  if (out.deviceCode.empty() || out.userCode.empty() || out.verificationUriComplete.empty())
    return deviceError(SyncErr::BadResponse, "bad_response");
  return {};
}

SyncError pairPoll(Http& http, const std::string& server, const std::string& deviceCode, PairPoll& out) {
  JsonDocument req;
  req["protocol"] = 1;
  req["deviceCode"] = deviceCode;
  std::string body;
  serializeJson(req, body);
  const HttpResponse r = http.postJson(server + "/api/v1/sync/pair/poll", body, "");
  if (r.status != 200) return errorFromResponse(r, 0);
  JsonDocument doc;
  if (!r.complete || deserializeJson(doc, r.body) != DeserializationError::Ok || !doc.is<JsonObject>())
    return deviceError(SyncErr::BadResponse, "bad_response");
  if (!protocolOk(doc)) return deviceError(SyncErr::Protocol, "protocol_unsupported");
  const std::string status = str(doc["status"]);
  out = PairPoll{};
  out.interval = doc["interval"] | 0;
  if (status == "pending") {
    out.status = PairStatus::Pending;
  } else if (status == "slow_down") {
    out.status = PairStatus::SlowDown;
  } else if (status == "denied") {
    out.status = PairStatus::Denied;
  } else if (status == "expired") {
    out.status = PairStatus::Expired;
  } else if (status == "approved") {
    out.status = PairStatus::Approved;
    out.token = str(doc["token"]);
    out.tokenHint = str(doc["tokenHint"]);
    out.deviceName = str(doc["deviceName"]);
    readAccount(doc["account"], out.account);
    if (out.token.empty()) return deviceError(SyncErr::BadResponse, "bad_response");
  } else {
    return deviceError(SyncErr::BadResponse, "bad_response");
  }
  return {};
}

bool isOtherAccount(const SyncState& st, const PairPoll& p) {
  return !st.account.id.empty() && !p.account.id.empty() && st.account.id != p.account.id;
}

bool applyPairing(Fs& fs, SyncState& st, const PairPoll& p, bool discardLocal) {
  if (discardLocal) {
    for (const auto& lang : bankLanguages(fs)) fs.remove(bankPath(lang));
    fs.remove(paths::LEGACY_BANK);
    fs.remove(paths::OUTBOX);
    st.cursor.clear();
    st.cursors.clear();
    st.lastSyncAt.clear();
  }
  st.token = p.token;
  st.tokenHint = p.tokenHint;
  if (!p.deviceName.empty()) st.deviceName = p.deviceName;
  if (!p.account.id.empty()) st.account = p.account;
  // Every study language syncs; `language`/`cursor` only mirror the first one.
  if (!st.account.studyLanguages.empty()) st.language = st.account.studyLanguages.front();
  const auto c = st.cursors.find(st.language);
  st.cursor = c == st.cursors.end() ? "" : c->second;
  st.failures = 0;
  st.retryAtMs = 0;
  st.lastError.clear();
  return saveState(fs, st);
}

// ---- push ----

namespace {

struct Push {
  Fs& fs;
  Http& http;
  SyncState& st;
  const SyncOptions& opt;
  SyncStats& stats;
  std::set<std::string>& resolved;
  std::vector<std::string> rejectedLines;
  int sent = 0, total = 0;

  void progress() {
    if (opt.progress) opt.progress(SyncProgress{SyncProgress::Push, sent, total});
  }

  SyncError send(const std::vector<OutboxLine>& batch, size_t lo, size_t hi) {
    std::string body = "{\"protocol\":1,\"deviceNow\":\"" + formatIso(opt.nowMs) + "\",\"ops\":[";
    for (size_t i = lo; i < hi; i++) {
      if (i > lo) body.push_back(',');
      body += batch[i].json;
    }
    body += "]}";
    const HttpResponse r = http.postJson(st.server + "/api/v1/sync/push", body, st.token);
    body.clear();
    body.shrink_to_fit();
    if (r.status == 413 && hi - lo > 1) {
      const size_t mid = lo + (hi - lo) / 2;
      SyncError e = send(batch, lo, mid);
      if (!e.ok()) return e;
      return send(batch, mid, hi);
    }
    if (r.status != 200) return errorFromResponse(r, opt.nowMs);
    JsonDocument doc;
    if (!r.complete || deserializeJson(doc, r.body) != DeserializationError::Ok || !doc.is<JsonObject>())
      return deviceError(SyncErr::BadResponse, "bad_response");
    if (!protocolOk(doc)) return deviceError(SyncErr::Protocol, "protocol_unsupported");
    JsonArrayConst results = doc["results"].as<JsonArrayConst>();
    // A count mismatch (or a result for another op) resolves nothing (section 5).
    if (results.size() != hi - lo) return deviceError(SyncErr::BadResponse, "results_mismatch");
    size_t i = lo;
    for (JsonObjectConst res : results) {
      if (batch[i].clientId != str(res["clientId"])) return deviceError(SyncErr::BadResponse, "results_mismatch");
      i++;
    }
    i = lo;
    for (JsonObjectConst res : results) {
      const OutboxLine& op = batch[i++];
      const std::string status = str(res["status"]);
      if (status == "applied") {
        stats.applied++;
      } else if (status == "duplicate") {
        stats.duplicate++;
      } else if (status == "dropped") {
        stats.dropped++;
      } else if (status == "rejected") {
        stats.rejected++;
        std::string resJson;
        serializeJson(res, resJson);
        rejectedLines.push_back("{\"op\":" + op.json + ",\"result\":" + resJson + "}");
      } else {
        continue;  // unknown status: not definitive, the op stays in the outbox
      }
      resolved.insert(op.clientId);
      stats.pushed++;
      const bool ok = status == "applied" || status == "duplicate";
      if (op.json.find("\"type\":\"deckImport\"") != std::string::npos) {
        DeckImportResult& d = stats.deckImports[op.clientId];
        d.ok = ok;
        d.deckId = str(res["deckId"]);
        d.created = res["created"] | false;
        d.fichas = res["fichas"] | 0;
        continue;
      }
      if (!ok) continue;
      if (op.json.find("\"type\":\"extraNew\"") != std::string::npos) {
        stats.extraNew++;
        continue;
      }
      JsonDocument opDoc;
      if (deserializeJson(opDoc, op.json) != DeserializationError::Ok) continue;
      if (std::string(str(opDoc["type"])) != "review") continue;
      stats.reviewsResolved++;
      std::string seed = str(opDoc["flashcardId"]);
      if (seed.empty()) seed = str(opDoc["saveClientId"]);
      int64_t due = 0;
      if (!seed.empty() && parseIso(str(res["flashcard"]["dueAt"]), due)) stats.serverDue[seed] = due;
    }
    sent += static_cast<int>(hi - lo);
    progress();
    return {};
  }

  SyncError run() {
    forEachOutbox(fs, [this](const OutboxLine&) {
      total++;
      return true;
    });
    if (total == 0) return {};
    progress();
    SyncError err;
    std::vector<OutboxLine> batch;
    size_t bytes = 0;
    auto flush = [&]() {
      if (batch.empty()) return true;
      err = send(batch, 0, batch.size());
      batch.clear();
      bytes = 0;
      return err.ok();
    };
    forEachOutbox(fs, [&](const OutboxLine& l) {
      if (static_cast<int>(batch.size()) >= opt.maxBatchOps || (!batch.empty() && bytes + l.json.size() + 1 > opt.maxBatchBytes)) {
        if (!flush()) return false;
      }
      bytes += l.json.size() + 1;
      batch.push_back(l);
      return true;
    });
    if (err.ok()) flush();
    appendRejected(fs, rejectedLines);
    return err;
  }
};

// ---- snapshot ----

// The snapshot of one study language, downloaded next to its bank file and only swapped in
// by commit() once every language downloaded fine.
struct Snapshot {
  Fs& fs;
  Http& http;
  const SyncState& st;
  const SyncOptions& opt;
  SyncStats& stats;  // only for the cumulative progress count
  const std::string lang, bank, bankTmpPath, deltaTmpPath, since;
  bool sentSince = false;

  Snapshot(Fs& f, Http& h, const SyncState& s, const SyncOptions& o, SyncStats& out, const std::string& language)
      : fs(f),
        http(h),
        st(s),
        opt(o),
        stats(out),
        lang(language),
        bank(bankPath(language)),
        bankTmpPath(bank + ".tmp"),
        deltaTmpPath(bank + ".delta.tmp"),
        since(cursorOf(s, language)) {}

  static std::string cursorOf(const SyncState& s, const std::string& language) {
    const auto it = s.cursors.find(language);
    return it == s.cursors.end() ? "" : it->second;
  }

  // whole snapshot
  std::unique_ptr<File> bankTmp, deltaTmp;
  bool haveHeader = false;
  std::string headerLine, cursor, mode, serverNow;
  int total = 0;
  int received = 0, bankItems = 0, removed = 0, fichas = 0;
  std::unordered_set<std::string> ids, delIds;
  // current page
  bool pageHeader = false, ended = false;
  int pageCount = 0;
  std::string next, partial;
  SyncError fail;

  void reset() {
    bankTmp.reset();
    deltaTmp.reset();
    fs.remove(bankTmpPath);
    fs.remove(deltaTmpPath);
    haveHeader = false;
    headerLine.clear();
    cursor.clear();
    mode.clear();
    serverNow.clear();
    total = 0;
    received = bankItems = removed = fichas = 0;
    ids.clear();
    delIds.clear();
  }

  void progress(SyncProgress::Phase phase) {
    if (opt.progress) opt.progress(SyncProgress{phase, stats.received + received, stats.snapshotTotal + total});
  }

  bool bad(const char* code, SyncErr kind = SyncErr::BadResponse) {
    if (fail.ok()) fail = deviceError(kind, code);
    return false;
  }

  bool writeLine(File& f, const std::string& line) {
    if (!f.writeStr(line) || !f.writeStr("\n")) return bad("storage", SyncErr::Disk);
    return true;
  }

  bool onLine(const std::string& line) {
    if (line.empty()) return true;
    if (line.size() > MAX_SNAPSHOT_LINE) return bad("line_too_long");
    if (ended) return bad("data_after_end");
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok || !doc.is<JsonObject>()) return bad("bad_line");
    const std::string t = str(doc["t"]);
    if (!pageHeader) {
      if (t != "header") return bad("missing_header");
      if (!protocolOk(doc)) return bad("protocol_unsupported", SyncErr::Protocol);
      if (lang != str(doc["language"])) return bad("wrong_language");
      pageHeader = true;
      if (haveHeader) return true;  // later pages repeat the first header
      mode = str(doc["mode"]);
      if (mode != "full" && !(mode == "delta" && sentSince)) return bad("bad_mode");
      cursor = str(doc["cursor"]);
      serverNow = str(doc["serverNow"]);
      total = doc["total"] | 0;
      headerLine = line;
      haveHeader = true;
      bankTmp = fs.open(bankTmpPath, OpenMode::Write);
      if (!bankTmp) return bad("storage", SyncErr::Disk);
      if (mode == "full") return writeLine(*bankTmp, headerLine);
      deltaTmp = fs.open(deltaTmpPath, OpenMode::Write);
      if (!deltaTmp) return bad("storage", SyncErr::Disk);
      return true;
    }
    if (t == "end") {
      if ((doc["count"] | -1) != pageCount) return bad("count_mismatch");
      next = str(doc["next"]);
      ended = true;
      return true;
    }
    if (t != "item" && t != "del" && t != "ficha") return true;  // unknown line types are ignored
    const std::string id = str(doc["id"]);
    if (id.empty()) return bad("bad_line");
    pageCount++;
    received++;
    if (received % 50 == 0) progress(SyncProgress::Snapshot);
    if (t == "ficha") fichas++;
    if (mode == "full") {
      if (t == "del") return true;  // `del` only means something in a delta
      // Items come before fichas (pages never mix them), so the bank keeps that order.
      ids.insert(id);
      return writeLine(*bankTmp, line);
    }
    ids.insert(id);
    if (t == "del") delIds.insert(id);
    return writeLine(*deltaTmp, line);
  }

  bool feed(const char* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
      if (data[i] != '\n') {
        if (partial.size() > MAX_SNAPSHOT_LINE) return bad("line_too_long");
        partial.push_back(data[i]);
        continue;
      }
      std::string line;
      line.swap(partial);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!onLine(line)) return false;
    }
    return true;
  }

  // One page; true when it ended with a valid `end`.
  bool page(const std::string& pageToken) {
    pageHeader = ended = false;
    pageCount = 0;
    next.clear();
    partial.clear();
    std::string url = st.server + "/api/v1/sync/snapshot?protocol=1&language=" + urlEncode(lang);
    if (sentSince) url += "&since=" + urlEncode(since);
    url += "&limit=" + std::to_string(opt.pageLimit);
    if (!pageToken.empty()) url += "&page=" + urlEncode(pageToken);
    const HttpResponse r =
        http.getStream(url, st.token, [this](const char* d, size_t n) { return feed(d, n); });
    if (!fail.ok()) return false;
    if (r.status != 200) {
      fail = errorFromResponse(r, opt.nowMs);
      return false;
    }
    if (!partial.empty() && r.complete && !onLine(partial)) return false;
    if (!r.complete || !ended) return bad("incomplete");
    return true;
  }

  bool download() {
    std::string token;
    do {
      if (!page(token)) return false;
      token = next;
    } while (!token.empty());
    if (!haveHeader) return bad("missing_header");
    return true;
  }

  // Rewrites the old bank with the delta: changed lines replaced, new ones added, deleted
  // ones removed, header swapped (section 8). Items first, then fichas, like a full snapshot.
  bool mergeDelta() {
    deltaTmp->sync();
    deltaTmp.reset();
    if (!writeLine(*bankTmp, headerLine)) return false;
    int items = 0;
    bool ok = true;
    std::unordered_set<std::string> written;
    for (const char* type : {"item", "ficha"}) {
      forEachTypedLine(fs, bank, [&](const std::string& t, const std::string& id, const std::string& line) {
        if (t != type) return true;
        if (ids.count(id)) {
          if (delIds.count(id) && t == "item") removed++;
          return true;
        }
        if (t == "item") items++;
        ok = writeLine(*bankTmp, line);
        return ok;
      });
      if (!ok) return false;
      forEachTypedLine(fs, deltaTmpPath, [&](const std::string& t, const std::string& id, const std::string& line) {
        if (t != type || delIds.count(id) || !written.insert(id).second) return true;
        if (t == "item") items++;
        ok = writeLine(*bankTmp, line);
        return ok;
      });
      if (!ok) return false;
    }
    bankItems = items;
    return true;
  }

  // Downloads every page into <bank>.tmp (merging a delta with the current bank). The bank
  // itself is untouched; on failure nothing is left behind.
  SyncError prepare() {
    std::string bankLang;
    const bool haveBank = readBankHeader(fs, bank, bankLang) && bankLang == lang;
    sentSince = !since.empty() && haveBank;
    for (int attempt = 0; attempt < 2; attempt++) {
      reset();
      fail = SyncError{};
      progress(SyncProgress::Snapshot);
      if (download()) break;
      if (fail.kind != SyncErr::PageExpired) break;
    }
    if (!fail.ok()) {
      reset();
      return fail;
    }
    progress(SyncProgress::Save);
    if (mode == "delta") {
      if (!mergeDelta()) {
        reset();
        return fail;
      }
    } else {
      bankItems = static_cast<int>(ids.size()) - fichas;
      if (haveBank) {
        forEachTypedLine(fs, bank, [&](const std::string& t, const std::string& id, const std::string&) {
          if (t == "item" && !ids.count(id)) removed++;
          return true;
        });
      }
    }
    ids = {};  // the other languages still have to download
    delIds = {};
    if (!bankTmp->sync()) {
      reset();
      return deviceError(SyncErr::Disk, "storage");
    }
    bankTmp.reset();
    fs.remove(deltaTmpPath);
    return {};
  }

  bool commit() { return fs.replace(bankTmpPath, bank); }
};

SyncError finish(Fs& fs, SyncState& st, const SyncOptions& opt, const SyncError& err, const std::string& serverNow) {
  if (err.ok()) {
    st.lastSyncStatus = "ok";
    st.lastError.clear();
    st.failures = 0;
    st.retryAtMs = 0;
    int64_t at = opt.nowMs;
    if (at < MIN_TRUSTED_MS) parseIso(serverNow.c_str(), at);
    st.lastSyncAt = formatIso(at);
  } else {
    st.lastSyncStatus = "error";
    st.lastError = err.code;
    if (err.kind == SyncErr::Server) {
      st.failures++;
      st.retryAtMs = opt.nowMs + BACKOFF_MS[std::min(st.failures, 3) - 1];
    } else if (err.kind == SyncErr::RateLimited) {
      st.retryAtMs = err.retryAtMs;
    } else if (err.kind == SyncErr::Unauthorized) {
      st.token.clear();
      st.tokenHint.clear();
    }
  }
  if (!saveState(fs, st) && err.ok()) return deviceError(SyncErr::Disk, "storage");
  return err;
}

// opt.languages, else the account's study languages; with neither (a state.json without
// the account), the languages already on the card.
std::vector<std::string> syncLanguages(Fs& fs, const SyncState& st, const SyncOptions& opt) {
  std::vector<std::string> out;
  auto add = [&out](const std::string& l) {
    if (!bankPath(l).empty() && std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
  };
  for (const auto& l : opt.languages.empty() ? st.account.studyLanguages : opt.languages) add(l);
  if (out.empty()) {
    add(st.language);
    for (const auto& l : bankLanguages(fs)) add(l);
  }
  return out;
}

}  // namespace

SyncError runSync(Fs& fs, Http& http, SyncState& st, const SyncOptions& opt, SyncStats& out) {
  out = SyncStats{};
  if (!st.paired()) return deviceError(SyncErr::NotPaired, "not_paired");
  if (!opt.ignoreBackoff && st.retryAtMs > opt.nowMs) {
    SyncError e = deviceError(SyncErr::Backoff, "backoff");
    e.retryAtMs = st.retryAtMs;
    return e;
  }
  fs.mkdirs(paths::ROOT);
  fs.mkdirs(paths::BANK_DIR);
  migrateLegacyBank(fs, st);

  // The SD's CSV/TSV decks go up with this push (contract section 4, deckImport).
  const std::vector<std::string> langs = syncLanguages(fs, st, opt);
  if (opt.newId) {
    const std::string importLang = !opt.importLanguage.empty() ? opt.importLanguage : langs.empty() ? "" : langs.front();
    out.decksQueued = deckimport::queue(fs, importLang, opt.nowMs, opt.newId);
  }

  std::set<std::string> resolved;
  Push push{fs, http, st, opt, out, resolved, {}, 0, 0};
  SyncError err = push.run();
  if (!err.ok()) return finish(fs, st, opt, err, "");

  // Every study language downloads before any bank changes: one failure keeps them all.
  std::vector<std::unique_ptr<Snapshot>> snaps;
  std::string serverNow;
  for (const auto& lang : langs) {
    snaps.emplace_back(new Snapshot(fs, http, st, opt, out, lang));
    Snapshot& snap = *snaps.back();
    err = snap.prepare();
    if (!err.ok()) {
      for (auto& s : snaps) s->reset();
      return finish(fs, st, opt, err, "");
    }
    out.received += snap.received;
    out.snapshotTotal += snap.total;
    out.bankItems += snap.bankItems;
    out.bankFichas += snap.fichas;
    out.removed += snap.removed;
    out.itemsByLanguage[lang] = snap.bankItems;
    if (out.mode.empty() || snap.mode == "full") out.mode = snap.mode;
    if (serverNow.empty()) serverNow = snap.serverNow;
  }
  std::map<std::string, std::string> cursors;
  for (auto& snap : snaps) {
    if (!snap->commit()) {
      for (auto& s : snaps) s->reset();
      st.cursors.clear();  // some banks are new, some old: full snapshots next time
      st.cursor.clear();
      return finish(fs, st, opt, deviceError(SyncErr::Disk, "storage"), serverNow);
    }
    cursors[snap->lang] = snap->cursor;
  }
  // A language the account no longer studies has no deck any more.
  for (const auto& lang : bankLanguages(fs)) {
    if (!snaps.empty() && !cursors.count(lang)) fs.remove(bankPath(lang));
  }

  if (!langs.empty()) st.language = langs.front();
  // Only now drop what the push resolved, then keep the new cursors (section 8, step 4).
  if (!rewriteOutboxWithout(fs, resolved)) {
    // The banks already show the resolved ops; full snapshots next time keep them honest.
    st.cursors.clear();
    st.cursor.clear();
    return finish(fs, st, opt, deviceError(SyncErr::Disk, "storage"), serverNow);
  }
  st.cursors = cursors;
  const auto c = cursors.find(st.language);
  st.cursor = c == cursors.end() ? "" : c->second;
  // The imported decks are in the new banks now: their local copies can go.
  out.decksImported = deckimport::settle(fs, out.deckImports);
  return finish(fs, st, opt, {}, serverNow);
}

int countAdjusted(const LocalView& before, const std::map<std::string, int64_t>& serverDue) {
  int n = 0;
  for (size_t i = 0; i < before.size(); i++) {
    const ViewEntry& e = before.entry(i);
    const auto it = serverDue.find(e.seed());
    if (it == serverDue.end()) continue;
    if (std::llabs(it->second - e.card.dueMs) > 60000) n++;
  }
  return n;
}

}  // namespace linguetta
