// Sync client against the contract fixtures (docs/sync/fixtures): pairing, push, full and
// delta snapshots, errors, backoff and the state.json bookkeeping.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <deque>
#include <fstream>

#include "DeckImport.h"
#include "LocalView.h"
#include "Ops.h"
#include "StdFs.h"
#include "Store.h"
#include "Sync.h"
#include "TestUtil.h"
#include "Text.h"

using namespace linguetta;

namespace {

constexpr const char* SERVER = "https://linguetta-app.vercel.app";
constexpr const char* TOKEN = "lr_pY2vQ9xN4cM8bR1tK6wZ3aL7dF0hJ5sE2uG8oP4iT1k";
constexpr const char* CURSOR = "v1.MjAyNi0xMS0xMVQyMTozMDowMi40MTdaOjg4MjEz";

std::string jsonOf(JsonVariantConst v) {
  std::string s;
  serializeJson(v, s);
  return s;
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

// Scripted server: each request takes the next queued response for its route.
struct FakeHttp : Http {
  struct Call {
    std::string method, url, body, bearer;
  };
  std::vector<Call> calls;
  std::map<std::string, std::deque<HttpResponse>> queue;  // route ("push", "snapshot", ...) -> responses
  std::function<HttpResponse(const Call&)> handler;       // overrides the queue when set

  static std::string route(const std::string& url) {
    const size_t p = url.find("/api/v1/sync/");
    std::string r = p == std::string::npos ? url : url.substr(p + 13);
    const size_t q = r.find('?');
    return q == std::string::npos ? r : r.substr(0, q);
  }
  HttpResponse next(const Call& c) {
    if (handler) return handler(c);
    auto& q = queue[route(c.url)];
    if (q.empty()) return HttpResponse{};
    HttpResponse r = q.front();
    q.pop_front();
    return r;
  }
  HttpResponse postJson(const std::string& url, const std::string& body, const std::string& bearer) override {
    calls.push_back({"POST", url, body, bearer});
    return next(calls.back());
  }
  HttpResponse getStream(const std::string& url, const std::string& bearer,
                         const std::function<bool(const char*, size_t)>& onData) override {
    calls.push_back({"GET", url, "", bearer});
    HttpResponse r = next(calls.back());
    if (r.status == 200) {
      // deliver in odd-sized pieces to exercise line splitting
      for (size_t i = 0; i < r.body.size(); i += 37) {
        if (!onData(r.body.data() + i, std::min<size_t>(37, r.body.size() - i))) {
          r.complete = false;
          break;
        }
      }
      r.body.clear();
    }
    return r;
  }
  void add(const std::string& route, int status, const std::string& body, bool complete = true, int retryAfter = -1) {
    HttpResponse r;
    r.status = status;
    r.body = body;
    r.complete = complete;
    r.retryAfterS = retryAfter;
    queue[route].push_back(r);
  }
};

std::string errorBody(const char* key) { return jsonOf(loadJsonFixture("errors.json")[key]); }

class SyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/linguetta-sync-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir = tmpl;
    fs = std::make_unique<StdFs>(dir);
    fs->mkdirs(paths::BANK_DIR);
    st.server = SERVER;
    st.token = TOKEN;
    st.language = "it";
    st.account.studyLanguages = {"it"};
    opt.nowMs = at("2026-11-11T21:30:00.000Z");
  }
  void TearDown() override { std::system(("rm -rf '" + dir + "'").c_str()); }

  void put(const std::string& path, const std::string& content) {
    std::ofstream out(fs->real(path), std::ios::binary);
    out << content;
  }
  std::string get(const std::string& path) {
    std::string out;
    readFile(*fs, path, out, 1 << 20);
    return out;
  }
  static int64_t at(const char* iso) {
    int64_t ms = 0;
    parseIso(iso, ms);
    return ms;
  }
  void queueFullSnapshot() {
    http.add("snapshot", 200, readFixture("snapshot-full.page1.jsonl"));
    http.add("snapshot", 200, readFixture("snapshot-full.page2.jsonl"));
    http.add("snapshot", 200, readFixture("snapshot-full.page3.jsonl"));  // the ficha page
  }
  void queuePush() { http.add("push", 200, readFixture("push.response.json")); }

  const std::string BANK = bankPath("it");
  const std::string BANK_TMP = BANK + ".tmp";
  std::string dir;
  std::unique_ptr<StdFs> fs;
  FakeHttp http;
  SyncState st;
  SyncOptions opt;
  SyncStats stats;
};

// ---- pairing ----

