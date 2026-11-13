#pragma once
// Blend links: <a href="#lb:LEMMA|TRANSLATION|NOTE">form</a> (crossnext-blend docs/blend-format.md,
// implemented from the spec). Plus the sentence around a tapped link, from the page words.

#include <string>
#include <vector>

namespace linguetta {

struct BlendLink {
  std::string lemma, translation, note;
};

inline bool isBlendHref(const char* href) {
  return href && href[0] == '#' && href[1] == 'l' && href[2] == 'b' && href[3] == ':';
}

// False if not "#lb:", LEMMA or TRANSLATION empty, more than three fields, or >= 256 bytes.
// Fields are kept whole (the contract limits apply when saving); %7C -> '|', %25 -> '%'.
bool parseBlendHref(const char* href, BlendLink& out);

// One word of the page in reading order.
struct PageWord {
  std::string text;
  bool paragraphStart = false;  // first word of a paragraph
  bool spaceBefore = true;      // separated from the previous word
  bool hyphenated = false;      // line-end hyphenation: joins the next word
  bool inLink = false;          // part of the tapped link
};

struct LinkContext {
  std::string form;      // the link text as it appears ("" if no word matched the link)
  std::string sentence;  // the sentence containing it, within the page ("" if unknown)
};

LinkContext extractLinkContext(const std::vector<PageWord>& words);

}  // namespace linguetta
