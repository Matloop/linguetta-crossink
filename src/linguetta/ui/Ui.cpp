#include "Ui.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

namespace linguetta::ui {

int screenW(const GfxRenderer& r) { return r.getScreenWidth(); }
int screenH(const GfxRenderer& r) { return r.getScreenHeight(); }

void shade(GfxRenderer& r, const int x, const int y, const int w, const int h) {
  // 25% grey dots under existing ink: only sets pixels (CrossInk's fillRectDither also clears them).
  for (int py = y + (y & 1); py < y + h; py += 2) {
    for (int px = x + (x & 1); px < x + w; px += 2) r.drawPixel(px, py, true);
  }
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

}  // namespace linguetta::ui
