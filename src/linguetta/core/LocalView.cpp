#include "LocalView.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>

#include "Ops.h"
#include "Store.h"
#include "Text.h"

namespace linguetta {

namespace {

std::string s(JsonVariantConst v) { return v.is<const char*>() ? std::string(v.as<const char*>()) : std::string(); }

bool fsrsMatches(JsonObjectConst f) {
  if (f.isNull()) return true;
  auto steps = [](JsonArrayConst a, std::initializer_list<const char*> want) {
    if (a.size() != want.size()) return false;
    size_t i = 0;
    for (const char* w : want) {
      if (s(a[i++]) != w) return false;
    }
    return true;
  };
  // desiredRetention is the account's choice and is followed (settings.desiredRetention).
  return s(f["version"]) == "FSRS-7" && (f["enableFuzz"] | true) && steps(f["learningSteps"], {"1m", "10m"}) &&
         steps(f["relearningSteps"], {"10m"}) && !(f["allowModelOverride"] | true) &&
         (f["maximumInterval"] | 36500) == 36500;
}

}  // namespace

void LocalView::load(Fs& fs, const std::string& language) {
  hdr = BankHeader{};
  entries.clear();
  byKey.clear();
  byCardId.clear();
  bySaveId.clear();
  newReviewTimes.clear();
  extras.clear();
  opOrder = 0;
  lang = language;

  const std::string path = bankPath(lang);
  if (auto f = path.empty() ? nullptr : fs.open(path, OpenMode::Read)) {
    LineReader reader(*f);
    std::string line;
    bool complete;
    bool first = true;
    while (reader.next(line, complete)) {
      if (reader.overlong() || line.empty()) continue;
      if (first) {
        first = false;
        parseHeader(line);
        // A file whose header names another language is not this deck's bank.
        if (hdr.language != lang) {
          hdr = BankHeader{};
          break;
        }
        continue;
      }
      addBankLine(line, static_cast<int64_t>(reader.lineOffset()));
    }
  }

  std::vector<PendingReview> reviews;
  forEachOutbox(fs, [&](const OutboxLine& l) {
    applyOpParsed(l.json, &reviews);
    return true;
  });
  // Reviews of one card apply in order of `at` (ties: file order).
  std::stable_sort(reviews.begin(), reviews.end(),
                   [](const PendingReview& a, const PendingReview& b) { return a.atMs < b.atMs; });
  for (const auto& r : reviews) applyReview(r.entry, r.atMs, r.rating);
}

void LocalView::applyOp(const std::string& jsonLine) { applyOpParsed(jsonLine, nullptr); }

void LocalView::parseHeader(const std::string& line) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok || s(doc["t"]) != "header") return;
  hdr.present = true;
  hdr.language = s(doc["language"]);
  hdr.deckName = s(doc["deckName"]);
  hdr.cursor = s(doc["cursor"]);
  JsonObjectConst st = doc["settings"];
  // "newFlashcardsPerDay" since the flashcard rename; older banks said "newCardsPerDay".
  hdr.newPerDay = st["newFlashcardsPerDay"] | (st["newCardsPerDay"] | 10);
  hdr.desiredRetention = st["desiredRetention"] | (doc["fsrs"]["desiredRetention"] | fsrs::DEFAULT_RETENTION);
  hdr.buryRelated = st["buryRelated"] | true;
  hdr.leechLapses = st["leech"]["lapses"] | 0;
  const std::string action = s(st["leech"]["action"]);
  hdr.leechAction = action == "tag" ? LeechAction::Tag : action == "suspend" ? LeechAction::Suspend : LeechAction::Wait;
  JsonObjectConst today = doc["today"];
  hdr.dayStartMs = parseIsoOr(today["dayStart"], 0);
  hdr.nextDayStartMs = parseIsoOr(today["nextDayStart"], 0);
  hdr.newDoneToday = today["newDoneToday"] | 0;
  hdr.extraNew = today["extraNew"] | 0;
  hdr.newAllowance = today["newAllowance"].is<int>() ? std::max(0, today["newAllowance"].as<int>()) : -1;
  std::vector<std::string> parentIds;
  for (JsonObjectConst d : doc["decks"].as<JsonArrayConst>()) {
    DeckInfo info;
    info.id = s(d["id"]);
    info.name = s(d["name"]);
    info.isDefault = d["default"] | false;
    if (info.id.empty()) continue;
    if (info.isDefault && hdr.defaultDeck < 0) hdr.defaultDeck = static_cast<int>(hdr.decks.size());
    parentIds.push_back(s(d["parentId"]));
    hdr.decks.push_back(std::move(info));
  }
  for (size_t i = 0; i < hdr.decks.size(); i++) {
    if (parentIds[i].empty()) continue;
    const int p = deckIndex(parentIds[i].c_str());
    if (p >= 0 && static_cast<size_t>(p) != i) hdr.decks[i].parent = p;
  }
  hdr.fichasTotal = doc["fichasTotal"] | 0;
  hdr.fsrsApproximate = !fsrsMatches(doc["fsrs"]);
}

