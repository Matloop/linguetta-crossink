#include "DeckListActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <set>

#include "HistoryActivity.h"
#include "ReviewActivity.h"
#include "SyncActivity.h"
#include "activities/ActivityManager.h"
#include "linguetta/core/DeckImport.h"
#include "linguetta/core/Decks.h"
#include "linguetta/core/LocalView.h"
#include "linguetta/core/Store.h"
#include "linguetta/core/Text.h"
#include "linguetta/device/Linguetta.h"

namespace linguetta {

namespace fui = freeink::ui;

namespace {
std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + (n == 1 ? one : many); }

// "58 cartões · 20 para hoje" (or "· nada para hoje"); "Estudar tudo": "14 para hoje · 4 novas de 10".
std::string deckSummary(const DeckListActivity::Deck& d) {
  if (d.all) {
    if (d.count < 0) return plural(d.cards, " cart\xC3\xA3o", " cart\xC3\xB5" "es");
    std::string s = (d.count == 0 ? std::string("nada") : std::to_string(d.count)) + " para hoje";
    if (d.limit > 0) s += " \xC2\xB7 " + std::to_string(d.fresh) + (d.fresh == 1 ? " nova de " : " novas de ") +
                          std::to_string(d.limit);
    return s;
  }
  const std::string s = plural(d.cards, " cart\xC3\xA3o", " cart\xC3\xB5" "es");
  if (d.count < 0) return s;
  if (d.count == 0) return s + " \xC2\xB7 nada para hoje";
  return s + " \xC2\xB7 " + std::to_string(d.count) + " para hoje";
}

const char* const SUBDECK_PREFIX = "    ";  // subdecks sit under their parent
}  // namespace

std::unique_ptr<Activity> makeDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<DeckListActivity>(renderer, mappedInput);
}

void DeckListActivity::onEnter() {
  Screen::onEnter();
  reload();
  requestUpdate();
}

void DeckListActivity::reload() {
  Service& svc = Service::get();
  const int64_t now = svc.nowMs();
  clockOk = now > 0;
  decks.clear();
  total = 0;
  int tsvNewPerDay = 0;

  for (const auto& lang : svc.languages()) {
    LocalView& view = svc.view(lang);
    if (view.size() == 0) continue;
    const BankHeader& hdr = view.header();
    if (!tsvNewPerDay && hdr.present) tsvNewPerDay = hdr.newPerDay;
    if (hdr.decks.empty()) {
      // Bank from before decks: one row per language.
      Deck lg;
      lg.name = svc.deckName(lang);
      lg.linguetta = true;
      lg.language = lang;
      lg.cards = view.cardCount(DECK_ALL);
      if (clockOk) lg.count = view.counts(now).total();
      total += std::max(0, lg.count);
      decks.push_back(std::move(lg));
      continue;
    }
    Deck sec;
    sec.section = true;
    sec.name = languageName(lang);
    decks.push_back(std::move(sec));

    Deck all;
    all.name = "Estudar tudo";
    all.linguetta = true;
    all.all = true;
    all.language = lang;
    all.deck = DECK_ALL;
    all.cards = view.cardCount(DECK_ALL);
    if (clockOk) {
      const LocalView::Counts c = view.counts(now);
      all.count = c.total();
      all.fresh = c.fresh;
      all.limit = view.newLimit(now);
      total += all.count;
    }
    decks.push_back(std::move(all));

    // Top-level decks in the server order, each followed by its subdecks.
    const int n = static_cast<int>(hdr.decks.size());
    std::vector<int> order;
    for (int top = 0; top < n; top++) {
      if (hdr.decks[top].parent >= 0) continue;
      order.push_back(top);
      for (int k = 0; k < n; k++) {
        if (hdr.decks[k].parent == top) order.push_back(k);
      }
    }
    for (int k : order) {
      Deck d;
      d.name = hdr.decks[k].name;
      d.linguetta = true;
      d.language = lang;
      d.deck = k;
      d.indent = hdr.decks[k].parent >= 0 ? 1 : 0;
      d.cards = view.cardCount(k);
      if (clockOk) d.count = view.counts(now, k).total();
      decks.push_back(std::move(d));
    }
  }

  // One language with decks needs no label (the title already says what is due).
  if (std::count_if(decks.begin(), decks.end(), [](const Deck& d) { return d.section; }) == 1) {
    decks.erase(std::remove_if(decks.begin(), decks.end(), [](const Deck& d) { return d.section; }), decks.end());
  }

  // Local decks of /flashcards; the ones already sent to the account study as server decks.
  const int64_t dayStart = clockOk ? svc.dayStart("", now) : 0;
  const std::set<std::string> imported = deckimport::imported(svc.fs());
  for (const auto& name : listTsvDecks(svc.fs())) {
    if (imported.count(name)) continue;
    Deck d;
    d.name = name;
    if (clockOk) {
      TsvDeck deck;
      if (deck.load(svc.fs(), name)) {
        d.cards = static_cast<int>(deck.size());
        d.count = deck.dueCount(now, dayStart, tsvNewPerDay > 0 ? tsvNewPerDay : 10);
      }
    }
    total += std::max(0, d.count);
    decks.push_back(std::move(d));
  }
  paired = svc.state().paired();
  queued = countOutbox(svc.fs()).total();

  rows.clear();
  std::string heading;
  for (size_t i = 0; i < decks.size(); i++) {
    const Deck& d = decks[i];
    if (d.section) {
      heading = d.name;
      continue;
    }
    Row r;
    r.deck = static_cast<int>(i);
    r.label = std::string(d.indent > 0 ? SUBDECK_PREFIX : "") + d.name;
    r.subtitle = deckSummary(d);
    r.heading = std::move(heading);
    heading.clear();
    rows.push_back(std::move(r));
  }
  Row sync;
  sync.kind = Row::Kind::Sync;
  sync.label = "Sincronizar";
  sync.value = !paired ? "Parear" : queued > 0 ? std::to_string(queued) + " na fila" : "";
  sync.heading = "Navega\xC3\xA7\xC3\xA3o";
  rows.push_back(std::move(sync));
  Row history;
  history.kind = Row::Kind::History;
  history.label = "Hist\xC3\xB3rico";
  rows.push_back(std::move(history));
  selected = std::min(selected, static_cast<int>(rows.size()) - 1);
}

