#pragma once
// What the Linguetta screens need beyond CrossInk's own UI components (FreeInkUI lists, buttons,
// metric cards, sheet; TouchHeaderBackButton; GUI header and button hints): a screen box, the
// flashcard word and IPA fonts, and a reader highlight. Everything else (fonts, row heights,
// colours, header, hints) comes from the theme and the "uiScale" setting, so the flashcards look
// like the rest of the firmware.

#include <EpdFontFamily.h>

#include <string>

class GfxRenderer;

namespace linguetta::ui {

struct Box {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(int px, int py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
};

// 28 pt bold, flashcard word only (DisplayFont.cpp). Registered from main.cpp.
constexpr int DISPLAY_FONT_ID = 0x4C474431;  // "LGD1"
int fontDisplay();
// 12 pt regular with IPA glyphs (the UI fonts have none).
constexpr int IPA_FONT_ID = 0x4C474932;  // "LGI2"
int fontIpa();
void registerDisplayFont(GfxRenderer& r);

// Light-grey shading that keeps the ink already drawn under it (reader highlight behind a word).
void shade(GfxRenderer& r, int x, int y, int w, int h);

// Centered wrapped sentence; the bytes [hlStart, hlEnd) get a grey box behind them. Returns the y
// below it.
int highlightedSentence(GfxRenderer& r, int fontId, int y, int maxW, const std::string& text, size_t hlStart,
                        size_t hlEnd, int maxLines, EpdFontFamily::Style style = EpdFontFamily::ITALIC);

int screenW(const GfxRenderer& r);
int screenH(const GfxRenderer& r);

}  // namespace linguetta::ui
