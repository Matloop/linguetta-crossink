#include "BlendFooterActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "linguetta/core/Ops.h"
#include "linguetta/device/Linguetta.h"
#include "WordLensReader.h"

namespace linguetta {

namespace {

std::string reviewLine(const ViewEntry& e, int64_t now) {
  if (e.known) return "Marcada como conhecida";
  if (e.card.state == 0) return "Nova no baralho";
  if (now <= 0) return "";
  const double days = static_cast<double>(e.card.dueMs - now) / fsrs::MS_PER_DAY;
  if (days <= 0) return "Revis\xC3\xA3o pendente hoje";
  const int n = static_cast<int>(std::ceil(days));
  if (n <= 1) return days * 24 < 20 ? "Pr\xC3\xB3xima revis\xC3\xA3o hoje" : "Pr\xC3\xB3xima revis\xC3\xA3o amanh\xC3\xA3";
  return "Pr\xC3\xB3xima revis\xC3\xA3o em " + std::to_string(n) + " dias";
}

std::string fileStem(const std::string& path) {
  std::string name = path.substr(path.find_last_of('/') + 1);
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

void BlendFooterActivity::onEnter() {
  Activity::onEnter();
  Service& svc = Service::get();
  koHash = svc.koHash(tap.bookPath);
  if (tap.lens) {
    language = tap.language;
    languages = {language};
  } else {
    languages = svc.blendLanguages();
    language = svc.blendLanguage(tap.link.lemma, koHash, fileStem(tap.bookPath) + " " + tap.title, tap.bookLanguage);
  }
  lookUp();
  if (mode == Mode::InBank && !tap.lens) {
    const int64_t now = svc.nowMs();
    // One `seen` per tap. Without a trusted clock nothing is recorded (contract section 4).
    if (now > 0) {
      svc.record(buildSeen(svc.newId(), now, language, tap.link.lemma));
    } else {
      LOG_INF("LGT", "Clock not trusted; seen not recorded");
    }
  }
  requestUpdate();
}

void BlendFooterActivity::lookUp() {
  Service& svc = Service::get();
  LocalView& view = svc.view(language);
  const int idx = view.find(tap.link.lemma);
  mode = Mode::New;
  bankLemma.clear();
  bankSense.clear();
  bankPos.clear();
  nextReview.clear();
  if (idx >= 0) {
    mode = Mode::InBank;
    ItemDetails d;
    if (view.details(svc.fs(), idx, d)) {
      bankLemma = d.lemma;
      bankSense = d.sense;
      bankPos = d.pos.empty() ? d.note : d.pos;
    }
    nextReview = reviewLine(view.entry(idx), svc.nowMs());
  }
}

void BlendFooterActivity::cycleLanguage() {
  if (languages.size() < 2) return;
  auto it = std::find(languages.begin(), languages.end(), language);
  language = (it == languages.end() || it + 1 == languages.end()) ? languages.front() : *(it + 1);
  Service::get().setBookLanguage(koHash, language);
  lookUp();
  status.clear();
  requestUpdate();
}

void BlendFooterActivity::save() {
  Service& svc = Service::get();
  const int64_t now = svc.nowMs();
  if (now <= 0) {
    status = "Conecte ao Wi-Fi para acertar a hora";
    requestUpdate();
    return;
  }
  SaveInput in;
  in.language = language;
  in.lemma = tap.link.lemma;
  in.sense = tap.link.translation;
  in.note = tap.link.note;
  if (tap.lens) {
    // A new encounter of the bank item: keep its own fields.
    if (!bankLemma.empty()) in.lemma = bankLemma;
    in.sense = bankSense;
    in.note.clear();
  }
  in.form = tap.context.form.empty() ? tap.link.lemma : tap.context.form;
  in.sentence = tap.context.sentence;
  in.encounterSense = tap.link.translation;
  in.chapter = tap.chapter;
  in.spine = tap.spine;
  in.percent = tap.percent;
  in.koHash = koHash;
  in.title = tap.title;
  // No OPF title: the file name without extension (contract section 4).
  if (in.title.empty()) in.title = fileStem(tap.bookPath);
  in.author = tap.author;
  in.workLanguage = tap.bookLanguage.substr(0, tap.bookLanguage.find('-'));
  const std::string line = buildSave(svc.newId(), now, in);
  if (line.empty() || !svc.record(line)) {
    status = "N\xC3\xA3o foi poss\xC3\xADvel salvar no cart\xC3\xA3o SD";
    requestUpdate();
    return;
  }
  mode = Mode::Saved;
  status.clear();
  requestUpdate();
}

void BlendFooterActivity::close() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BlendFooterActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (canSave()) {
      save();
    } else {
      close();
    }
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (canSave() && saveBox.contains(x, y)) {
      save();
    } else if (mode == Mode::New && langBox.contains(x, y)) {
      cycleLanguage();
    } else if (closeBox.contains(x, y) || !strip.contains(x, y)) {
      close();
    }
  }
}

void BlendFooterActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (page) {
    page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
    wordlens::draw(renderer, *page, SETTINGS.getReaderFontId(), marginLeft, marginTop, false);
  }
  for (const auto& b : tap.linkBoxes) {
    // Link boxes span the full line height (leading included); a border at their
    // edge would run through the line above, so stay inside the box.
    ui::shade(renderer, b.x - 2, b.y + 3, b.w + 4, b.h - 3);
    renderer.drawRect(b.x - 3, b.y + 2, b.w + 6, b.h - 1, 2, true);
  }