TEST_F(SyncTest, PairStartSendsTheEnvelopeAndReadsTheCode) {
  http.add("pair/start", 200, readFixture("pair-start.response.json"));
  PairStart ps;
  ASSERT_TRUE(pairStart(http, SERVER, "X4 Pro", "linguetta-x4/1.0", ps).ok());
  ASSERT_EQ(http.calls.size(), 1u);
  EXPECT_EQ(http.calls[0].url, std::string(SERVER) + "/api/v1/sync/pair/start");
  EXPECT_EQ(http.calls[0].bearer, "");
  JsonDocument req;
  deserializeJson(req, http.calls[0].body);
  EXPECT_EQ(req["protocol"].as<int>(), 1);
  EXPECT_STREQ(req["deviceName"], "X4 Pro");
  EXPECT_STREQ(req["deviceModel"], "xteink-x4-pro");
  EXPECT_STREQ(req["firmware"], "linguetta-x4/1.0");

  EXPECT_EQ(ps.deviceCode, "Zk3pQ8vX1mN7cR2tY6wL0aB4dF9hJ5sE_uK8oP3iG7q");
  EXPECT_EQ(ps.userCode, "K7Q-4MP");
  EXPECT_EQ(ps.verificationUriComplete, "https://linguetta-app.vercel.app/parear?code=K7Q4MP");
  EXPECT_EQ(ps.expiresIn, 600);
  EXPECT_EQ(ps.interval, 5);
}

TEST_F(SyncTest, PairStartRateLimited) {
  http.add("pair/start", 429, errorBody("429 rate_limited"));
  PairStart ps;
  const SyncError e = pairStart(http, SERVER, "X4 Pro", "", ps);
  EXPECT_EQ(e.kind, SyncErr::RateLimited);
  EXPECT_EQ(e.code, "rate_limited");
}

TEST_F(SyncTest, PairPollStatuses) {
  JsonDocument responses = loadJsonFixture("pair-poll.responses.json");
  const std::pair<const char*, PairStatus> cases[] = {{"pending", PairStatus::Pending},
                                                      {"slow_down", PairStatus::SlowDown},
                                                      {"denied", PairStatus::Denied},
                                                      {"expired", PairStatus::Expired},
                                                      {"approved", PairStatus::Approved}};
  for (const auto& [key, status] : cases) {
    http.add("pair/poll", 200, jsonOf(responses[key]));
    PairPoll p;
    ASSERT_TRUE(pairPoll(http, SERVER, "Zk3pQ8vX1mN7cR2tY6wL0aB4dF9hJ5sE_uK8oP3iG7q", p).ok()) << key;
    EXPECT_EQ(p.status, status) << key;
    if (status == PairStatus::SlowDown) {
      EXPECT_EQ(p.interval, 10);
    }
    if (status == PairStatus::Approved) {
      EXPECT_EQ(p.token, TOKEN);
      EXPECT_EQ(p.tokenHint, "iT1k");
      EXPECT_EQ(p.account.id, "3f6c9a12-8d4e-4b7a-9c2f-1e0d5a6b7c80");
      ASSERT_EQ(p.account.studyLanguages.size(), 2u);
    }
  }
  // request body as in pair-poll.request.json
  JsonDocument req, want = loadJsonFixture("pair-poll.request.json");
  deserializeJson(req, http.calls[0].body);
  EXPECT_EQ(jsonOf(req), jsonOf(want));
}

TEST_F(SyncTest, ApprovedPairingWritesStateJson) {
  JsonDocument responses = loadJsonFixture("pair-poll.responses.json");
  http.add("pair/poll", 200, jsonOf(responses["approved"]));
  PairPoll p;
  ASSERT_TRUE(pairPoll(http, SERVER, "x", p).ok());
  SyncState fresh;
  EXPECT_FALSE(isOtherAccount(fresh, p));
  ASSERT_TRUE(applyPairing(*fs, fresh, p, false));
  SyncState loaded;
  ASSERT_TRUE(loadState(*fs, loaded));
  JsonDocument want = loadJsonFixture("state.json");
  EXPECT_EQ(loaded.token, want["token"].as<std::string>());
  EXPECT_EQ(loaded.tokenHint, "iT1k");
  EXPECT_EQ(loaded.account.email, "matheus@example.com");
  EXPECT_EQ(loaded.language, "it");  // first study language
  EXPECT_EQ(loaded.cursor, "");
}

