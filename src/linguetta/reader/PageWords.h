#pragma once
// The visible words of a laid-out page in reading order, shared by the blend footer (sentence
// around a tapped link) and Word Lens (matching and gloss placement). RTL blocks are skipped.

#include <Epub/Page.h>

#include <cstdint>
#include <cstring>

namespace linguetta {

struct PageWordRef {
  const PageLine* line = nullptr;
  const TextBlock* block = nullptr;
  uint16_t index = 0;          // word index in the block
  const char* text = nullptr;  // without the paragraph indent (leading em spaces)
  bool paragraphStart = false;  // first word of a paragraph (never true for the page's first word)
  bool spaceBefore = true;      // false: glued to the previous word (style run split, line hyphen)
  bool hyphenated = false;      // line-end discretionary hyphen: text ends in '-' and continues
};

namespace pagewords {

// Paragraph indents are rendered as a leading em space (U+2003).
inline const char* skipEmSpaces(const char* t) {
  while (static_cast<uint8_t>(t[0]) == 0xE2 && static_cast<uint8_t>(t[1]) == 0x80 &&
         static_cast<uint8_t>(t[2]) == 0x83) {
    t += 3;
  }
  return t;
}

inline bool visible(const char* t) {
  for (const auto* p = reinterpret_cast<const uint8_t*>(t); *p; ++p) {
    if (*p > ' ') return true;
  }
  return false;
}

}  // namespace pagewords

// CrossInk's TextBlock keeps no source offsets, so paragraph starts and glued words are derived from
// the line layout: a word is glued when the block says no space precedes it (or the previous line
// ended in a hyphenation break); a paragraph starts at a leading em space indent, or when the
// previous line closed a sentence (a sentence end at a line end only splits Word Lens phrases
// there, which is harmless).
namespace pagewords {
inline bool endsSentence(const char* t) {
  size_t n = std::strlen(t);
  while (n > 0) {
    const auto c = static_cast<uint8_t>(t[n - 1]);
    if (c == '"' || c == '\'' || c == ')' || c == ']') {
      n--;
    } else if (n >= 3 && c >= 0x80) {
      // UTF-8 closing quotes / ellipsis: » ” ’ …
      const auto b0 = static_cast<uint8_t>(t[n - 3]);
      const auto b1 = static_cast<uint8_t>(t[n - 2]);
      if (b0 == 0xE2 && b1 == 0x80 && (c == 0x9D || c == 0x99 || c == 0xA6)) {
        if (c == 0xA6) return true;
        n -= 3;
      } else {
        break;
      }
    } else if (n >= 2 && static_cast<uint8_t>(t[n - 2]) == 0xC2 && c == 0xBB) {
      n -= 2;
    } else {
      break;
    }
  }
  if (n == 0) return false;
  const char c = t[n - 1];
  return c == '.' || c == '!' || c == '?' || c == ':';
}
}  // namespace pagewords

template <class F>
void forEachPageWord(const Page& page, F&& f) {
  bool any = false;
  bool prevLineEndsSentence = false;
  bool prevLineHyphenated = false;
  bool haveLine = false;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const TextBlock* block = line.getBlock().get();
    if (!block || !block->valid() || block->getBlockStyle().isRtl) continue;
    bool firstOfLine = true;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* raw = block->wordText(i);
      const char* text = pagewords::skipEmSpaces(raw);
      if (!pagewords::visible(text)) continue;
      PageWordRef w;
      w.line = &line;
      w.block = block;
      w.index = i;
      w.text = text;
      w.hyphenated = block->wordEndsWithInsertedHyphen(i);
      if (firstOfLine) {
        w.paragraphStart = any && (text != raw || (haveLine && prevLineEndsSentence));
        w.spaceBefore = !(haveLine && prevLineHyphenated);
      } else {
        w.paragraphStart = text != raw;
        w.spaceBefore = block->wordHasSpaceBefore(i);
      }
      firstOfLine = false;
      any = true;
      f(w);
      prevLineEndsSentence = pagewords::endsSentence(text);
      prevLineHyphenated = w.hyphenated;
    }
    if (!firstOfLine) haveLine = true;
  }
}

}  // namespace linguetta