  const int w = ui::screenW(renderer);
  const int h = ui::screenH(renderer);
  const int padX = 24;
  const int innerW = w - 2 * padX;
  const bool inBank = mode != Mode::New;
  const std::string lemma = inBank && !bankLemma.empty() ? bankLemma : tap.link.lemma;
  const std::string gloss = inBank && !bankSense.empty() ? bankSense : tap.link.translation;
  const std::string pos = inBank && !bankPos.empty() ? bankPos : tap.link.note;

  const int lemmaH = renderer.getLineHeight(ui::fontTitle());
  const auto glossLines = renderer.wrappedText(ui::fontLarge(), gloss.c_str(), innerW, 2);
  int height = 18 + lemmaH + 4 + static_cast<int>(glossLines.size()) * renderer.getLineHeight(ui::fontLarge()) + 14;
  const bool showReview = mode == Mode::InBank && !nextReview.empty();
  if (showReview) height += renderer.getLineHeight(ui::fontSmall()) + 10;
  if (!status.empty()) height += renderer.getLineHeight(ui::fontSmall()) + 8;
  const int smallH = renderer.getLineHeight(ui::fontSmall());
  const int langRowH = smallH + 18;  // tall enough to tap
  height += langRowH + ui::BUTTON_H + 22;

  // Bottom strip, unless the word sits under it: then the strip goes to the top.
  bool top = false;
  for (const auto& b : tap.linkBoxes) {
    if (b.y + b.h > h - height - 8) top = true;
  }
  strip = ui::Box{0, top ? 0 : h - height, w, height};
  renderer.fillRect(strip.x, strip.y, strip.w, strip.h, false);
  renderer.fillRect(0, top ? strip.y + strip.h - 3 : strip.y, w, 3, true);