TEST_F(SyncTest, OtherAccountCanDiscardLocalData) {
  put(BANK, readFixture("bank.jsonl"));
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  st.account.id = "someone-else";
  st.cursor = CURSOR;
  st.cursors["it"] = CURSOR;
  PairPoll p;
  p.status = PairStatus::Approved;
  p.token = "lr_new";
  p.account.id = "3f6c9a12-8d4e-4b7a-9c2f-1e0d5a6b7c80";
  p.account.studyLanguages = {"en"};
  EXPECT_TRUE(isOtherAccount(st, p));
  ASSERT_TRUE(applyPairing(*fs, st, p, true));
  EXPECT_FALSE(fs->exists(BANK));
  EXPECT_FALSE(fs->exists(paths::OUTBOX));
  EXPECT_EQ(st.cursor, "");
  EXPECT_TRUE(st.cursors.empty());
  EXPECT_EQ(st.language, "en");
  EXPECT_EQ(st.token, "lr_new");
}

// ---- sync ----

TEST_F(SyncTest, FullSyncPushesThenDownloadsEveryPage) {
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  queuePush();
  queueFullSnapshot();
  std::vector<SyncProgress> seen;
  opt.progress = [&](const SyncProgress& p) { seen.push_back(p); };

  const SyncError e = runSync(*fs, http, st, opt, stats);
  ASSERT_TRUE(e.ok()) << e.code;

  // push: the outbox, in file order, with the bearer token and the envelope
  ASSERT_EQ(http.calls.size(), 4u);
  EXPECT_EQ(http.calls[0].url, std::string(SERVER) + "/api/v1/sync/push");
  EXPECT_EQ(http.calls[0].bearer, TOKEN);
  JsonDocument req, want = loadJsonFixture("push.request.json");
  ASSERT_EQ(deserializeJson(req, http.calls[0].body), DeserializationError::Ok);
  EXPECT_EQ(req["protocol"].as<int>(), 1);
  EXPECT_STREQ(req["deviceNow"], "2026-11-11T21:30:00.000Z");
  EXPECT_EQ(jsonOf(req["ops"]), jsonOf(want["ops"]));

  // snapshot: no cursor yet, so no `since`; the second page carries the token
  EXPECT_EQ(http.calls[1].url, std::string(SERVER) + "/api/v1/sync/snapshot?protocol=1&language=it&limit=500");
  EXPECT_NE(http.calls[2].url.find("&page=p1.eyJsIjoiaXQi"), std::string::npos);
  EXPECT_EQ(http.calls[2].url.find("since="), std::string::npos);
  EXPECT_NE(http.calls[3].url.find("&page="), std::string::npos);

  // bank.jsonl = the fixture (header of page 1 + every item, then the ficha)
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
  EXPECT_FALSE(fs->exists(BANK_TMP));
  // every op was resolved by one of the four statuses
  EXPECT_EQ(get(paths::OUTBOX), "");
  const auto rejected = lines(get(paths::REJECTED));
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_NE(rejected[0].find("\"lema vazio\""), std::string::npos);
  EXPECT_NE(rejected[0].find("958473625106"), std::string::npos);

  EXPECT_EQ(stats.pushed, 9);
  EXPECT_EQ(stats.applied, 6);
  EXPECT_EQ(stats.duplicate, 1);
  EXPECT_EQ(stats.dropped, 1);
  EXPECT_EQ(stats.rejected, 1);
  EXPECT_EQ(stats.reviewsResolved, 2);
  EXPECT_EQ(stats.serverDue.at("5d2a7e10-0c43-4b8f-a6d9-71e0c4b2f902"), at("2026-12-08T22:40:00.000Z"));
  EXPECT_EQ(stats.serverDue.at("d4b3a2c1-0f9e-4d8c-b7a6-958473625103"), at("2026-11-11T08:17:00.000Z"));
  EXPECT_EQ(stats.extraNew, 1);
  ASSERT_EQ(stats.deckImports.size(), 1u);
  const DeckImportResult& imp = stats.deckImports.at("d4b3a2c1-0f9e-4d8c-b7a6-958473625108");
  EXPECT_TRUE(imp.ok);
  EXPECT_EQ(imp.deckId, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c50");
  EXPECT_TRUE(imp.created);
  EXPECT_EQ(imp.fichas, 2);
  EXPECT_EQ(stats.mode, "full");
  EXPECT_EQ(stats.received, 6);  // 5 items + 1 ficha
  EXPECT_EQ(stats.bankItems, 5);
  EXPECT_EQ(stats.bankFichas, 1);
  EXPECT_EQ(stats.removed, 0);
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back().phase, SyncProgress::Save);

  // state.json as in the fixture
  SyncState loaded;
  ASSERT_TRUE(loadState(*fs, loaded));
  EXPECT_EQ(loaded.cursor, CURSOR);
  EXPECT_EQ(loaded.lastSyncStatus, "ok");
  EXPECT_EQ(loaded.lastError, "");
  EXPECT_EQ(loaded.lastSyncAt, "2026-11-11T21:30:00.000Z");
  EXPECT_EQ(loaded.token, TOKEN);

  // the local view now reads the new bank with an empty outbox
  LocalView view;
  view.load(*fs, "it");
  EXPECT_EQ(view.header().cursor, CURSOR);
  EXPECT_EQ(view.size(), 6u);
}

