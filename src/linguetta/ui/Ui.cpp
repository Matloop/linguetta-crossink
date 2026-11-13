#include "Ui.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <vector>

#include "components/UITheme.h"
#include "fontIds.h"

namespace linguetta::ui {

// CrossInk ships Inter at 8/10/12 pt only (the CrossPoint build had Noto Sans 12-18 pt).
int fontTitle() { return UI_12_FONT_ID; }
int fontLarge() { return UI_12_FONT_ID; }
int fontBody() { return UI_10_FONT_ID; }
int fontSmall() { return SMALL_FONT_ID; }

void shade(GfxRenderer& r, const int x, const int y, const int w, const int h) {
  // 25% grey dots under existing ink: only sets pixels (CrossInk's fillRectDither also clears them).
  for (int py = y + (y & 1); py < y + h; py += 2) {
    for (int px = x + (x & 1); px < x + w; px += 2) r.drawPixel(px, py, true);
  }
}

int screenW(const GfxRenderer& r) { return r.getScreenWidth(); }
int screenH(const GfxRenderer& r) { return r.getScreenHeight(); }

void backArrow(GfxRenderer& r, int x, int y, int size, bool black) {
  const int mid = y + size / 2;
  r.drawLine(x + 2, mid, x + size, mid, 3, black);
  r.drawLine(x + 2, mid, x + size / 2, y + 2, 3, black);
  r.drawLine(x + 2, mid, x + size / 2, y + size - 2, 3, black);
}

void vertArrow(GfxRenderer& r, int x, int y, int size, bool up, bool black) {
  const int mid = x + size / 2;
  const int tip = up ? y + 2 : y + size - 2;
  const int tail = up ? y + size : y;
  const int wing = y + size / 2;
  r.drawLine(mid, tip, mid, tail, 3, black);
  r.drawLine(mid, tip, x + 2, wing, 3, black);
  r.drawLine(mid, tip, x + size - 2, wing, 3, black);
}

void progressBar(GfxRenderer& r, Box b, int done, int total) {
  r.drawRoundedRect(b.x, b.y, b.w, b.h, 2, b.h / 2, true);
  if (total <= 0 || done <= 0) return;
  const int inner = b.w - 8;
  const int fill = static_cast<int>(static_cast<int64_t>(inner) * std::min(done, total) / total);
  if (fill > 0) r.fillRect(b.x + 4, b.y + 4, fill, b.h - 8, true);
}

// The CrossPoint header (battery, clock, touch back button folded into Back), so these
// screens match the rest of the firmware. Its back button is not a screen tap target.
Box header(GfxRenderer& r, const std::string& title, const std::string& right, bool back) {
  const auto& m = UITheme::getInstance().getMetrics();
  GUI.drawHeader(r, Rect{0, m.topPadding, r.getScreenWidth(), m.headerHeight}, title.c_str(),
                 right.empty() ? nullptr : right.c_str(), back);
  return Box{};
}

int headerH() {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight;
}

int section(GfxRenderer& r, int y, const std::string& label) {
  y += 14;
  r.drawText(fontSmall(), MARGIN, y, label.c_str(), true);
  y += r.getLineHeight(fontSmall()) + 5;
  r.fillRect(MARGIN, y, screenW(r) - 2 * MARGIN, 2, true);
  return y + 2 + 10;
}

Box row(GfxRenderer& r, int y, const std::string& label, const std::string& right, bool strong, bool chevron) {
  const Box b{MARGIN, y, screenW(r) - 2 * MARGIN, ROW_H};
  r.drawRoundedRect(b.x, b.y, b.w, b.h, strong ? 3 : 2, 10, true);
  const auto style = strong ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  int rightEdge = b.x + b.w - 16;
  if (chevron) {
    const int cx = rightEdge - 8, cy = b.y + b.h / 2;
    r.drawLine(cx - 4, cy - 8, cx + 3, cy, 2, true);
    r.drawLine(cx + 3, cy, cx - 4, cy + 8, 2, true);
    rightEdge -= 24;
  }
  if (!right.empty()) {
    const int rw = r.getTextWidth(fontBody(), right.c_str());
    r.drawText(fontBody(), rightEdge - rw, b.y + (b.h - r.getLineHeight(fontBody())) / 2, right.c_str(), true);
    rightEdge -= rw + 14;
  }
  const std::string t = r.truncatedText(fontBody(), label.c_str(), rightEdge - (b.x + 16), style);
  r.drawText(fontBody(), b.x + 16, b.y + (b.h - r.getLineHeight(fontBody())) / 2, t.c_str(), true, style);
  return b;
}

int sectionH(const GfxRenderer& r) { return 14 + r.getLineHeight(fontSmall()) + 5 + 2 + 10; }

Box row2(GfxRenderer& r, int y, const std::string& title, const std::string& subtitle, bool chevron, int indent,
         bool strong) {
  const Box b{MARGIN + indent, y, screenW(r) - 2 * MARGIN - indent, ROW2_H};
  r.drawRoundedRect(b.x, b.y, b.w, b.h, strong ? 3 : 2, 10, true);
  int rightEdge = b.x + b.w - 16;
  if (chevron) {
    const int cx = rightEdge - 8, cy = b.y + b.h / 2;
    r.drawLine(cx - 4, cy - 8, cx + 3, cy, 2, true);
    r.drawLine(cx + 3, cy, cx - 4, cy + 8, 2, true);
    rightEdge -= 24;
  }
  const int maxW = rightEdge - (b.x + 16);
  const int th = r.getLineHeight(fontBody()), sh = r.getLineHeight(fontSmall());
  const int top = b.y + (b.h - th - sh - 2) / 2;
  const std::string t = r.truncatedText(fontBody(), title.c_str(), maxW, EpdFontFamily::BOLD);
  r.drawText(fontBody(), b.x + 16, top, t.c_str(), true, EpdFontFamily::BOLD);
  const std::string s = r.truncatedText(fontSmall(), subtitle.c_str(), maxW);
  r.drawText(fontSmall(), b.x + 16, top + th + 2, s.c_str(), true);
  return b;
}

int highlightedSentence(GfxRenderer& r, int fontId, int y, int maxW, const std::string& text, size_t hlStart,
                        size_t hlEnd, int maxLines, EpdFontFamily::Style style) {
  struct Word {
    size_t start, end;
  };
  std::vector<Word> words;
  for (size_t i = 0; i < text.size();) {
    while (i < text.size() && text[i] == ' ') i++;
    const size_t s = i;
    while (i < text.size() && text[i] != ' ') i++;
    if (i > s) words.push_back({s, i});
  }
  const int space = r.getSpaceWidth(fontId, style);
  const int lh = r.getLineHeight(fontId);
  const int w = screenW(r);
  size_t k = 0;
  for (int line = 0; line < maxLines && k < words.size(); line++) {
    // Greedy fill; an over-long word gets a line of its own (truncated).
    size_t end = k;
    int width = 0;
    while (end < words.size()) {
      const std::string word = text.substr(words[end].start, words[end].end - words[end].start);
      const int ww = r.getTextWidth(fontId, word.c_str(), style);
      const int next = width + (end > k ? space : 0) + ww;
      if (next > maxW && end > k) break;
      width = next;
      end++;
    }
    const bool last = line == maxLines - 1 && end < words.size();
    int x = (w - std::min(width, maxW)) / 2;
    for (size_t i = k; i < end; i++) {
      std::string word = text.substr(words[i].start, words[i].end - words[i].start);
      if (last && i == end - 1) word += "\xE2\x80\xA6";
      word = r.truncatedText(fontId, word.c_str(), maxW, style);
      const int ww = r.getTextWidth(fontId, word.c_str(), style);
      if (words[i].end > hlStart && words[i].start < hlEnd) {
        shade(r, x - 3, y, ww + 6, lh);
      }
      r.drawText(fontId, x, y, word.c_str(), true, style);
      x += ww + space;
    }
    y += lh;
    k = end;
  }
  return y;
}

int pager(GfxRenderer& r, int y, const std::string& label, Box& prev, Box& next) {
  const int w = screenW(r);
  const int lw = r.getTextWidth(fontBody(), label.c_str(), EpdFontFamily::BOLD);
  const int sq = 44;
  const int total = sq + 24 + lw + 24 + sq;
  int x = (w - total) / 2;
  prev = Box{x, y, sq, sq};
  next = Box{x + total - sq, y, sq, sq};
  for (const Box* b : {&prev, &next}) {
    r.drawRoundedRect(b->x, b->y, b->w, b->h, 2, 8, true);
    const int cx = b->x + b->w / 2, cy = b->y + b->h / 2;
    const int d = b == &prev ? -1 : 1;
    r.drawLine(cx - 3 * d, cy - 8, cx + 4 * d, cy, 2, true);
    r.drawLine(cx + 4 * d, cy, cx - 3 * d, cy + 8, 2, true);
  }
  r.drawText(fontBody(), x + sq + 24, y + (sq - r.getLineHeight(fontBody())) / 2, label.c_str(), true,
             EpdFontFamily::BOLD);
  // Taller tap boxes than the drawn squares.
  prev = Box{prev.x - 20, y - 10, sq + 40, sq + 20};
  next = Box{next.x - 20, y - 10, sq + 40, sq + 20};
  return sq;
}

Box button(GfxRenderer& r, Box b, const std::string& label, bool primary) {
  if (primary) {
    r.fillRoundedRect(b.x, b.y, b.w, b.h, 10, Color::Black);
  } else {
    r.fillRoundedRect(b.x, b.y, b.w, b.h, 10, Color::White);
    r.drawRoundedRect(b.x, b.y, b.w, b.h, 3, 10, true);
  }
  const int font = b.h >= 56 ? fontBody() : fontSmall();
  const std::string t = r.truncatedText(font, label.c_str(), b.w - 12, EpdFontFamily::BOLD);
  const int tw = r.getTextWidth(font, t.c_str(), EpdFontFamily::BOLD);
  r.drawText(font, b.x + (b.w - tw) / 2, b.y + (b.h - r.getLineHeight(font)) / 2, t.c_str(), !primary,
             EpdFontFamily::BOLD);
  return b;
}

int chip(GfxRenderer& r, int x, int y, const std::string& label) {
  const int tw = r.getTextWidth(SMALL_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
  const int h = r.getLineHeight(SMALL_FONT_ID) + 6;
  r.drawRoundedRect(x, y, tw + 16, h, 2, 6, true);
  r.drawText(SMALL_FONT_ID, x + 8, y + 3, label.c_str(), true, EpdFontFamily::BOLD);
  return tw + 16;
}

int chipWidth(const GfxRenderer& r, const std::string& label) {
  return r.getTextWidth(SMALL_FONT_ID, label.c_str(), EpdFontFamily::BOLD) + 16;
}
int chipHeight(const GfxRenderer& r) { return r.getLineHeight(SMALL_FONT_ID) + 6; }

int paragraph(GfxRenderer& r, int fontId, int x, int y, int maxW, const std::string& text, int maxLines, bool black,
              EpdFontFamily::Style style) {
  if (text.empty()) return y;
  const int lh = r.getLineHeight(fontId);
  for (const auto& line : r.wrappedText(fontId, text.c_str(), maxW, maxLines, style)) {
    r.drawText(fontId, x, y, line.c_str(), black, style);
    y += lh;
  }
  return y;
}

void statBox(GfxRenderer& r, Box b, const std::string& value, const std::string& label) {
  r.drawRoundedRect(b.x, b.y, b.w, b.h, 2, 10, true);
  const int vh = r.getLineHeight(fontTitle()), lh = r.getLineHeight(fontSmall());
  const int top = b.y + (b.h - vh - lh) / 2;
  r.drawText(fontTitle(), b.x + 16, top, value.c_str(), true, EpdFontFamily::BOLD);
  const std::string l = r.truncatedText(fontSmall(), label.c_str(), b.w - 32);
  r.drawText(fontSmall(), b.x + 16, top + vh, l.c_str(), true);
}

void message(GfxRenderer& r, int y, const std::string& text) {
  const int w = screenW(r) - 2 * MARGIN;
  const auto lines = r.wrappedText(fontBody(), text.c_str(), w, 6);
  const int lh = r.getLineHeight(fontBody());
  for (const auto& line : lines) {
    const int tw = r.getTextWidth(fontBody(), line.c_str());
    r.drawText(fontBody(), MARGIN + (w - tw) / 2, y, line.c_str(), true);
    y += lh;
  }
}

}  // namespace linguetta::ui