  int y = strip.y + 18;
  const std::string lemmaText = renderer.truncatedText(ui::fontTitle(), lemma.c_str(), innerW - 130, EpdFontFamily::BOLD);
  renderer.drawText(ui::fontTitle(), padX, y, lemmaText.c_str(), true, EpdFontFamily::BOLD);
  int x = padX + renderer.getTextWidth(ui::fontTitle(), lemmaText.c_str(), EpdFontFamily::BOLD) + 10;
  const char* chipLabel = mode == Mode::InBank ? "NO BANCO" : mode == Mode::Saved ? "SALVO" : nullptr;
  const int chipW = chipLabel ? renderer.getTextWidth(SMALL_FONT_ID, chipLabel, EpdFontFamily::BOLD) + 16 : 0;
  if (!pos.empty()) {
    const int posMax = w - padX - x - (chipW ? chipW + 10 : 0);
    if (posMax > 30) {
      const std::string p = renderer.truncatedText(ui::fontSmall(), pos.c_str(), posMax, EpdFontFamily::ITALIC);
      renderer.drawText(ui::fontSmall(), x, y + lemmaH - renderer.getLineHeight(ui::fontSmall()) - 2, p.c_str(), true,
                        EpdFontFamily::ITALIC);
    }
  }
  if (chipLabel) ui::chip(renderer, w - padX - chipW, y + (lemmaH - renderer.getLineHeight(SMALL_FONT_ID) - 6) / 2, chipLabel);
  y += lemmaH + 4;
  for (const auto& line : glossLines) {
    renderer.drawText(ui::fontLarge(), padX, y, line.c_str(), true);
    y += renderer.getLineHeight(ui::fontLarge());
  }
  y += 10;
  if (showReview) {
    renderer.drawText(ui::fontSmall(), padX, y, nextReview.c_str(), true);
    y += renderer.getLineHeight(ui::fontSmall()) + 10;
  }
  if (!status.empty()) {
    renderer.drawText(ui::fontSmall(), padX, y, status.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(ui::fontSmall()) + 8;
  }
  // Destination deck; with more than one study language a tap switches it for this book.
  {
    const std::string deck = languageName(language);  // "Italiano": the deck name is too long here
    const char* prefix = mode == Mode::New ? "Salvar em" : mode == Mode::Saved ? "Salvo em" : "No baralho";
    // Only before saving: a word already in the bank has had its `seen` recorded in this deck.
    const bool canSwitch = languages.size() > 1 && mode == Mode::New;
    const char* hint = "trocar";
    const int hintW = canSwitch ? renderer.getTextWidth(ui::fontSmall(), hint, EpdFontFamily::BOLD) + 20 : 0;
    const int prefixW = renderer.getTextWidth(ui::fontSmall(), prefix) + 6;
    const std::string deckText =
        renderer.truncatedText(ui::fontSmall(), deck.c_str(), innerW - prefixW - hintW - 12, EpdFontFamily::BOLD);
    const int ty = y + (langRowH - smallH) / 2 - 2;
    renderer.drawText(ui::fontSmall(), padX, ty, prefix, true);
    renderer.drawText(ui::fontSmall(), padX + prefixW, ty, deckText.c_str(), true, EpdFontFamily::BOLD);
    if (canSwitch) {
      const int hx = w - padX - hintW;
      renderer.drawRect(hx, y + 2, hintW, langRowH - 6, 2, true);
      renderer.drawText(ui::fontSmall(), hx + 10, ty, hint, true, EpdFontFamily::BOLD);
      langBox = ui::Box{padX, y, innerW, langRowH};
    } else {
      langBox = ui::Box{};
    }
    y += langRowH + 4;
  }
  if (canSave()) {
    const int gap = 10;
    const int saveW = (innerW - gap) * 2 / 3;
    saveBox = ui::button(renderer, ui::Box{padX, y, saveW, ui::BUTTON_H}, mode == Mode::New ? "Salvar" : "Salvar encontro",
                         true);
    closeBox = ui::button(renderer, ui::Box{padX + saveW + gap, y, innerW - saveW - gap, ui::BUTTON_H}, "Fechar", false);
  } else {
    saveBox = ui::Box{};
    closeBox = ui::button(renderer, ui::Box{padX, y, innerW, ui::BUTTON_H}, "Fechar", false);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

}  // namespace linguetta