TEST_F(SyncTest, EmptyOutboxSkipsThePush) {
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  ASSERT_EQ(http.calls.size(), 3u);
  EXPECT_EQ(http.calls[0].method, "GET");
}

TEST_F(SyncTest, DeltaReplacesAddsAndDeletesByIdAndSendsTheCursor) {
  // old bank: the fixture header, "il mare" (changed in the delta), one item that the
  // delta deletes and one untouched item
  const auto bank = lines(readFixture("bank.jsonl"));
  std::string old = bank[0] + "\n";
  for (const auto& l : bank) {
    if (l.find("\"il mare\"") != std::string::npos || l.find("ricordarsi") != std::string::npos) old += l + "\n";
  }
  old += "{\"t\":\"item\",\"id\":\"1f2e3d4c-5b6a-4978-8695-a4b3c2d1e006\",\"lemma\":\"cane\",\"language\":\"it\","
         "\"status\":\"ativo\",\"card\":{}}\n";
  put(BANK, old);
  st.cursors["it"] = CURSOR;
  http.add("snapshot", 200, readFixture("snapshot-delta.jsonl"));

  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  ASSERT_EQ(http.calls.size(), 1u);
  EXPECT_NE(http.calls[0].url.find("&since=v1.MjAyNi0xMS0xMVQyMTozMDowMi40MTdaOjg4MjEz&"), std::string::npos);
  EXPECT_EQ(stats.mode, "delta");
  EXPECT_EQ(stats.removed, 1);

  const auto out = lines(get(BANK));
  const auto delta = lines(readFixture("snapshot-delta.jsonl"));
  ASSERT_EQ(out.size(), 7u);  // header + ricordarsi + 4 delta items + the delta ficha
  EXPECT_EQ(out[0], delta[0]);
  EXPECT_NE(out[1].find("ricordarsi"), std::string::npos);
  for (int i = 1; i <= 4; i++) EXPECT_EQ(out[1 + i], delta[i]);
  EXPECT_EQ(out[6], delta[6]);  // fichas after the items
  EXPECT_EQ(get(BANK).find("1f2e3d4c"), std::string::npos);
  EXPECT_EQ(stats.bankItems, 5);
  EXPECT_FALSE(fs->exists(BANK + ".delta.tmp"));
}

TEST_F(SyncTest, BankWithAnotherLanguageHeaderAsksForAFullSnapshot) {
  std::string bank = readFixture("bank.jsonl");
  bank.replace(bank.find("\"language\":\"it\""), 15, "\"language\":\"en\"");
  put(BANK, bank);
  st.cursors["it"] = CURSOR;
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(http.calls[0].url.find("since="), std::string::npos);
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
}

TEST_F(SyncTest, LegacySingleBankIsMovedAndResyncedInFull) {
  put(paths::LEGACY_BANK, readFixture("bank.jsonl"));
  st.cursor = CURSOR;
  st.cursors.clear();
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(http.calls[0].url.find("since="), std::string::npos);
  EXPECT_FALSE(fs->exists(paths::LEGACY_BANK));
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
  EXPECT_EQ(stats.removed, 0);  // the moved bank was compared with the new one
}

