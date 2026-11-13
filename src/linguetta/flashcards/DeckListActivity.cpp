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

namespace {
enum : int { ID_BACK = 9000, ID_HISTORY = 9001, ID_PREV = 9002, ID_NEXT = 9003, ID_SYNC = 9004 };

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

constexpr int SUBDECK_INDENT = 28;
}  // namespace

std::unique_ptr<Activity> makeDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<DeckListActivity>(renderer, mappedInput);
}

void DeckListActivity::onEnter() {
  Activity::onEnter();
  reload();
  for (size_t i = 0; i < decks.size(); i++) {
    if (!decks[i].section) {
      setFocus(static_cast<int>(i));
      break;
    }
  }
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
      d.indent = hdr.decks[k].parent >= 0 ? SUBDECK_INDENT : 0;
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
}

void DeckListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  clearTargets();
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  target(ui::header(renderer, "Flashcards"), ID_BACK, false);

  int y = ui::headerH() + 18;
  if (clockOk) {
    const std::string title = (total == 0 ? std::string("Nada") : std::to_string(total)) + " para hoje";
    renderer.drawText(ui::fontTitle(), ui::MARGIN, y, title.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(ui::fontTitle());
  } else {
    renderer.drawText(ui::fontTitle(), ui::MARGIN, y, "Rel\xC3\xB3gio n\xC3\xA3o acertado", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(ui::fontTitle());
    y = ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y, w - 2 * ui::MARGIN,
                      "Conecte ao Wi-Fi (Sincronizar) para acertar a hora antes de revisar.", 2);
  }
  // No "Decks" label: the title says what is due and the rows need the room.
  y += 12;

  // Bottom block: status line, "Navegação", Sincronizar and Histórico.
  const int navH = ui::sectionH(renderer) + 2 * ui::ROW_H + ui::ROW_GAP + 20;
  const int statusH = status.empty() ? 0 : renderer.getLineHeight(ui::fontSmall()) * 2 + 6;
  const int listBottom = h - navH - statusH;
  const int pagerH = 44 + 8;
  const int count = static_cast<int>(decks.size());
  auto rowH = [&](int i) { return decks[i].section ? ui::sectionH(renderer) : ui::ROW2_H + ui::ROW_GAP; };

  // Pages by height; with more than one page the pager takes the bottom of the list.
  auto paginate = [&](int bottom) {
    pageStart.assign(1, 0);
    int used = 0;
    for (int i = 0; i < count; i++) {
      const int rh = rowH(i);
      // A section label never ends a page.
      const int need = rh + (decks[i].section && i + 1 < count ? rowH(i + 1) : 0);
      // The last row's gap may run into the bottom margin.
      if (used > 0 && used + need > bottom - y + ui::ROW_GAP) {
        pageStart.push_back(i);
        used = 0;
      }
      used += rh;
    }
  };
  paginate(listBottom);
  if (pageStart.size() > 1) paginate(listBottom - pagerH);
  pages = static_cast<int>(pageStart.size());
  page = std::clamp(page, 0, std::max(0, pages - 1));

  const int first = pageStart[page], last = page + 1 < pages ? pageStart[page + 1] : count;
  for (int i = first; i < last; i++) {
    const Deck& d = decks[i];
    if (d.section) {
      y = ui::section(renderer, y, d.name);
      continue;
    }
    const ui::Box b = target(ui::row2(renderer, y, d.name, deckSummary(d), true, d.indent, d.all), i);
    if (focused(i)) drawFocus(b);
    y += ui::ROW2_H + ui::ROW_GAP;
  }
  if (pages > 1) {
    ui::Box prev, next;
    const std::string label = std::to_string(page + 1) + " / " + std::to_string(pages);
    ui::pager(renderer, listBottom - 44 - 4, label, prev, next);
    target(prev, ID_PREV, false);
    target(next, ID_NEXT, false);
  }

  int by = listBottom;
  if (!status.empty()) {
    ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, by, w - 2 * ui::MARGIN, status, 2, true, EpdFontFamily::BOLD);
    by += statusH;
  }
  by = ui::section(renderer, by, "Navega\xC3\xA7\xC3\xA3o");
  const std::string syncRight = !paired ? "Parear" : queued > 0 ? std::to_string(queued) + " na fila" : "";
  const ui::Box sb = target(ui::row(renderer, by, "Sincronizar", syncRight), ID_SYNC);
  if (focused(ID_SYNC)) drawFocus(sb);
  by += ui::ROW_H + ui::ROW_GAP;
  const ui::Box hb = target(ui::row(renderer, by, "Hist\xC3\xB3rico"), ID_HISTORY);
  if (focused(ID_HISTORY)) drawFocus(hb);

  renderer.displayBuffer(firstRender ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}

bool DeckListActivity::onSideButton(MappedInputManager::Button b) {
  if (b != MappedInputManager::Button::Left && b != MappedInputManager::Button::Right) return false;
  if (pages <= 1) return false;
  page = (page + (b == MappedInputManager::Button::Right ? 1 : pages - 1)) % pages;
  int f = page < static_cast<int>(pageStart.size()) ? pageStart[page] : 0;
  while (f < static_cast<int>(decks.size()) && decks[f].section) f++;
  setFocus(f);
  requestUpdate();
  return true;
}

void DeckListActivity::onAction(int id) {
  if (id == ID_BACK) {
    onBack();
  } else if (id == ID_PREV || id == ID_NEXT) {
    onSideButton(id == ID_NEXT ? MappedInputManager::Button::Right : MappedInputManager::Button::Left);
  } else if (id == ID_SYNC) {
    status.clear();
    auto act = makeUniqueNoThrow<SyncActivity>(renderer, mappedInput);
    if (act) {
      startActivityForResult(std::move(act), [this](const ActivityResult&) {
        reload();
        firstRender = true;
        requestUpdate();
      });
    }
  } else if (id == ID_HISTORY) {
    status.clear();
    auto act = makeUniqueNoThrow<HistoryActivity>(renderer, mappedInput);
    if (act) startActivityForResult(std::move(act), [this](const ActivityResult&) { requestUpdate(); });
  } else if (id >= 0 && id < static_cast<int>(decks.size()) && !decks[id].section) {
    setFocus(id);
    openDeck(static_cast<size_t>(id));
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
    firstRender = true;
    requestUpdate();
  });
}

void DeckListActivity::onBack() { onGoHome(HomeMenuItem::FLASHCARDS); }

}  // namespace linguetta
