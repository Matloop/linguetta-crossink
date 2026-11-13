#pragma once
// Review session over one deck: front (lemma + highlighted sentence, "Mostrar resposta"),
// back (lemma, IPA · pos, sense, sentence, obra · capítulo, the 4 grades with their
// intervals), then the session summary. A ficha shows its front text, then front and back. A
// card that turns teimosa (leech) gets a notice and leaves the session; the summary lists them
// and, when today's new cards ran out, offers "+5 hoje" (an extraNew op). Linguetta answers become `review` ops; local deck
// answers go to the deck log. The session is written to the local history at the end.

#include <memory>
#include <string>
#include <vector>

#include "linguetta/core/Decks.h"
#include "linguetta/core/Session.h"
#include "linguetta/ui/Screen.h"

namespace linguetta {

struct DeckRef {
  bool linguetta = false;
  std::string name;      // TSV deck name (local decks)
  std::string language;  // study language (synced decks)
  int deck = -1;         // synced decks: DECK_ALL ("Estudar tudo") or the deck index of the bank header
};

class ReviewActivity final : public ui::Screen {
 public:
  ReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, DeckRef ref)
      : ui::Screen("LinguettaReview", renderer, mappedInput), ref(std::move(ref)) {}

 private:
  void onEnter() override;
  void render(RenderLock&&) override;
  void onAction(int id) override;
  void onBack() override;

  void loadCard();
  void answer(int rating);
  void endSession();
  void renderFront();
  void renderBack();
  // Word centred between top and bottom (bottom < 0: from top). Returns the y below it.
  int drawWord(const std::string& word, int top, int bottom);
  // Ficha text (front or back) centred in the title font; returns the y below it.
  int drawFicha(const std::string& text, int top, int bottom, int maxLines);
  void renderSummary();
  void renderEmpty();
  void renderLeech();
  void startQueue(int64_t now);
  void addExtraNew();
  bool canAddExtra() const;

  enum class Mode { Empty, Front, Back, Leech, Summary };
  struct Face {
    std::string lemma, ipa, pos, sense, sentence, form, work, chapter;
    bool ficha = false, cloze = false;
    int64_t createdMs = 0;
  };

  DeckRef ref;
  std::string title;
  std::string shortTitle;  // card header: "Italiano" for a synced deck (the full name does not fit)
  TsvDeck tsv;
  std::unique_ptr<CardSource> source;
  ReviewSession session;
  Mode mode = Mode::Empty;
  Face face;
  struct Leech {
    std::string word;
    int lapses = 0;
  };
  std::vector<Leech> leeches;  // teimosas of this session, in order
  double intervals[4] = {0, 0, 0, 0};
  std::string status, emptyText;
  bool historySaved = false;
  bool fullRefresh = true;
};

}  // namespace linguetta