// The field bug: account {en, it}, every item in Italian. Each study language gets its own
// paged snapshot, cursor and bank file; English comes back empty.
TEST_F(SyncTest, EveryStudyLanguageGetsItsOwnSnapshotBankAndCursor) {
  st.account.id = "3f6c9a12-8d4e-4b7a-9c2f-1e0d5a6b7c80";
  st.account.studyLanguages = {"en", "it"};
  st.language = "en";
  const std::string enCursor = "v1.ZW4tY3Vyc29y";
  http.add("snapshot", 200,
           "{\"t\":\"header\",\"protocol\":1,\"language\":\"en\",\"mode\":\"full\",\"cursor\":\"" + enCursor +
               "\",\"serverNow\":\"2026-11-11T21:30:02.417Z\",\"total\":0}\n{\"t\":\"end\",\"count\":0}\n");
  queueFullSnapshot();
  std::vector<SyncProgress> seen;
  opt.progress = [&](const SyncProgress& p) { seen.push_back(p); };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());

  ASSERT_EQ(http.calls.size(), 4u);
  EXPECT_NE(http.calls[0].url.find("language=en&"), std::string::npos);
  EXPECT_NE(http.calls[1].url.find("language=it&"), std::string::npos);
  EXPECT_NE(http.calls[2].url.find("language=it&"), std::string::npos);
  EXPECT_NE(http.calls[2].url.find("&page="), std::string::npos);
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
  EXPECT_EQ(lines(get(bankPath("en"))).size(), 1u);  // header only
  EXPECT_EQ(stats.bankItems, 5);
  EXPECT_EQ(stats.itemsByLanguage.at("en"), 0);
  EXPECT_EQ(stats.itemsByLanguage.at("it"), 5);
  EXPECT_EQ(seen.back().done, 6);  // 5 items + 1 ficha

  SyncState loaded;
  ASSERT_TRUE(loadState(*fs, loaded));
  EXPECT_EQ(loaded.cursors.at("en"), enCursor);
  EXPECT_EQ(loaded.cursors.at("it"), CURSOR);
  EXPECT_EQ(loaded.language, "en");
  EXPECT_EQ(loaded.cursor, enCursor);

  LocalView it, en;
  it.load(*fs, "it");
  en.load(*fs, "en");
  EXPECT_EQ(it.size(), 6u);  // 5 items + 1 ficha
  EXPECT_EQ(en.size(), 0u);
  EXPECT_EQ(deckLanguages(*fs, loaded), (std::vector<std::string>{"en", "it"}));

  // next sync: each language sends its own cursor
  http.calls.clear();
  http.add("snapshot", 200,
           "{\"t\":\"header\",\"protocol\":1,\"language\":\"en\",\"mode\":\"delta\",\"cursor\":\"" + enCursor +
               "\",\"serverNow\":\"2026-11-11T21:40:00.000Z\",\"total\":0}\n{\"t\":\"end\",\"count\":0}\n");
  http.add("snapshot", 200, readFixture("snapshot-delta.jsonl"));
  ASSERT_TRUE(runSync(*fs, http, loaded, opt, stats).ok());
  ASSERT_EQ(http.calls.size(), 2u);
  EXPECT_NE(http.calls[0].url.find("since=" + urlEncode(enCursor)), std::string::npos);
  EXPECT_NE(http.calls[1].url.find("since=" + urlEncode(CURSOR)), std::string::npos);
}

TEST_F(SyncTest, OneLanguageFailingKeepsEveryBankAndTheOutbox) {
  st.account.studyLanguages = {"en", "it"};
  put(BANK, "old\n");
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  queuePush();
  http.add("snapshot", 200,
           "{\"t\":\"header\",\"protocol\":1,\"language\":\"en\",\"mode\":\"full\",\"cursor\":\"c\","
           "\"serverNow\":\"2026-11-11T21:30:02.417Z\",\"total\":0}\n{\"t\":\"end\",\"count\":0}\n");
  http.add("snapshot", 500, errorBody("500 server_error"));
  EXPECT_EQ(runSync(*fs, http, st, opt, stats).kind, SyncErr::Server);
  EXPECT_FALSE(fs->exists(bankPath("en")));
  EXPECT_FALSE(fs->exists(bankPath("en") + ".tmp"));
  EXPECT_EQ(get(BANK), "old\n");
  EXPECT_EQ(get(paths::OUTBOX), readFixture("outbox.jsonl"));
  EXPECT_TRUE(st.cursors.empty());
}

TEST_F(SyncTest, BankOfALanguageNoLongerStudiedIsRemoved) {
  put(bankPath("fr"), "{\"t\":\"header\",\"language\":\"fr\"}\n");
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_FALSE(fs->exists(bankPath("fr")));
  EXPECT_EQ(bankLanguages(*fs), std::vector<std::string>{"it"});
}

TEST_F(SyncTest, FullSnapshotCountsItemsThatLeft) {
  std::string old = readFixture("bank.jsonl");
  old += "{\"t\":\"item\",\"id\":\"gone\",\"lemma\":\"cane\",\"language\":\"it\",\"card\":{}}\n";
  put(BANK, old);
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
}

TEST_F(SyncTest, CountMismatchDiscardsTheSnapshotAndKeepsTheOutbox) {
  put(BANK, "old\n");
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  queuePush();
  std::string page1 = readFixture("snapshot-full.page1.jsonl");
  const size_t p = page1.find("\"count\":3");
  page1.replace(p, 9, "\"count\":4");
  http.add("snapshot", 200, page1);

  const SyncError e = runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(e.kind, SyncErr::BadResponse);
  EXPECT_EQ(e.code, "count_mismatch");
  EXPECT_EQ(get(BANK), "old\n");
  EXPECT_FALSE(fs->exists(BANK_TMP));
  EXPECT_EQ(get(paths::OUTBOX), readFixture("outbox.jsonl"));  // ops come back as duplicates next time
  SyncState loaded;
  loadState(*fs, loaded);
  EXPECT_EQ(loaded.lastSyncStatus, "error");
  EXPECT_EQ(loaded.lastError, "count_mismatch");
  EXPECT_EQ(loaded.cursor, "");
}

