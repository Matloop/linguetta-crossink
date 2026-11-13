#pragma once
// Local study history (never synced): /.linguetta/history.jsonl, one finished session per line
// {"deck":"...","start":ms,"ms":durationMs,"cards":[{"l":lemma,"s":sense,"g":grade},...]}.

#include <cstdint>
#include <string>
#include <vector>

#include "Fs.h"

namespace linguetta {

struct HistoryCard {
  std::string lemma, sense;
  int grade = 0;
};

struct HistorySession {
  std::string deck;
  int64_t startMs = 0, durationMs = 0;
  int cardCount = 0;              // answers in the session (cards.size() when loaded)
  int64_t offset = -1;            // line offset in history.jsonl (set by loadHistory)
  std::vector<HistoryCard> cards;  // only filled when asked for
};

constexpr size_t HISTORY_MAX_CARDS = 300;  // answers kept per session line

bool appendHistory(Fs& fs, const HistorySession& s);

// Sessions that started in [fromMs, toMs), newest first, without their cards.
std::vector<HistorySession> loadHistory(Fs& fs, int64_t fromMs, int64_t toMs);
// One session with its cards, by the offset loadHistory returned.
bool loadHistorySession(Fs& fs, int64_t offset, HistorySession& out);

// "Hoje · 11:34", "Ontem · 15:01", "09/11 · 08:30" (wall clock at utcOffsetMs).
std::string sessionDayLabel(int64_t startMs, int64_t nowMs, int64_t utcOffsetMs);
// "NOVEMBRO 2026"
std::string monthLabel(int year, int month);

// Month boundaries in UTC ms for a wall-clock month at a fixed UTC offset.
int64_t monthStartMs(int year, int month, int64_t utcOffsetMs);

}  // namespace linguetta
