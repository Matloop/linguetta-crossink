#pragma once
// Footer strip over the reader page after a tap on a blend link (#lb:LEMMA|TRANSLATION|NOTE).
// New word: lemma, NOTE, gloss, Salvar / Fechar; Salvar appends a `save` op.
// Word already in the bank: "NO BANCO" chip, next review, Fechar only; appends a `seen` op.
// Word Lens tap (tap.lens): the bank item, next review, Salvar encontro / Fechar; no `seen`
// (the page already recorded it) and no language switch.

#include <Epub/Page.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "linguetta/core/Blend.h"
#include "linguetta/ui/Ui.h"

namespace linguetta {

struct BlendTap {
  BlendLink link;
  LinkContext context;
  std::vector<ui::Box> linkBoxes;  // screen coordinates of the tapped link (one per line)
  // where
  std::string chapter;
  int spine = -1;
  double percent = -1;
  // work
  std::string bookPath, title, author, bookLanguage;
  // Word Lens: link.lemma is the bank lemma, link.translation the gloss, language fixed.
  bool lens = false;
  std::string language;
};

class BlendFooterActivity final : public Activity {
 public:
  BlendFooterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                      int marginLeft, int marginTop, BlendTap tap)
      : Activity("LinguettaBlendFooter", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        tap(std::move(tap)) {}

 private:
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void save();
  void close();
  void lookUp();          // fills mode/bank* for `language`
  void cycleLanguage();   // next study language; remembered for this book
  bool canSave() const { return mode == Mode::New || (tap.lens && mode == Mode::InBank); }

  enum class Mode { New, InBank, Saved };
  std::unique_ptr<Page> page;
  const int marginLeft, marginTop;
  BlendTap tap;
  Mode mode = Mode::New;
  std::string language;  // study language of this word's ops (Service::blendLanguage)
  std::string koHash;    // the book's KOSync hash (Obra key, books.json key)
  std::vector<std::string> languages;  // study languages the word could go to
  std::string bankLemma, bankSense, bankPos, nextReview, status;
  ui::Box strip, saveBox, closeBox, langBox;
};

}  // namespace linguetta