TEST_F(SyncTest, PageWithoutEndIsDiscarded) {
  std::string page2 = readFixture("snapshot-full.page2.jsonl");
  page2 = page2.substr(0, page2.find("{\"t\":\"end\""));
  http.add("snapshot", 200, readFixture("snapshot-full.page1.jsonl"));
  http.add("snapshot", 200, page2);
  const SyncError e = runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(e.code, "incomplete");
  EXPECT_FALSE(fs->exists(BANK));
  EXPECT_FALSE(fs->exists(BANK_TMP));
}

TEST_F(SyncTest, DroppedConnectionIsAnError) {
  http.add("snapshot", 200, readFixture("snapshot-full.page1.jsonl"), /*complete=*/false);
  EXPECT_EQ(runSync(*fs, http, st, opt, stats).code, "incomplete");
}

TEST_F(SyncTest, ExpiredPageRestartsTheSnapshotOnce) {
  http.add("snapshot", 200, readFixture("snapshot-full.page1.jsonl"));
  http.add("snapshot", 410, errorBody("410 page_expired"));
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  ASSERT_EQ(http.calls.size(), 5u);
  EXPECT_EQ(http.calls[2].url.find("&page="), std::string::npos);  // from the start
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
  EXPECT_EQ(stats.received, 6);
}

TEST_F(SyncTest, UnauthorizedClearsTheTokenAndKeepsTheData) {
  put(BANK, readFixture("bank.jsonl"));
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  http.add("push", 401, errorBody("401 invalid_token"));
  const SyncError e = runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(e.kind, SyncErr::Unauthorized);
  EXPECT_EQ(errorText(e), "Pareie de novo.");
  EXPECT_FALSE(st.paired());
  SyncState loaded;
  loadState(*fs, loaded);
  EXPECT_EQ(loaded.token, "");
  EXPECT_EQ(loaded.lastError, "invalid_token");
  EXPECT_EQ(get(BANK), readFixture("bank.jsonl"));
  EXPECT_EQ(get(paths::OUTBOX), readFixture("outbox.jsonl"));
  EXPECT_EQ(runSync(*fs, http, st, opt, stats).kind, SyncErr::NotPaired);
}

TEST_F(SyncTest, ServerErrorsBackOff30s2min10min) {
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  const int64_t t0 = opt.nowMs;
  for (int64_t wait : {30000LL, 120000LL, 600000LL, 600000LL}) {
    http.add("push", 500, errorBody("500 server_error"));
    const SyncError e = runSync(*fs, http, st, opt, stats);
    ASSERT_EQ(e.kind, SyncErr::Server);
    EXPECT_EQ(st.retryAtMs, opt.nowMs + wait);
    // too early: nothing is sent
    const size_t calls = http.calls.size();
    opt.nowMs += wait - 1;
    EXPECT_EQ(runSync(*fs, http, st, opt, stats).kind, SyncErr::Backoff);
    EXPECT_EQ(http.calls.size(), calls);
    opt.nowMs += 1;
  }
  EXPECT_EQ(get(paths::OUTBOX), readFixture("outbox.jsonl"));
  // success resets the backoff
  queuePush();
  queueFullSnapshot();
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(st.failures, 0);
  EXPECT_EQ(st.retryAtMs, 0);
  EXPECT_GT(opt.nowMs, t0);
}

TEST_F(SyncTest, RateLimitWaitsForRetryAfter) {
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  http.add("push", 429, errorBody("429 rate_limited"), true, 90);
  EXPECT_EQ(runSync(*fs, http, st, opt, stats).kind, SyncErr::RateLimited);
  EXPECT_EQ(st.retryAtMs, opt.nowMs + 90000);
  // without the header, the body's retryAfter (60 s)
  opt.nowMs += 90000;
  http.add("push", 429, errorBody("429 rate_limited"));
  runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(st.retryAtMs, opt.nowMs + 60000);
}

TEST_F(SyncTest, ProtocolUnsupported) {
  http.add("snapshot", 426, errorBody("426 protocol_unsupported"));
  const SyncError e = runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(e.kind, SyncErr::Protocol);
  EXPECT_EQ(errorText(e), "Atualize o firmware.");
}

