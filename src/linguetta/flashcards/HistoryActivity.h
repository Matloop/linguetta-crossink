#pragma once
// Local study history: month navigation, totals, a paged list of sessions (all decks) and a
// session detail with each card's grade. Read from /.linguetta/history.jsonl.

#include <string>
#include <vector>

#include "linguetta/core/History.h"
#include "linguetta/ui/Screen.h"

namespace linguetta {

class HistoryActivity final : public ui::Screen {
 public:
  HistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : ui::Screen("LinguettaHistory", renderer, mappedInput) {}

 private:
  void onEnter() override;
  void render(RenderLock&&) override;
  void onAction(int id) override;
  bool onSideButton(MappedInputManager::Button b) override;
  void load();
  void shiftMonth(int delta);

  int year = 2026, month = 1;
  int64_t nowRef = 0, offset = 0;
  std::vector<HistorySession> sessions;
  int cardTotal = 0;
  int page = 0, perPage = 1;
  bool firstRender = true;
};

class HistorySessionActivity final : public ui::Screen {
 public:
  HistorySessionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int64_t lineOffset)
      : ui::Screen("LinguettaHistorySession", renderer, mappedInput), lineOffset(lineOffset) {}

 private:
  void onEnter() override;
  void render(RenderLock&&) override;
  void onAction(int id) override;
  bool onSideButton(MappedInputManager::Button b) override;

  int64_t lineOffset;
  HistorySession session;
  bool ok = false;
  int64_t nowRef = 0, offset = 0;
  int page = 0, perPage = 1;
};

}  // namespace linguetta
