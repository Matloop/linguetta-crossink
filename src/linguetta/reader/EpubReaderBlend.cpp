// EpubReaderActivity members added by Linguetta (declared in EpubReaderActivity.h, see
// LINGUETTA.md). Kept here so the upstream reader .cpp only gains the call sites.
//
// Blend links ("#lb:LEMMA|TRANSLATION|...") arrive from the EPUB parser as footnote entries.
// They are moved out of the page's footnotes (so they never open the footnote UI) into
// linguettaBlendLinks, and are hit-tested here from the laid-out words that carry their link id.

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <limits>

#include "BlendFooterActivity.h"
#include "CrossPointSettings.h"
#include "PageWords.h"
#include "WordLensReader.h"
#include "activities/RenderLock.h"
#include "activities/reader/EpubReaderActivity.h"
#include "components/UITheme.h"
#include "linguetta/core/Blend.h"

namespace {
constexpr int kBlendTouchSlop = 12;  // px around a link that still counts as a tap on it

bool within(const linguetta::ui::Box& b, const int x, const int y, const int slop) {
  return x >= b.x - slop && x < b.x + b.w + slop && y >= b.y - slop && y < b.y + b.h + slop;
}
}  // namespace

void EpubReaderActivity::linguettaSplitBlend(std::vector<FootnoteEntry>& footnotes) {
  linguettaBlendLinks.clear();
  for (const auto& f : footnotes) {
    if (linguetta::isBlendHref(f.href)) linguettaBlendLinks.push_back(f);
  }
  if (linguettaBlendLinks.empty()) return;
  footnotes.erase(std::remove_if(footnotes.begin(), footnotes.end(),
                                 [](const FootnoteEntry& f) { return linguetta::isBlendHref(f.href); }),
                  footnotes.end());
}

bool EpubReaderActivity::linguettaTouch(const int x, const int y) {
  if (!SETTINGS.touchReaderControls || !mappedInput.hasTouch() || RenderLock::peek() || activeFootnotePreview ||
      !section || !epub) {
    return false;
  }

  // A tap on a glossed word (Word Lens).
  linguetta::wordlens::Hit hit;
  const bool lensHit = linguetta::wordlens::hitAt(x, y, hit);
  if (!lensHit && linguettaBlendLinks.empty()) return false;

  std::unique_ptr<Page> page;
  {
    RenderLock lock(*this);
    page = section->loadPageFromSectionFile();
  }
  if (!page) return false;
  const int ml = linguettaMarginLeft;
  const int mt = linguettaMarginTop;
  const int fontId = linguettaFontId;

  // Blend links: screen boxes of each link (one per line it spans) from the words carrying its id.
  const FootnoteEntry* tappedLink = nullptr;
  std::vector<linguetta::ui::Box> boxes;
  if (!lensHit) {
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (const auto& entry : linguettaBlendLinks) {
      std::vector<linguetta::ui::Box> entryBoxes;
      const PageLine* lastLine = nullptr;
      linguetta::forEachPageWord(*page, [&](const linguetta::PageWordRef& r) {
        if (r.block->wordLinkId(r.index) != entry.linkId) return;
        const int lineFont = r.block->resolvedFontId(renderer, fontId);
        const int lineHeight = r.block->getBlockStyle().lineHeight ? r.block->getBlockStyle().lineHeight
                                                                    : renderer.getLineHeight(lineFont);
        const auto style = static_cast<EpdFontFamily::Style>(r.block->wordStyle(r.index) & ~EpdFontFamily::UNDERLINE);
        const int wx = ml + r.line->xPos + r.block->wordXpos(r.index);
        const int wy = mt + r.line->yPos;
        const int ww = std::max(1, renderer.getTextAdvanceX(lineFont, r.block->wordText(r.index), style));
        if (lastLine == r.line && !entryBoxes.empty()) {
          auto& b = entryBoxes.back();
          const int right = std::max(b.x + b.w, wx + ww);
          b.x = std::min(b.x, wx);
          b.w = right - b.x;
        } else {
          entryBoxes.push_back(linguetta::ui::Box{wx, wy, ww, lineHeight});
          lastLine = r.line;
        }
      });
      for (const auto& b : entryBoxes) {
        if (!within(b, x, y, kBlendTouchSlop)) continue;
        const int64_t dx = static_cast<int64_t>(x) - (b.x + b.w / 2);
        const int64_t dy = static_cast<int64_t>(y) - (b.y + b.h / 2);
        const int64_t d = dx * dx + dy * dy;
        if (d < bestDistance) {
          bestDistance = d;
          tappedLink = &entry;
          boxes = entryBoxes;
        }
      }
    }
    if (!tappedLink) return false;
  }

  linguetta::BlendTap tap;
  std::vector<linguetta::PageWord> words;
  if (lensHit) {
    tap.lens = true;
    tap.language = linguetta::wordlens::language();
    tap.link.lemma = hit.lemma;
    tap.link.translation = hit.text;
    tap.linkBoxes.push_back(hit.wordBox);
    size_t n = 0;
    linguetta::forEachPageWord(*page, [&](const linguetta::PageWordRef& r) {
      linguetta::PageWord w;
      w.text = r.text;
      w.paragraphStart = r.paragraphStart;
      w.spaceBefore = r.spaceBefore;
      w.hyphenated = r.hyphenated;
      w.inLink = n >= hit.firstWord && n <= hit.lastWord;
      n++;
      words.push_back(std::move(w));
    });
  } else {
    if (!linguetta::parseBlendHref(tappedLink->href, tap.link)) return true;  // malformed: swallow the tap
    tap.linkBoxes = std::move(boxes);
    const uint8_t linkId = tappedLink->linkId;
    linguetta::forEachPageWord(*page, [&](const linguetta::PageWordRef& r) {
      linguetta::PageWord w;
      w.text = r.text;
      w.paragraphStart = r.paragraphStart;
      w.spaceBefore = r.spaceBefore;
      w.hyphenated = r.hyphenated;
      w.inLink = r.block->wordLinkId(r.index) == linkId;
      words.push_back(std::move(w));
    });
  }
  tap.context = linguetta::extractLinkContext(words);
  words.clear();
  words.shrink_to_fit();

  linguettaOpenFooter(std::move(page), std::move(tap), ml, mt);
  return true;
}