int16_t LocalView::deckIndex(const char* deckId) const {
  if (hdr.decks.empty()) return DECK_NONE;
  if (!deckId || !*deckId) return hdr.defaultDeck >= 0 ? static_cast<int16_t>(hdr.defaultDeck) : DECK_OFF;
  for (size_t i = 0; i < hdr.decks.size(); i++) {
    if (hdr.decks[i].id == deckId) return static_cast<int16_t>(i);
  }
  return DECK_OFF;
}

void LocalView::addBankLine(const std::string& line, int64_t offset) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return;
  const std::string t = s(doc["t"]);
  const bool ficha = t == "ficha";
  if (!ficha && t != "item") return;
  ViewEntry e;
  e.ficha = ficha;
  e.bankOffset = offset;
  const std::string status = s(doc["status"]);
  e.known = status == "conhecido";
  e.active = status.empty() || status == "ativo";
  e.deck = deckIndex(doc["deckId"].as<const char*>());
  e.queueRank = doc["queueRank"].is<int>() ? std::max(0, doc["queueRank"].as<int>()) : 0;
  e.leechWaiting = doc["leechWaiting"] | false;
  e.siblingMs = parseIsoOr(doc["siblingReviewedAt"], -1);
  // "flashcard" since the server rename; banks written before it say "card".
  JsonObjectConst card = doc["flashcard"].isNull() ? doc["card"] : doc["flashcard"];
  const bool hasCard = cardFromJson(card, e.card, e.cardId, e.cardCreatedMs);
  if (ficha) {
    // A ficha is only its flashcard; it never joins the lemma index (Word Lens, saves).
    if (!hasCard || byCardId.count(e.cardId)) return;
  } else {
    e.key = lowerKey(s(doc["lemma"]));
    if (e.key.empty() || byKey.count(e.key)) return;
    if (!hasCard) {
      e.card = fsrs::newCard(parseIsoOr(doc["createdAt"], 0));
      e.cardCreatedMs = e.card.dueMs;
    }
    e.seenCount = doc["seenCount"] | 0;
  }
  const int idx = static_cast<int>(entries.size());
  if (!ficha) byKey[e.key] = idx;
  if (!e.cardId.empty()) byCardId[e.cardId] = idx;
  entries.push_back(std::move(e));
}

