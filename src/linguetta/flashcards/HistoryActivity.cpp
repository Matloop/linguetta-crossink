#include "HistoryActivity.h"

#include <GfxRenderer.h>
#include <Memory.h>

#include <algorithm>
#include <ctime>

#include "linguetta/core/Session.h"
#include "linguetta/core/Text.h"
#include "linguetta/device/Linguetta.h"

namespace linguetta {

namespace fui = freeink::ui;

namespace {

enum : int { ID_PREV_MONTH = 1, ID_NEXT_MONTH = 2 };

const char* const GRADE_NAMES[5] = {"?", "Errei", "Dif\xC3\xAD" "cil", "Bom", "F\xC3\xA1" "cil"};

// Display clock: the trusted time when there is one, else whatever the RTC says.
int64_t wallNowMs() {
  const int64_t trusted = Service::get().nowMs();
  return trusted > 0 ? trusted : static_cast<int64_t>(time(nullptr)) * 1000;
}

std::string shortDuration(int64_t ms) {
  const int64_t s = ms / 1000;
  if (s < 60) return std::to_string(s) + " s";
  return std::to_string(s / 60) + " min";
}

}  // namespace

// ---- month list ----

void HistoryActivity::onEnter() {
  Screen::onEnter();
  nowRef = wallNowMs();
  offset = Service::get().utcOffsetMs(nowRef);
  const CivilTime c = toCivil(nowRef + offset);
  year = c.year;
  month = c.month;
  load();
  requestUpdate();
}

void HistoryActivity::load() {
  const int64_t from = monthStartMs(year, month, offset);
  const int64_t to = monthStartMs(year, month + 1, offset);
  sessions = loadHistory(Service::get().fs(), from, to);
  cardTotal = 0;
  for (const auto& s : sessions) cardTotal += s.cardCount;
  select(0);
}

void HistoryActivity::shiftMonth(int delta) {
  month += delta;
  while (month < 1) {
    month += 12;
    year--;
  }
  while (month > 12) {
    month -= 12;
    year++;
  }
  load();
  requestUpdate();
}

bool HistoryActivity::onKey(MappedInputManager::Button b) {
  using B = MappedInputManager::Button;
  if (b == B::Left || b == B::Right) {
    shiftMonth(b == B::Right ? 1 : -1);
    return true;
  }
  return false;
}

void HistoryActivity::build(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int side = sidePadding();

  // Month navigation: "‹  março 2026  ›".
  const fui::Rect nav = ui.takeTop(theme.rowHeight, theme.spaceSm);
  const int btnW = theme.rowHeight + 8;
  button(ui, fui::Rect{static_cast<int16_t>(nav.x + side), nav.y, static_cast<int16_t>(btnW), nav.height}, "<",
         ID_PREV_MONTH);
  button(ui, fui::Rect{static_cast<int16_t>(nav.right() - side - btnW), nav.y, static_cast<int16_t>(btnW), nav.height},
         ">", ID_NEXT_MONTH);
  const std::string label = monthLabel(year, month);
  ui.target().text(fui::Rect{static_cast<int16_t>(nav.x + side + btnW), nav.y,
                             static_cast<int16_t>(nav.width - 2 * (side + btnW)), nav.height},
                   label.c_str(), style(ui, 2, true, fui::TextAlign::Center));

  // Totals.
  const fui::Rect cards = ui.takeTop(static_cast<int16_t>(theme.rowHeight * 3 / 2), theme.spaceMd);
  const int cw = (cards.width - 2 * side - theme.spaceMd) / 2;
  const std::string sessionsText = std::to_string(sessions.size());
  const std::string reviewsText = std::to_string(cardTotal);
  fui::MetricCardProps card;
  card.valueText = style(ui, 2, true, fui::TextAlign::Center);
  card.captionText = style(ui, 0, false, fui::TextAlign::Center);
  card.value = sessionsText.c_str();
  card.caption = sessions.size() == 1 ? "sess\xC3\xA3o" : "sess\xC3\xB5" "es";
  fui::metricCard(ui.frame(), fui::Rect{static_cast<int16_t>(cards.x + side), cards.y, static_cast<int16_t>(cw), cards.height}, card);
  card.value = reviewsText.c_str();
  card.caption = "revis\xC3\xB5" "es";
  fui::metricCard(ui.frame(),
                  fui::Rect{static_cast<int16_t>(cards.x + side + cw + theme.spaceMd), cards.y, static_cast<int16_t>(cw),
                            cards.height},
                  card);

  if (sessions.empty()) {
    ui.centeredText("Nenhuma sess\xC3\xA3o neste m\xC3\xAAs.", ui.theme().bodyText);
    return;
  }
  std::vector<std::string> subs;
  subs.reserve(sessions.size());
  std::vector<std::string> days;
  days.reserve(sessions.size());
  std::vector<fui::ListItem> items;
  items.reserve(sessions.size());
  for (const HistorySession& s : sessions) {
    days.push_back(sessionDayLabel(s.startMs, nowRef, offset));
    subs.push_back(s.deck + " \xC2\xB7 " + std::to_string(s.cardCount) + " \xC2\xB7 " + shortDuration(s.durationMs));
    fui::ListItem item;
    item.label = days.back().c_str();
    item.subtitle = subs.back().c_str();
    items.push_back(item);
  }
  list(ui, items, UiListRowType::WithSubtitle, true);
}

void HistoryActivity::onAction(int id) {
  if (id == ID_PREV_MONTH || id == ID_NEXT_MONTH) shiftMonth(id == ID_NEXT_MONTH ? 1 : -1);
}

void HistoryActivity::onRow(const int row) {
  if (row < 0 || row >= static_cast<int>(sessions.size())) return;
  auto act = makeUniqueNoThrow<HistorySessionActivity>(renderer, mappedInput, sessions[row].offset);
  if (act) {
    startActivityForResult(std::move(act), [this](const ActivityResult&) {
      requestFullRefresh();
      requestUpdate();
    });
  }
}

// ---- one session ----

void HistorySessionActivity::onEnter() {
  Screen::onEnter();
  nowRef = wallNowMs();
  offset = Service::get().utcOffsetMs(nowRef);
  ok = loadHistorySession(Service::get().fs(), lineOffset, session);
  requestUpdate();
}

std::string HistorySessionActivity::title() const {
  return ok ? sessionDayLabel(session.startMs, nowRef, offset) : "Sess\xC3\xA3o";
}

std::string HistorySessionActivity::subtitle() const { return ""; }

void HistorySessionActivity::build(ui::Ui& ui) {
  if (!ok) {
    ui.centeredText("N\xC3\xA3o foi poss\xC3\xADvel ler esta sess\xC3\xA3o.", ui.theme().bodyText);
    return;
  }
  const int n = static_cast<int>(session.cards.size());
  const std::string sub = session.deck + " \xC2\xB7 " + std::to_string(n) + (n == 1 ? " cart\xC3\xA3o" : " cart\xC3\xB5" "es") +
                          " \xC2\xB7 " + formatDuration(session.durationMs);
  paragraph(ui, sub, 0, false, fui::TextAlign::Left, 2);
  ui.spacer(ui.theme().spaceMd);
  std::vector<std::string> senses;
  senses.reserve(session.cards.size());
  std::vector<fui::ListItem> items;
  items.reserve(session.cards.size());
  for (const HistoryCard& c : session.cards) {
    senses.push_back(c.sense);
    fui::ListItem item;
    item.label = c.lemma.c_str();
    if (!c.sense.empty()) item.subtitle = senses.back().c_str();
    item.value = GRADE_NAMES[c.grade >= 1 && c.grade <= 4 ? c.grade : 0];
    items.push_back(item);
  }
  list(ui, items, UiListRowType::WithSubtitle, true);
}

}  // namespace linguetta
