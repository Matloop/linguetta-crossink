#include "HistoryActivity.h"

#include <GfxRenderer.h>
#include <Memory.h>

#include <algorithm>
#include <ctime>

#include "linguetta/core/Session.h"
#include "linguetta/core/Text.h"
#include "linguetta/device/Linguetta.h"

namespace linguetta {

namespace {

enum : int { ID_BACK = 9000, ID_PREV_MONTH = 9001, ID_NEXT_MONTH = 9002, ID_PREV = 9003, ID_NEXT = 9004, ID_ROW = 100 };

const char* const GRADE_CHIPS[5] = {"?", "ERREI", "DIF\xC3\x8D" "CIL", "BOM", "F\xC3\x81" "CIL"};

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
  Activity::onEnter();
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
  page = 0;
  setFocus(sessions.empty() ? -1 : ID_ROW);
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

bool HistoryActivity::onSideButton(MappedInputManager::Button b) {
  using B = MappedInputManager::Button;
  if (b == B::Left || b == B::Right) {
    shiftMonth(b == B::Right ? 1 : -1);
    return true;
  }
  return false;
}

void HistoryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  clearTargets();
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  target(ui::header(renderer, "Hist\xC3\xB3rico"), ID_BACK, false);

  int y = ui::headerH() + 16;
  ui::Box prev, next;
  ui::pager(renderer, y, monthLabel(year, month), prev, next);
  target(prev, ID_PREV_MONTH, false);
  target(next, ID_NEXT_MONTH, false);
  y += 44 + 16;

  const int boxW = (w - 2 * ui::MARGIN - 12) / 2;
  ui::statBox(renderer, ui::Box{ui::MARGIN, y, boxW, 84}, std::to_string(sessions.size()),
              sessions.size() == 1 ? "sess\xC3\xA3o" : "sess\xC3\xB5" "es");
  ui::statBox(renderer, ui::Box{ui::MARGIN + boxW + 12, y, boxW, 84}, std::to_string(cardTotal), "revis\xC3\xB5" "es");
  y += 84 + 4;
  y = ui::section(renderer, y, "Sess\xC3\xB5" "es");

  if (sessions.empty()) {
    ui::message(renderer, y + 40, "Nenhuma sess\xC3\xA3o neste m\xC3\xAAs.");
  } else {
    const int pagerH = 44 + 20;
    const int step = ui::ROW2_H + ui::ROW_GAP;
    const int count = static_cast<int>(sessions.size());
    perPage = std::max(1, (h - ui::MARGIN - y) / step);
    if (count > perPage) perPage = std::max(1, (h - ui::MARGIN - pagerH - y) / step);
    const int pages = (count + perPage - 1) / perPage;
    page = std::clamp(page, 0, pages - 1);
    for (int i = page * perPage; i < std::min(count, (page + 1) * perPage); i++) {
      const HistorySession& s = sessions[i];
      const std::string sub = s.deck + " \xC2\xB7 " + std::to_string(s.cardCount) + " \xC2\xB7 " + shortDuration(s.durationMs);
      const ui::Box b = target(ui::row2(renderer, y, sessionDayLabel(s.startMs, nowRef, offset), sub), ID_ROW + i);
      if (focused(ID_ROW + i)) drawFocus(b);
      y += step;
    }
    if (pages > 1) {
      ui::pager(renderer, h - ui::MARGIN - 44, std::to_string(page + 1) + " / " + std::to_string(pages), prev, next);
      target(prev, ID_PREV, false);
      target(next, ID_NEXT, false);
    }
  }
  renderer.displayBuffer(firstRender ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}

void HistoryActivity::onAction(int id) {
  const int count = static_cast<int>(sessions.size());
  const int pages = std::max(1, (count + perPage - 1) / perPage);
  if (id == ID_BACK) {
    onBack();
  } else if (id == ID_PREV_MONTH || id == ID_NEXT_MONTH) {
    shiftMonth(id == ID_NEXT_MONTH ? 1 : -1);
  } else if (id == ID_PREV || id == ID_NEXT) {
    page = (page + (id == ID_NEXT ? 1 : pages - 1)) % pages;
    setFocus(ID_ROW + page * perPage);
    requestUpdate();
  } else if (id >= ID_ROW && id < ID_ROW + count) {
    setFocus(id);
    auto act = makeUniqueNoThrow<HistorySessionActivity>(renderer, mappedInput, sessions[id - ID_ROW].offset);
    if (act) {
      startActivityForResult(std::move(act), [this](const ActivityResult&) {
        firstRender = true;
        requestUpdate();
      });
    }
  }
}

// ---- one session ----

void HistorySessionActivity::onEnter() {
  Activity::onEnter();
  nowRef = wallNowMs();
  offset = Service::get().utcOffsetMs(nowRef);
  ok = loadHistorySession(Service::get().fs(), lineOffset, session);
  requestUpdate();
}

bool HistorySessionActivity::onSideButton(MappedInputManager::Button b) {
  using B = MappedInputManager::Button;
  const int pages = std::max(1, (static_cast<int>(session.cards.size()) + perPage - 1) / perPage);
  if (b == B::Left || b == B::Up) {
    page = (page + pages - 1) % pages;
  } else {
    page = (page + 1) % pages;
  }
  requestUpdate();
  return true;
}

void HistorySessionActivity::onAction(int id) {
  if (id == ID_BACK) {
    onBack();
  } else if (id == ID_PREV || id == ID_NEXT) {
    onSideButton(id == ID_NEXT ? MappedInputManager::Button::Right : MappedInputManager::Button::Left);
  }
}

void HistorySessionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  clearTargets();
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const std::string title = ok ? sessionDayLabel(session.startMs, nowRef, offset) : "Sess\xC3\xA3o";
  target(ui::header(renderer, title), ID_BACK, false);
  if (!ok) {
    ui::message(renderer, h / 2 - 40, "N\xC3\xA3o foi poss\xC3\xADvel ler esta sess\xC3\xA3o.");
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }
  int y = ui::headerH() + 16;
  const int n = static_cast<int>(session.cards.size());
  const std::string sub = session.deck + " \xC2\xB7 " + std::to_string(n) + (n == 1 ? " cart\xC3\xA3o" : " cart\xC3\xB5" "es") +
                          " \xC2\xB7 " + formatDuration(session.durationMs);
  y = ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y, w - 2 * ui::MARGIN, sub, 2) + 12;

