#pragma once
// The two kinds of deck behind a ReviewSession: a synced deck (local view, items and fichas;
// answers become `review` ops in the outbox) and a local TSV deck (answers go to its log).

#include <functional>
#include <string>

#include "Decks.h"
#include "LocalView.h"
#include "Session.h"

namespace linguetta {

class ViewSource final : public CardSource {
 public:
  // `record` appends an op line to the outbox and applies it to `view`.
  using Recorder = std::function<bool(const std::string& line)>;
  using IdMaker = std::function<std::string()>;
  ViewSource(LocalView& view, Fs& fs, Recorder record, IdMaker newId)
      : view(view), fs(fs), record(std::move(record)), newId(std::move(newId)) {}

  const CardState& state(int i) const override { return view.entry(i).card; }
  std::string seed(int i) const override { return view.entry(i).seed(); }
  void label(int i, std::string& lemma, std::string& sense) const override;
  bool grade(int i, int rating, int64_t nowMs, int64_t durationMs) override;
  bool leech(int i) const override { return view.entry(i).leechWaiting; }

 private:
  LocalView& view;
  Fs& fs;
  Recorder record;
  IdMaker newId;
};

class TsvSource final : public CardSource {
 public:
  TsvSource(TsvDeck& deck, Fs& fs) : deck(deck), fs(fs) {}
  const CardState& state(int i) const override { return deck.card(i).card; }
  std::string seed(int i) const override { return deck.seed(i); }
  void label(int i, std::string& lemma, std::string& sense) const override {
    lemma = deck.card(i).front;
    sense = deck.card(i).back;
  }
  bool grade(int i, int rating, int64_t nowMs, int64_t) override { return deck.grade(fs, i, rating, nowMs); }

 private:
  TsvDeck& deck;
  Fs& fs;
};

}  // namespace linguetta