int LocalView::applyOpParsed(const std::string& line, std::vector<PendingReview>* deferred) {
  // deckImport rows are not part of the view (the deck arrives with the next snapshot).
  if (line.find("\"type\":\"deckImport\"") != std::string::npos) return -1;
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return -1;
  if (s(doc["language"]) != lang) return -1;
  const std::string type = s(doc["type"]);
  int64_t at = 0;
  if (!parseIso(doc["at"] | "", at)) return -1;

  if (type == "save") {
    const std::string lemma = s(doc["lemma"]);
    const std::string key = lowerKey(trim(lemma));
    if (key.empty() || codePointCount(trim(lemma)) > limits::LEMMA) return -1;  // the server rejects it
    const std::string clientId = s(doc["clientId"]);
    auto enc = std::make_shared<ItemDetails>();
    enc->lemma = lemma;
    enc->sense = s(doc["sense"]);
    enc->note = s(doc["note"]);
    enc->ipa = s(doc["ipa"]);
    enc->pos = s(doc["pos"]);
    JsonObjectConst e = doc["encounter"];
    enc->encounter.form = s(e["form"]);
    enc->encounter.sentence = s(e["sentence"]);
    enc->encounter.sense = s(e["sense"]);
    enc->encounter.chapter = s(e["position"]["chapter"]);
    enc->encounter.work = s(doc["work"]["title"]);
    enc->hasEncounter = true;
    auto it = byKey.find(key);
    int idx;
    if (it != byKey.end()) {
      idx = it->second;
      entries[idx].local = enc;  // newer encounter; empty item fields are filled when shown
    } else {
      ViewEntry v;
      v.key = key;
      v.saveClientId = clientId;
      v.card = fsrs::newCard(at);
      v.cardCreatedMs = at;
      v.local = enc;
      // A new item lands in the language's default deck ("Minhas palavras").
      v.deck = hdr.defaultDeck >= 0 ? static_cast<int16_t>(hdr.defaultDeck) : DECK_NONE;
      idx = static_cast<int>(entries.size());
      byKey[key] = idx;
      entries.push_back(std::move(v));
    }
    // An encounter with a sentence takes the item out of the teimosa wait.
    if (!enc->encounter.sentence.empty()) {
      entries[idx].leechWaiting = false;
      entries[idx].lastSaveMs = std::max(entries[idx].lastSaveMs, at);
    }
    if (!clientId.empty()) bySaveId[clientId] = idx;
    return idx;
  }

  if (type == "extraNew") {
    const int n = doc["n"] | 0;
    if (n >= 1 && n <= limits::EXTRA_NEW) extras.push_back({at, n});
    return -1;
  }

  if (type == "seen") {
    auto it = byKey.find(lowerKey(s(doc["lemma"])));
    if (it == byKey.end()) return -1;
    entries[it->second].seenCount++;
    return it->second;
  }

  if (type == "review") {
    int idx = -1;
    const std::string cardId = doc["flashcardId"].isNull() ? s(doc["cardId"]) : s(doc["flashcardId"]);
    if (!cardId.empty()) {
      auto it = byCardId.find(cardId);
      if (it != byCardId.end()) idx = it->second;
    } else {
      auto it = bySaveId.find(s(doc["saveClientId"]));
      if (it != bySaveId.end()) idx = it->second;
    }
    const int rating = doc["rating"] | 0;
    if (idx < 0 || rating < 1 || rating > 4) return -1;
    if (deferred) {
      deferred->push_back({idx, at, rating, opOrder++});
    } else {
      applyReview(idx, at, rating);
    }
    return idx;
  }
  return -1;
}

void LocalView::applyReview(int idx, int64_t atMs, int rating) {
  ViewEntry& e = entries[idx];
  if (e.card.state == 0) newReviewTimes.push_back(atMs);
  const int lapsesBefore = e.card.lapses;
  e.card = fsrs::review(e.card, e.seed(), rating, atMs, hdr.desiredRetention);
  // Teimosa (settings.leech): the lapse that reaches the limit takes it out of the queue until
  // a new encounter; the server sets leechWaiting on the same review.
  if (hdr.leechLeavesQueue() && e.card.lapses > lapsesBefore && e.card.lapses >= hdr.leechLapses &&
      atMs > e.lastSaveMs)
    e.leechWaiting = true;
}

int LocalView::find(const std::string& lemma) const {
  auto it = byKey.find(lowerKey(trim(lemma)));
  return it == byKey.end() ? -1 : it->second;
}

