#pragma once
// Linguetta screen kit, after the approved 480x800 mock: dark header with a back arrow,
// section labels, rounded bordered rows with a chevron, black primary buttons, chips.

#include <EpdFontFamily.h>

#include <string>

class GfxRenderer;

namespace linguetta::ui {

struct Box {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(int px, int py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
};

constexpr int MARGIN = 22;
// Height of the CrossPoint header band (top padding included); content starts below it.
int headerH();
constexpr int ROW_H = 66;
constexpr int ROW_GAP = 10;
constexpr int BUTTON_H = 64;

// Fonts (all with regular/bold/italic).
int fontTitle();   // 18 pt
int fontLarge();   // 16 pt
int fontBody();    // 14 pt
int fontSmall();   // 12 pt
// 28 pt bold, flashcard word only (DisplayFont.cpp). Registered from main.cpp.
constexpr int DISPLAY_FONT_ID = 0x4C474431;  // "LGD1"
int fontDisplay();
// 12 pt regular with IPA glyphs (the UI fonts have none).
constexpr int IPA_FONT_ID = 0x4C474932;  // "LGI2"
int fontIpa();
void registerDisplayFont(GfxRenderer& r);
// Light-grey shading that keeps the ink already drawn under it (reader highlight behind a word).
void shade(GfxRenderer& r, int x, int y, int w, int h);

// CrossPoint header (title, right label, touch back button routed to Back). Returns {}.
Box header(GfxRenderer& r, const std::string& title, const std::string& right = "", bool back = true);
// Grey label with a rule under it; returns the y below it.
int section(GfxRenderer& r, int y, const std::string& label);
// Bordered row with label, optional right text and a chevron. `strong`: thick border, bold.
Box row(GfxRenderer& r, int y, const std::string& label, const std::string& right = "", bool strong = false,
        bool chevron = true);
// Two-line bordered row (bold title, small subtitle) with a chevron; ROW2_H tall.
constexpr int ROW2_H = 78;
// indent: pixels from the left margin (a subdeck); strong: thicker border (the main action).
Box row2(GfxRenderer& r, int y, const std::string& title, const std::string& subtitle, bool chevron = true,
         int indent = 0, bool strong = false);
// Height used by section() (label, rule and spacing).
int sectionH(const GfxRenderer& r);
Box button(GfxRenderer& r, Box b, const std::string& label, bool primary);
// Centered wrapped sentence; the bytes [hlStart, hlEnd) get a grey box behind them.
// Returns the y below it.
int highlightedSentence(GfxRenderer& r, int fontId, int y, int maxW, const std::string& text, size_t hlStart,
                        size_t hlEnd, int maxLines, EpdFontFamily::Style style = EpdFontFamily::ITALIC);
// "‹  label  ›" pager centred at y; fills the tap boxes of both arrows. Returns its height.
int pager(GfxRenderer& r, int y, const std::string& label, Box& prev, Box& next);
// Bordered caps chip; returns its width.
int chip(GfxRenderer& r, int x, int y, const std::string& label);
int chipWidth(const GfxRenderer& r, const std::string& label);
int chipHeight(const GfxRenderer& r);
// Left arrow drawn with lines (no arrow glyph in the fonts).
void backArrow(GfxRenderer& r, int x, int y, int size, bool black);
// Up / down arrow drawn with lines, in a size x size box.
void vertArrow(GfxRenderer& r, int x, int y, int size, bool up, bool black = true);
// Bordered progress bar; `done` of `total` filled (empty when total <= 0).
void progressBar(GfxRenderer& r, Box b, int done, int total);
// Wrapped text; returns the y below it.
int paragraph(GfxRenderer& r, int fontId, int x, int y, int maxW, const std::string& text, int maxLines,
              bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
// Bordered box with a big number over a small label (session and history totals).
void statBox(GfxRenderer& r, Box b, const std::string& value, const std::string& label);
// Centered message on an otherwise empty body.
void message(GfxRenderer& r, int y, const std::string& text);

int screenW(const GfxRenderer& r);
int screenH(const GfxRenderer& r);

}  // namespace linguetta::ui