void EpubReaderActivity::linguettaOpenFooter(std::unique_ptr<Page> page, linguetta::BlendTap&& tap, const int ml,
                                             const int mt) {
  tap.spine = currentSpineIndex;
  const int toc = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (toc >= 0) tap.chapter = epub->getTocItem(toc).title;
  tap.percent = std::clamp(getCurrentBookProgressPercent(), 0.0f, 100.0f);
  tap.bookPath = epub->getPath();
  tap.title = epub->getTitle();
  tap.author = epub->getAuthor();
  tap.bookLanguage = epub->getLanguage();

  auto footer = makeUniqueNoThrow<linguetta::BlendFooterActivity>(renderer, mappedInput, std::move(page), ml, mt,
                                                                  std::move(tap));
  if (!footer) {
    LOG_ERR("LGT", "OOM: BlendFooterActivity");
    return;
  }
  startActivityForResult(std::move(footer), [this](const ActivityResult&) { requestUpdate(); });
}

// Word Lens turned on/off: its line spacing floor moved, so the section is laid out again.
void EpubReaderActivity::linguettaWordLensRelayout() {
  if (!linguetta::wordlens::takeRelayout()) return;
  RenderLock lock(*this);
  prepareCurrentSectionForRelayout();
  section.reset();
  requestUpdate();
}

void EpubReaderActivity::linguettaWordLensPopup() {
  using linguetta::lens::Level;
  using linguetta::lens::levelLabel;
  static const char* const levels[] = {levelLabel(Level::Off), levelLabel(Level::Muita), levelLabel(Level::Media),
                                       levelLabel(Level::Pouca)};
  quickActionsPopup.show("Word Lens", levels, 4, static_cast<int>(linguetta::wordlens::level()), [this](int idx) {
    mappedInput.setReaderTouchscreenOverride(false);
    linguetta::wordlens::setLevel(static_cast<Level>(idx));
    linguettaWordLensRelayout();
    requestUpdate();
  });
  if (quickActionsPopup.isActive()) {
    mappedInput.setReaderTouchscreenOverride(true);
    quickActionsPopup.setCancelCallback([this] { mappedInput.setReaderTouchscreenOverride(false); });
  }
  requestUpdate();
}