bool LocalView::details(Fs& fs, size_t i, ItemDetails& out) const {
  if (i >= entries.size()) return false;
  const ViewEntry& e = entries[i];
  out = ItemDetails{};
  if (e.bankOffset >= 0) {
    auto f = fs.open(bankPath(lang), OpenMode::Read);
    if (!f || !f->seek(static_cast<uint64_t>(e.bankOffset))) return false;
    LineReader reader(*f);
    std::string line;
    bool complete;
    if (!reader.next(line, complete)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return false;
    if (e.ficha) {
      out.ficha = true;
      out.cloze = s(doc["kind"]) == "lacuna";
      out.lemma = s(doc["front"]);
      out.sense = s(doc["back"]);
      return true;
    }
    out.lemma = s(doc["lemma"]);
    out.sense = s(doc["sense"]);
    out.note = s(doc["note"]);
    out.ipa = s(doc["ipa"]);
    out.pos = s(doc["pos"]);
    JsonObjectConst enc = doc["encounters"][0];
    if (!enc.isNull()) {
      out.encounter.form = s(enc["form"]);
      out.encounter.sentence = s(enc["sentence"]);
      out.encounter.sense = s(enc["sense"]);
      out.encounter.work = s(enc["work"]);
      out.hasEncounter = true;
    }
  }
  if (e.local) {
    const ItemDetails& l = *e.local;
    // The server keeps what the person wrote and only fills empty fields.
    if (out.lemma.empty()) out.lemma = l.lemma;
    if (out.sense.empty()) out.sense = l.sense;
    if (out.note.empty()) out.note = l.note;
    if (out.ipa.empty()) out.ipa = l.ipa;
    if (out.pos.empty()) out.pos = l.pos;
    out.encounter = l.encounter;
    out.hasEncounter = true;
  }
  return true;
}

int64_t LocalView::dayStart(int64_t nowMs) const {
  if (hdr.nextDayStartMs > 0 && nowMs >= hdr.nextDayStartMs) {
    const int64_t k = (nowMs - hdr.nextDayStartMs) / fsrs::MS_PER_DAY;
    return hdr.nextDayStartMs + k * fsrs::MS_PER_DAY;
  }
  if (hdr.dayStartMs > 0) return hdr.dayStartMs;
  return nowMs - (nowMs % fsrs::MS_PER_DAY);  // no bank yet: UTC day
}

int64_t LocalView::nextDayStart(int64_t nowMs) const {
  const int64_t start = dayStart(nowMs);
  // The server's nextDayStart is exact across DST; later days step by 24 h.
  if (hdr.nextDayStartMs > start) return hdr.nextDayStartMs;
  return start + fsrs::MS_PER_DAY;
}

int LocalView::newDoneToday(int64_t nowMs) const {
  const int64_t start = dayStart(nowMs);
  int n = (hdr.nextDayStartMs > 0 && nowMs >= hdr.nextDayStartMs) ? 0 : hdr.newDoneToday;
  for (int64_t t : newReviewTimes) {
    if (t >= start) n++;
  }
  return n;
}

int LocalView::extraToday(int64_t nowMs) const {
  const int64_t start = dayStart(nowMs);
  int n = 0;
  for (const Extra& x : extras) {
    if (x.atMs >= start) n += x.n;
  }
  return n;
}

int LocalView::newAllowance(int64_t nowMs) const {
  const int64_t start = dayStart(nowMs);
  const bool stale = hdr.nextDayStartMs > 0 && nowMs >= hdr.nextDayStartMs;
  int offline = 0;
  for (int64_t t : newReviewTimes) {
    if (t >= start) offline++;
  }
  // Section 8: newAllowance + the day's extraNew ops − new cards reviewed offline. After
  // nextDayStart the day resets to newFlashcardsPerDay.
  int base;
  if (hdr.serverQueue()) {
    base = stale ? hdr.newPerDay : hdr.newAllowance;
  } else {
    base = hdr.newPerDay - (stale ? 0 : hdr.newDoneToday);
  }
  return std::max(0, base + extraToday(nowMs) - offline);
}

int LocalView::newLimit(int64_t nowMs) const {
  const bool stale = hdr.nextDayStartMs > 0 && nowMs >= hdr.nextDayStartMs;
  return hdr.newPerDay + (stale ? 0 : hdr.extraNew) + extraToday(nowMs);
}

bool LocalView::inDeck(const ViewEntry& e, int deck) const {
  if (deck == DECK_ALL) return true;
  if (e.deck < 0) return false;
  return e.deck == deck || hdr.decks[e.deck].parent == deck;
}

bool LocalView::reviewable(const ViewEntry& e) const {
  if (e.known || !e.active || e.leechWaiting || e.deck == DECK_OFF) return false;
  return !(e.cardId.empty() && e.saveClientId.empty());  // no flashcard to review
}

bool LocalView::buried(const ViewEntry& e, int64_t dayStartMs) const {
  // Siblings (section 11): review or new waits when the item's Produzir was reviewed today;
  // learning steps are minutes and do not wait.
  return hdr.buryRelated && e.siblingMs >= dayStartMs && (e.card.state == 0 || e.card.state == 2);
}

std::vector<int> LocalView::queue(int64_t nowMs, int deck) const {
  std::vector<int> due, ranked, unranked;
  const int64_t day = dayStart(nowMs);
  const int64_t dayEnd = nextDayStart(nowMs);
  const bool server = hdr.serverQueue();
  for (size_t i = 0; i < entries.size(); i++) {
    const ViewEntry& e = entries[i];
    if (!reviewable(e) || !inDeck(e, deck) || buried(e, day)) continue;
    const int idx = static_cast<int>(i);
    if (e.card.state > 0) {
      if (fsrs::isDue(e.card, nowMs, dayEnd)) due.push_back(idx);
    } else if (!server) {
      unranked.push_back(idx);
    } else if (e.queueRank > 0) {
      ranked.push_back(idx);
    } else if (e.bankOffset < 0) {
      unranked.push_back(idx);  // saved offline: after the ranked ones
    }
    // A bank card with queueRank null does not start today.
  }
  std::stable_sort(due.begin(), due.end(), [this](int a, int b) { return entries[a].card.dueMs < entries[b].card.dueMs; });
  std::stable_sort(ranked.begin(), ranked.end(),
                   [this](int a, int b) { return entries[a].queueRank < entries[b].queueRank; });
  std::stable_sort(unranked.begin(), unranked.end(),
                   [this](int a, int b) { return entries[a].cardCreatedMs < entries[b].cardCreatedMs; });
  ranked.insert(ranked.end(), unranked.begin(), unranked.end());
  const int allowance = newAllowance(nowMs);
  if (static_cast<int>(ranked.size()) > allowance) ranked.resize(allowance);
  due.insert(due.end(), ranked.begin(), ranked.end());
  return due;
}

int LocalView::newBeyondBudget(int64_t nowMs, int deck) const {
  const int64_t day = dayStart(nowMs);
  int candidates = 0;
  for (const ViewEntry& e : entries) {
    if (!reviewable(e) || !inDeck(e, deck) || buried(e, day) || e.card.state != 0) continue;
    if (!hdr.serverQueue() || e.queueRank > 0 || e.bankOffset < 0) candidates++;
  }
  return std::max(0, candidates - newAllowance(nowMs));
}

int LocalView::cardCount(int deck) const {
  int n = 0;
  for (const ViewEntry& e : entries) {
    if (e.known || !e.active || e.deck == DECK_OFF || !inDeck(e, deck)) continue;
    if (e.cardId.empty() && e.saveClientId.empty()) continue;
    n++;
  }
  return n;
}

LocalView::Counts LocalView::counts(int64_t nowMs, int deck) const {
  Counts c;
  for (int i : queue(nowMs, deck)) {
    if (entries[i].card.state > 0) {
      c.due++;
    } else {
      c.fresh++;
    }
  }
  return c;
}

}  // namespace linguetta
