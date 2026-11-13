#include "WordLensReader.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <vector>

#include "PageWords.h"
#include "linguetta/core/Ops.h"
#include "linguetta/device/Linguetta.h"
#include "linguetta/ui/Ui.h"

namespace linguetta::wordlens {

namespace {

constexpr int EDGE_ROOM = 12;  // a gloss may overhang a line-edge word by this much
constexpr int GLOSS_GAP = 3;   // between the gloss baseline and the top of the word's line box

struct BoxHit {
  ui::Box box, word;
  int gloss = -1;
  size_t firstWord = 0, lastWord = 0;
};

struct Lens {
  bool stateLoaded = false;
  lens::State state;
  std::string bookPath, bookLanguage;  // bookLanguage: matching study language or ""
  lens::Index index;
  std::string indexLanguage;
  lens::Level indexLevel = lens::Level::Off;
  uint32_t indexGeneration = 0;
  bool indexBuilt = false;
  bool relayout = false;
  std::vector<BoxHit> hits;
};

Lens& L() {
  static Lens lens;
  return lens;
}

lens::State& state() {
  Lens& l = L();
  if (!l.stateLoaded) {
    l.state = lens::loadState(Service::get().fs());
    l.stateLoaded = true;
  }
  return l.state;
}

std::string primarySubtag(const std::string& tag) {
  std::string out = tag.substr(0, tag.find_first_of("-_"));
  for (char& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return out;
}

void ensureIndex() {
  Lens& l = L();
  Service& svc = Service::get();
  const lens::Level lv = state().level;
  if (l.indexBuilt && l.indexLanguage == l.bookLanguage && l.indexLevel == lv &&
      l.indexGeneration == svc.generation()) {
    return;
  }
  const unsigned long t0 = millis();
  LocalView& view = svc.view(l.bookLanguage);  // may load: read the generation after it
  l.index.build(svc.fs(), view, lv);
  l.indexLanguage = l.bookLanguage;
  l.indexLevel = lv;
  l.indexGeneration = svc.generation();
  l.indexBuilt = true;
  LOG_INF("LGT", "Word Lens %s/%s: %u glosses in %lu ms", l.bookLanguage.c_str(), lens::levelCode(lv),
          static_cast<unsigned>(l.index.size()), millis() - t0);
}

// One page word with its screen box.
struct Word {
  const char* text;
  int x, w, lineTop, line;
  bool paragraphStart, spaceBefore, hyphenated;
};

}  // namespace

void openBook(const std::string& path, const std::string& dcLanguage) {
  Lens& l = L();
  l.bookPath = path;
  l.bookLanguage.clear();
  l.hits.clear();
  l.relayout = false;
  const std::string primary = primarySubtag(dcLanguage);
  if (primary.empty()) return;
  for (const auto& lang : Service::get().languages()) {
    if (lang == primary || primarySubtag(lang) == primary) {
      l.bookLanguage = lang;
      break;
    }
  }
  LOG_INF("LGT", "Word Lens book language '%s' -> '%s'", dcLanguage.c_str(), l.bookLanguage.c_str());
}

void closeBook() {
  Lens& l = L();
  l.bookPath.clear();
  l.bookLanguage.clear();
  l.hits.clear();
  l.index.clear();
  l.indexBuilt = false;
}

bool active() { return !L().bookLanguage.empty() && state().level != lens::Level::Off; }

const std::string& language() { return L().bookLanguage; }

float lineCompression(const float configured) {
  return active() ? std::max(configured, lens::MIN_LINE_COMPRESSION) : configured;
}

bool available() { return !Service::get().languages().empty(); }

lens::Level level() { return state().level; }

void setLevel(const lens::Level lv) {
  lens::State& st = state();
  if (st.level == lv) return;
  const bool wasActive = active();
  st.level = lv;
  if (!lens::saveState(Service::get().fs(), st)) LOG_ERR("LGT", "Word Lens state not saved");
  // Only Off <-> on moves the spacing floor (and only if the book's spacing is under it).
  if (wasActive != active()) L().relayout = true;
}

bool takeRelayout() {
  Lens& l = L();
  const bool r = l.relayout;
  l.relayout = false;
  return r;
}

void draw(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft, const int marginTop,
          const bool shown) {
  Lens& l = L();
  const bool scanning = renderer.isFontCacheScanning();
  if (!scanning) l.hits.clear();
  if (!active()) return;
  ensureIndex();
  if (l.index.empty()) return;

  // Page words with their boxes, then tokens: glued pieces (style runs, line hyphens) are one word.
  std::vector<Word> words;
  int lineNo = -1;
  const PageLine* lastLine = nullptr;
  forEachPageWord(page, [&](const PageWordRef& r) {
    if (r.line != lastLine) {
      lastLine = r.line;
      lineNo++;
    }
    const int font = r.block->resolvedFontId(renderer, fontId);
    const int x = marginLeft + r.line->xPos + r.block->wordXpos(r.index);
    const int w = scanning ? 0
                           : renderer.getTextAdvanceX(font, r.text, r.block->wordStyle(r.index));
    words.push_back(Word{r.text, x, w, marginTop + r.line->yPos, lineNo, r.paragraphStart, r.spaceBefore,
                         r.hyphenated});
  });
  if (words.empty()) return;

  std::vector<lens::Token> tokens;
  std::vector<std::pair<size_t, size_t>> tokenWords;  // first / last word of each token
  std::string text;
  for (size_t i = 0; i < words.size(); i++) {
    const bool glued = i > 0 && (words[i - 1].hyphenated || !words[i].spaceBefore) && !words[i].paragraphStart;
    if (glued) {
      if (words[i - 1].hyphenated && !text.empty() && text.back() == '-') text.pop_back();
      text += words[i].text;
      tokenWords.back().second = i;
      tokens.back().word = lens::wordKey(text);
      continue;
    }
    text = words[i].text;
    lens::Token t;
    t.word = lens::wordKey(text);
    t.paragraphStart = words[i].paragraphStart;
    tokens.push_back(std::move(t));
    tokenWords.emplace_back(i, i);
  }
  const std::vector<lens::Span> spans = l.index.match(tokens);
  if (spans.empty()) return;

  Service& svc = Service::get();
  const int64_t now = shown ? svc.nowMs() : 0;
  const int64_t day = now > 0 ? svc.dayStart(l.bookLanguage, now) : 0;
  bool stateDirty = false;
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int lineH = renderer.getLineHeight(fontId);
  const int screenW = ui::screenW(renderer);

  for (const auto& span : spans) {
    const lens::Gloss& g = l.index.gloss(span.gloss);
    const size_t a = tokenWords[span.first].first;
    const size_t b = tokenWords[span.first + span.count - 1].second;
    const int line = words[a].line;
    const int lineTop = words[a].lineTop;
    const int glossY = lineTop - GLOSS_GAP - ascender;  // drawText takes the top; SUP sits on its baseline

    if (scanning) {
      renderer.drawText(fontId, words[a].x, glossY, g.text.c_str(), true, EpdFontFamily::SUP);
      renderer.drawText(fontId, words[a].x, glossY, "\xE2\x80\xA6", true, EpdFontFamily::SUP);
      continue;
    }

    // The span's extent on its first line, and room up to halfway to the neighbours.
    size_t lastOnLine = a;
    int x0 = words[a].x, x1 = words[a].x + words[a].w;
    for (size_t i = a + 1; i <= b && words[i].line == line; i++) {
      x1 = std::max(x1, words[i].x + words[i].w);
      lastOnLine = i;
    }
    const int left = a > 0 && words[a - 1].line == line ? (words[a - 1].x + words[a - 1].w + x0) / 2
                                                          : std::max(0, x0 - EDGE_ROOM);
    const int right = lastOnLine + 1 < words.size() && words[lastOnLine + 1].line == line
                          ? (x1 + words[lastOnLine + 1].x) / 2
                          : std::min(screenW, x1 + EDGE_ROOM);
    // A gloss that only repeats the word ("casa" over "casa": same in both languages) adds
    // nothing; the word keeps its tap target and its seen.
    std::string spanKey;
    for (size_t t = span.first; t < span.first + span.count; t++) {
      if (!spanKey.empty()) spanKey += ' ';
      spanKey += tokens[t].word.key;
    }
    const std::string glossKey = lens::itemKey(g.text);
    const bool repeats = glossKey == spanKey || glossKey == lens::itemKey(g.lemma);
    const int room = repeats ? 0 : right - left;
    if (!repeats && room <= 4) continue;
    std::string label = repeats ? std::string() : g.text;
    int tw = renderer.getTextAdvanceX(fontId, label.c_str(), EpdFontFamily::SUP);
    if (tw > room) {
      label = renderer.truncatedText(fontId, label.c_str(), room, EpdFontFamily::SUP);
      tw = renderer.getTextAdvanceX(fontId, label.c_str(), EpdFontFamily::SUP);
    }
    int gx = (x0 + x1) / 2 - tw / 2;
    gx = std::max(left, std::min(gx, right - tw));
    if (repeats) {
      tw = 0;
      gx = x0;
    } else {
      renderer.drawText(fontId, gx, glossY, label.c_str(), true, EpdFontFamily::SUP);
    }

    const int top = lineTop - lineH / 2;
    l.hits.push_back(BoxHit{ui::Box{std::min(x0, gx) - 4, top, std::max(x1, gx + tw) - std::min(x0, gx) + 8,
                                    lineTop + lineH - top},
                            ui::Box{x0, lineTop, x1 - x0, lineH}, span.gloss, a, b});

    if (day > 0 && state().markSeen(day, l.bookPath, lens::itemKey(g.lemma))) {
      svc.record(buildSeen(svc.newId(), now, l.bookLanguage, g.lemma));
      stateDirty = true;
    }
  }
  if (stateDirty && !lens::saveState(svc.fs(), state())) LOG_ERR("LGT", "Word Lens state not saved");
}

bool hitAt(const int x, const int y, Hit& out) {
  Lens& l = L();
  for (const auto& h : l.hits) {
    if (!h.box.contains(x, y)) continue;
    if (!l.indexBuilt || h.gloss < 0 || static_cast<size_t>(h.gloss) >= l.index.size()) return false;
    const lens::Gloss& g = l.index.gloss(h.gloss);
    out.gloss = h.gloss;
    out.lemma = g.lemma;
    out.text = g.text;
    out.firstWord = h.firstWord;
    out.lastWord = h.lastWord;
    out.wordBox = h.word;
    return true;
  }
  return false;
}

}  // namespace linguetta::wordlens
