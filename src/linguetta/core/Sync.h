#pragma once
// Client side of /api/v1/sync, protocol 1 (docs/sync/CONTRACT.md sections 2, 3, 5-8, 10):
// device pairing, push of the outbox, one paged snapshot per study language into
// bank/<language>.jsonl, and the state.json bookkeeping (cursors, last sync, backoff). Transport is abstract (Http) so the whole flow
// runs in host tests against the contract fixtures; the device plugs in the upstream HTTPS
// client (device/SyncHttp).

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Fs.h"
#include "Store.h"

namespace linguetta {

class LocalView;

// ---- transport ----

struct HttpResponse {
  int status = -1;        // -1: no response (DNS, TLS, timeout...)
  bool complete = false;  // the whole body arrived
  std::string body;       // buffered body (for streamed GETs: only non-200 bodies)
  int retryAfterS = -1;   // Retry-After header in seconds, -1 when absent
};

class Http {
 public:
  virtual ~Http() = default;
  // POST a JSON body. `bearer` "" = no Authorization header.
  virtual HttpResponse postJson(const std::string& url, const std::string& body, const std::string& bearer) = 0;
  // GET; a 200 body is handed to onData in pieces (return false to stop), any other status
  // is buffered into `body`.
  virtual HttpResponse getStream(const std::string& url, const std::string& bearer,
                                 const std::function<bool(const char*, size_t)>& onData) = 0;
};

// ---- errors ----

enum class SyncErr {
  None,
  NotPaired,        // no token in state.json
  Backoff,          // still waiting after a 5xx / 429 (retryAtMs)
  Network,          // no response
  Unauthorized,     // 401 invalid_token: token cleared, pair again
  Protocol,         // 426: update the firmware
  RateLimited,      // 429
  Server,           // 5xx
  BadRequest,       // 400
  PageExpired,      // 410 twice in a row
  PayloadTooLarge,  // 413 even for a single operation
  BadResponse,      // unexpected/incomplete/mismatched response
  Disk,             // SD write failed
};

struct SyncError {
  SyncErr kind = SyncErr::None;
  int status = 0;
  std::string code;     // server `code`, or a device code ("network", "bad_response", ...)
  std::string message;  // server `message` (Portuguese), when it sent one
  int64_t retryAtMs = 0;
  bool ok() const { return kind == SyncErr::None; }
};

// Short Portuguese text for the result screens.
std::string errorText(const SyncError& e);
// Classifies a non-2xx (or failed) response. `nowMs` dates the Retry-After of a 429.
SyncError errorFromResponse(const HttpResponse& r, int64_t nowMs);

// ---- pairing (section 3) ----

struct PairStart {
  std::string deviceCode, userCode, verificationUri, verificationUriComplete;
  int expiresIn = 600, interval = 5;
};
SyncError pairStart(Http& http, const std::string& server, const std::string& deviceName,
                    const std::string& firmware, PairStart& out);

enum class PairStatus { Pending, SlowDown, Denied, Expired, Approved };
struct PairPoll {
  PairStatus status = PairStatus::Pending;
  int interval = 0;  // new interval (slow_down / pending), 0 when absent
  std::string token, tokenHint, deviceName;
  Account account;
};
SyncError pairPoll(Http& http, const std::string& server, const std::string& deviceCode, PairPoll& out);

// True when the approved account differs from the one the card's data belongs to: the
// device must ask before discarding bank, outbox and cursor.
bool isOtherAccount(const SyncState& st, const PairPoll& p);
// Stores the token and account in `st` and saves state.json.
// discardLocal: also removes every bank file and outbox.jsonl and forgets the cursors.
bool applyPairing(Fs& fs, SyncState& st, const PairPoll& p, bool discardLocal);

// ---- sync (sections 5, 6, 8) ----

struct SyncProgress {
  enum Phase { Push, Snapshot, Save } phase = Push;
  // ops sent / ops to send; snapshot lines received / header totals (all languages so far)
  int done = 0, total = 0;
};

struct SyncOptions {
  int64_t nowMs = 0;     // device clock (UTC ms); deviceNow and the backoff use it
  // Study languages to download; empty = the account's studyLanguages (state.json).
  std::vector<std::string> languages;
  int pageLimit = 500;   // snapshot `limit`
  int maxBatchOps = 200;
  size_t maxBatchBytes = 256 * 1024;  // stays well under the 512 KB push cap
  bool ignoreBackoff = false;
  std::function<void(const SyncProgress&)> progress;
  // Client ids for the ops the sync itself creates (deckImport of the SD's CSV/TSV decks).
  // Unset: no deck import.
  std::function<std::string()> newId;
  // Study language of imported decks; "" = the first sync language.
  std::string importLanguage;
};

struct DeckImportResult {
  bool ok = false;  // applied or duplicate
  std::string deckId;
  bool created = false;
  int fichas = 0;
};

struct SyncStats {
  int pushed = 0;  // ops resolved (any of the four statuses)
  int applied = 0, duplicate = 0, dropped = 0, rejected = 0;
  int reviewsResolved = 0;  // review ops applied or duplicate: the server replayed them
  // card seed (cardId, or the saveClientId of a local item) -> server dueAt after the last
  // resolved review of that card
  std::map<std::string, int64_t> serverDue;
  int extraNew = 0;  // extraNew ops applied or duplicate
  std::map<std::string, DeckImportResult> deckImports;  // by clientId, every resolved deckImport
  std::string mode;  // "full" if any language got a full snapshot, else "delta"
  int received = 0;  // item + ficha + del lines downloaded (all languages)
  int snapshotTotal = 0;
  int bankItems = 0;  // items in all banks afterwards
  int bankFichas = 0; // fichas received (full snapshots)
  int removed = 0;    // items that were in an old bank and are gone now
  std::map<std::string, int> itemsByLanguage;
  int decksQueued = 0;    // deck files queued as deckImport this sync
  int decksImported = 0;  // deck files whose import completed (now server decks)
};

// Runs push, then the snapshot of every study language (all downloaded before any bank is
// replaced), then rewrites the outbox and saves the cursors (section 8, "Ordem do sync").
// Migrates a single-language bank.jsonl first. Always saves state.json (status, error, backoff; a 401 clears the token).
SyncError runSync(Fs& fs, Http& http, SyncState& st, const SyncOptions& opt, SyncStats& out);

// Cards whose server schedule differs from the local prediction (by more than a minute).
// `before` is the local view loaded before the sync.
int countAdjusted(const LocalView& before, const std::map<std::string, int64_t>& serverDue);

// Percent-encodes a query value.
std::string urlEncode(const std::string& s);

}  // namespace linguetta