std::string DeckListActivity::subtitle() const {
  if (!clockOk) return "";
  return total == 0 ? "Nada hoje" : std::to_string(total) + " hoje";
}

void DeckListActivity::build(ui::Ui& ui) {
  // The first row carries what the screen needs to say before the decks: a failed start, or the
  // clock the review needs.
  const std::string notice = !status.empty() ? status : !clockOk ? "Rel\xC3\xB3gio n\xC3\xA3o acertado" : "";
  std::vector<ui::fui::ListItem> items;
  items.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    const Row& r = rows[i];
    ui::fui::ListItem item;
    item.label = r.label.c_str();
    if (!r.subtitle.empty()) item.subtitle = r.subtitle.c_str();
    if (!r.value.empty()) item.value = r.value.c_str();
    if (i == 0 && !notice.empty()) {
      item.sectionHeading = notice.c_str();
    } else if (!r.heading.empty()) {
      item.sectionHeading = r.heading.c_str();
    }
    items.push_back(item);
  }
  list(ui, items, UiListRowType::WithSubtitle, true);
}

void DeckListActivity::onRow(const int row) {
  if (row < 0 || row >= static_cast<int>(rows.size())) return;
  const Row& r = rows[row];
  if (r.kind == Row::Kind::Sync) {
    status.clear();
    auto act = makeUniqueNoThrow<SyncActivity>(renderer, mappedInput);
    if (act) {
      startActivityForResult(std::move(act), [this](const ActivityResult&) {
        reload();
        requestFullRefresh();
        requestUpdate();
      });
    }
  } else if (r.kind == Row::Kind::History) {
    status.clear();
    auto act = makeUniqueNoThrow<HistoryActivity>(renderer, mappedInput);
    if (act) startActivityForResult(std::move(act), [this](const ActivityResult&) { requestUpdate(); });
  } else {
    openDeck(static_cast<size_t>(r.deck));
  }
}

void DeckListActivity::openDeck(size_t i) {
  if (!clockOk && Service::get().nowMs() <= 0) {
    status = "Conecte ao Wi-Fi para acertar a hora";
    requestUpdate();
    return;
  }
  status.clear();
  DeckRef ref{decks[i].linguetta, decks[i].name, decks[i].language, decks[i].deck};
  auto act = makeUniqueNoThrow<ReviewActivity>(renderer, mappedInput, std::move(ref));
  if (!act) {
    LOG_ERR("LGT", "OOM: ReviewActivity");
    return;
  }
  startActivityForResult(std::move(act), [this](const ActivityResult&) {
    reload();
    requestFullRefresh();
    requestUpdate();
  });
}

void DeckListActivity::onBack() { onGoHome(HomeMenuItem::FLASHCARDS); }

}  // namespace linguetta
