#pragma once
// Flashcards home: "N para hoje"; per study language with server decks a section with
// "Estudar tudo" (thick border: due + "X novas de Y") and one row per deck ("N cartões · M para
// hoje", subdecks indented); a bank from before decks keeps one row per language; then the
// local TSV/CSV decks of /flashcards not imported yet, then Sincronizar and Histórico.

#include <memory>
#include <string>
#include <vector>

#include "linguetta/ui/Screen.h"

class GfxRenderer;
class MappedInputManager;

namespace linguetta {

std::unique_ptr<Activity> makeDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

class DeckListActivity final : public ui::Screen {
 public:
  struct Deck {
    std::string name;  // TSV deck name, deck name, or the section label
    bool linguetta = false;
    bool section = false;  // a label row ("Decks · Italiano"), not tappable
    bool all = false;      // "Estudar tudo"
    std::string language;  // synced decks
    int deck = -1;         // DECK_ALL or the index in the bank header decks
    int indent = 0;        // subdeck
    int count = -1;        // cards for today; -1 when unknown (no trusted clock / unreadable)
    int cards = 0;         // flashcards in the deck (known items excluded)
    int fresh = 0, limit = 0;  // "Estudar tudo": new cards in the queue / new cards allowed today
  };

  DeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : ui::Screen("LinguettaDecks", renderer, mappedInput) {}

 private:
  // One row of the list: a deck, or "Sincronizar" / "Histórico".
  struct Row {
    enum class Kind { Deck, Sync, History } kind = Kind::Deck;
    int deck = -1;  // index in `decks`
    std::string label, subtitle, value, heading;
  };

  void onEnter() override;
  std::string title() const override { return "Flashcards"; }
  std::string subtitle() const override;
  void build(ui::Ui& ui) override;
  int listCount() const override { return static_cast<int>(rows.size()); }
  void onRow(int row) override;
  void onBack() override;
  void reload();
  void openDeck(size_t i);

  std::vector<Deck> decks;
  std::vector<Row> rows;
  int total = 0;
  bool clockOk = false;
  bool paired = false;
  int queued = 0;  // outbox operations waiting for the next sync
  std::string status;
};

}  // namespace linguetta