TEST_F(SyncTest, NoResponseKeepsEverything) {
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  http.add("push", -1, "", false);
  const SyncError e = runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(e.kind, SyncErr::Network);
  EXPECT_EQ(st.retryAtMs, 0);  // only 5xx and 429 back off
  EXPECT_EQ(get(paths::OUTBOX), readFixture("outbox.jsonl"));
}

TEST_F(SyncTest, ResultCountMismatchResolvesNothing) {
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  JsonDocument resp = loadJsonFixture("push.response.json");
  resp["results"].as<JsonArray>().remove(6);
  http.add("push", 200, jsonOf(resp));
  const SyncError e = runSync(*fs, http, st, opt, stats);
  EXPECT_EQ(e.code, "results_mismatch");
  EXPECT_EQ(get(paths::OUTBOX), readFixture("outbox.jsonl"));
  EXPECT_FALSE(fs->exists(paths::REJECTED));
  EXPECT_EQ(http.calls.size(), 1u);  // no snapshot after a failed push
}

// Builds `n` seen ops and answers each push with "applied" for every op it got.
struct EchoServer {
  std::vector<int> batchSizes;
  int tooLargeAbove = 1 << 30;
  HttpResponse operator()(const FakeHttp::Call& c) {
    HttpResponse r;
    r.complete = true;
    if (FakeHttp::route(c.url) == "snapshot") {
      r.status = 200;
      r.body = readFixture("snapshot-delta.jsonl");
      r.body.replace(r.body.find("\"mode\":\"delta\""), 14, "\"mode\":\"full\"");
      return r;
    }
    JsonDocument req;
    deserializeJson(req, c.body);
    JsonArrayConst ops = req["ops"].as<JsonArrayConst>();
    if (static_cast<int>(ops.size()) > tooLargeAbove) {
      r.status = 413;
      r.body = errorBody("413 payload_too_large");
      return r;
    }
    batchSizes.push_back(static_cast<int>(ops.size()));
    JsonDocument resp;
    resp["protocol"] = 1;
    JsonArray results = resp["results"].to<JsonArray>();
    for (JsonObjectConst op : ops) {
      JsonObject res = results.add<JsonObject>();
      res["clientId"] = op["clientId"];
      res["status"] = "applied";
    }
    r.status = 200;
    serializeJson(resp, r.body);
    return r;
  }
};

std::string seenOps(int n) {
  std::string out;
  for (int i = 0; i < n; i++) {
    char id[40];
    snprintf(id, sizeof id, "00000000-0000-4000-8000-%012d", i);
    out += buildSeen(id, 1762900000000LL + i, "it", "bello") + "\n";
  }
  return out;
}

TEST_F(SyncTest, PushesInBatchesOfAtMost200) {
  put(paths::OUTBOX, seenOps(450));
  EchoServer server;
  http.handler = [&](const FakeHttp::Call& c) { return server(c); };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(server.batchSizes, (std::vector<int>{200, 200, 50}));
  EXPECT_EQ(stats.pushed, 450);
  EXPECT_EQ(get(paths::OUTBOX), "");
}

TEST_F(SyncTest, BatchesAlsoRespectTheByteBudget) {
  put(paths::OUTBOX, seenOps(30));
  opt.maxBatchBytes = 1500;  // ~10 seen ops
  EchoServer server;
  http.handler = [&](const FakeHttp::Call& c) { return server(c); };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  ASSERT_GT(server.batchSizes.size(), 2u);
  for (int n : server.batchSizes) EXPECT_LE(n * 120, 1500 + 120);
}

TEST_F(SyncTest, PayloadTooLargeSplitsTheBatch) {
  put(paths::OUTBOX, seenOps(10));
  EchoServer server;
  server.tooLargeAbove = 3;
  http.handler = [&](const FakeHttp::Call& c) { return server(c); };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  int sum = 0;
  for (int n : server.batchSizes) {
    EXPECT_LE(n, 3);
    sum += n;
  }
  EXPECT_EQ(sum, 10);
  EXPECT_EQ(get(paths::OUTBOX), "");
}

