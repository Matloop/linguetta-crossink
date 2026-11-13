#pragma once
// Word Lens in the EPUB reader: a short gloss from the bank drawn in the gap above the words
// of the book that are in the study deck. Rules live in linguetta/core/WordLens.h; this is the
// device side (book, level, drawing, hit boxes, `seen` ops). See LINGUETTA.md.

#include <string>

#include "linguetta/core/WordLens.h"
#include "linguetta/ui/Ui.h"

class GfxRenderer;
class Page;

namespace linguetta::wordlens {

// Reader lifecycle (EpubReaderActivity::loadBook / destructor). `dcLanguage` is the OPF
// dc:language; its primary subtag picks the study language.
void openBook(const std::string& path, const std::string& dcLanguage);
void closeBook();

// True when the open book's language is a study language and the level is not Off.
bool active();
// The study language glossed in the open book ("" if none).
const std::string& language();

// Line spacing for layout: at least lens::MIN_LINE_COMPRESSION while active.
float lineCompression(float configured);

// Reader menu: shown when the device has any study language.
bool available();
lens::Level level();
void setLevel(lens::Level level);
// True once after setLevel() changed the open book's line spacing (its section must be rebuilt).
bool takeRelayout();

// Draws the glosses of `page` (after page->render, same font and margins) and remembers their
// hit boxes. `shown`: this is the frame going to the display; records the day's `seen` ops.
void draw(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop, bool shown);

struct Hit {
  int gloss = -1;
  std::string lemma, text;           // bank lemma and the gloss shown
  size_t firstWord = 0, lastWord = 0;  // forEachPageWord indices covered by the match
  ui::Box wordBox;                     // screen box of the match on its first line
};
// The glossed word under a tap on the last drawn page.
bool hitAt(int x, int y, Hit& out);

}  // namespace linguetta::wordlens