  const int rowH = 58, gap = 8, pagerH = 44 + 20;
  perPage = std::max(1, (h - ui::MARGIN - y) / (rowH + gap));
  if (n > perPage) perPage = std::max(1, (h - ui::MARGIN - pagerH - y) / (rowH + gap));
  const int pages = std::max(1, (n + perPage - 1) / perPage);
  page = std::clamp(page, 0, pages - 1);
  for (int i = page * perPage; i < std::min(n, (page + 1) * perPage); i++) {
    const HistoryCard& c = session.cards[i];
    const ui::Box b{ui::MARGIN, y, w - 2 * ui::MARGIN, rowH};
    renderer.drawRoundedRect(b.x, b.y, b.w, b.h, 2, 10, true);
    const char* chip = GRADE_CHIPS[c.grade >= 1 && c.grade <= 4 ? c.grade : 0];
    const int chipW = ui::chipWidth(renderer, chip);
    const int chipH = ui::chipHeight(renderer);
    ui::chip(renderer, b.x + b.w - 14 - chipW, b.y + (b.h - chipH) / 2, chip);
    const int textMax = b.w - 16 - chipW - 28;
    const int ty = b.y + (b.h - renderer.getLineHeight(ui::fontBody())) / 2;
    const std::string lemma = renderer.truncatedText(ui::fontBody(), c.lemma.c_str(), textMax, EpdFontFamily::BOLD);
    renderer.drawText(ui::fontBody(), b.x + 16, ty, lemma.c_str(), true, EpdFontFamily::BOLD);
    const int lw = renderer.getTextWidth(ui::fontBody(), lemma.c_str(), EpdFontFamily::BOLD);
    if (!c.sense.empty() && textMax - lw > 40) {
      const std::string sense =
          renderer.truncatedText(ui::fontBody(), (" \xC2\xB7 " + c.sense).c_str(), textMax - lw);
      renderer.drawText(ui::fontBody(), b.x + 16 + lw, ty, sense.c_str(), true);
    }
    y += rowH + gap;
  }
  if (pages > 1) {
    ui::Box prev, next;
    ui::pager(renderer, h - ui::MARGIN - 44, std::to_string(page + 1) + " / " + std::to_string(pages), prev, next);
    target(prev, ID_PREV, false);
    target(next, ID_NEXT, false);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

}  // namespace linguetta