TEST_F(SyncTest, CsvDeckGoesUpAsDeckImportOnceAndIsHiddenAfterwards) {
  fs->mkdirs(paths::TSV_DIR);
  put("/flashcards/Viaggio.csv", "front,back\nciao,oi\ngrazie,obrigado\n");
  int n = 0;
  opt.newId = [&] { return "aaaaaaaa-0000-4000-8000-00000000000" + std::to_string(++n); };
  EchoServer server;
  std::vector<std::string> pushed;
  http.handler = [&](const FakeHttp::Call& c) {
    HttpResponse r = server(c);
    if (FakeHttp::route(c.url) != "push") return r;
    JsonDocument req, resp;
    deserializeJson(req, c.body);
    deserializeJson(resp, r.body);
    for (size_t i = 0; i < req["ops"].size(); i++) {
      pushed.push_back(req["ops"][i]["type"].as<std::string>());
      if (req["ops"][i]["type"] == "deckImport") {
        resp["results"][i]["deckId"] = "deck-viaggio";
        resp["results"][i]["created"] = true;
        resp["results"][i]["fichas"] = 2;
      }
    }
    r.body.clear();
    serializeJson(resp, r.body);
    return r;
  };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(pushed, (std::vector<std::string>{"deckImport"}));
  EXPECT_EQ(stats.decksQueued, 1);
  EXPECT_EQ(stats.decksImported, 1);
  ASSERT_EQ(stats.deckImports.size(), 1u);
  EXPECT_EQ(stats.deckImports.begin()->second.deckId, "deck-viaggio");
  EXPECT_EQ(stats.deckImports.begin()->second.fichas, 2);
  EXPECT_TRUE(get(paths::OUTBOX).empty());
  EXPECT_EQ(deckimport::imported(*fs), (std::set<std::string>{"Viaggio"}));

  // The next sync sends nothing for the same file.
  pushed.clear();
  stats = SyncStats{};
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_TRUE(pushed.empty());
  EXPECT_EQ(stats.decksQueued, 0);
}

TEST_F(SyncTest, OpsAddedAfterThePushStay) {
  put(paths::OUTBOX, seenOps(3));
  EchoServer server;
  bool appended = false;
  http.handler = [&](const FakeHttp::Call& c) {
    if (FakeHttp::route(c.url) == "snapshot" && !appended) {
      appended = true;
      appendOutbox(*fs, buildSeen("11111111-1111-4111-8111-111111111111", 1762900000000LL, "it", "mare"));
    }
    return server(c);
  };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  const auto rest = lines(get(paths::OUTBOX));
  ASSERT_EQ(rest.size(), 1u);
  EXPECT_NE(rest[0].find("11111111-1111"), std::string::npos);
}

TEST_F(SyncTest, RejectedLogKeepsTheLast100) {
  put(paths::OUTBOX, seenOps(120));
  http.handler = [&](const FakeHttp::Call& c) {
    EchoServer s;
    HttpResponse r = s(c);
    if (FakeHttp::route(c.url) == "push") {
      size_t p = 0;
      while ((p = r.body.find("\"applied\"", p)) != std::string::npos) r.body.replace(p, 9, "\"rejected\"");
    }
    return r;
  };
  ASSERT_TRUE(runSync(*fs, http, st, opt, stats).ok());
  EXPECT_EQ(stats.rejected, 120);
  EXPECT_EQ(lines(get(paths::REJECTED)).size(), 100u);
}

TEST_F(SyncTest, AdjustedCardsComparesLocalAndServerSchedules) {
  put(BANK, readFixture("bank.jsonl"));
  put(paths::OUTBOX, readFixture("outbox.jsonl"));
  LocalView before;
  before.load(*fs, "it");
  ASSERT_GT(before.size(), 0u);
  std::map<std::string, int64_t> due;
  int idx = -1;
  for (size_t i = 0; i < before.size(); i++) {
    if (!before.entry(i).cardId.empty()) idx = static_cast<int>(i);
  }
  ASSERT_GE(idx, 0);
  const ViewEntry& e = before.entry(static_cast<size_t>(idx));
  due[e.seed()] = e.card.dueMs + 30000;  // within a minute: same schedule
  EXPECT_EQ(countAdjusted(before, due), 0);
  due[e.seed()] = e.card.dueMs + 86400000;
  EXPECT_EQ(countAdjusted(before, due), 1);
}

TEST(SyncText, UrlEncode) {
  EXPECT_EQ(urlEncode("v1.Mj+/=a b"), "v1.Mj%2B%2F%3Da%20b");
  EXPECT_EQ(urlEncode("p1.eyJsIjoiaXQi"), "p1.eyJsIjoiaXQi");
}

TEST(SyncText, ErrorsFromFixtures) {
  JsonDocument errors = loadJsonFixture("errors.json");
  for (JsonPairConst kv : errors.as<JsonObjectConst>()) {
    HttpResponse r;
    r.status = atoi(kv.key().c_str());
    r.complete = true;
    r.body = jsonOf(kv.value());
    const SyncError e = errorFromResponse(r, 1000);
    EXPECT_EQ(e.code, kv.value()["code"].as<std::string>());
    EXPECT_EQ(e.message, kv.value()["message"].as<std::string>());
    EXPECT_FALSE(errorText(e).empty());
  }
}

}  // namespace
