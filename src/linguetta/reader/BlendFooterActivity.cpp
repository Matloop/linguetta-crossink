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

namespace fui = freeink::ui;

namespace {

enum : int { ID_CLOSE = 0, ID_SAVE = 1, ID_LANG = 2 };

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
  Screen::onEnter();
  useFastRefresh();  // an overlay on the reader page: no full flash
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

void BlendFooterActivity::onAction(int id) {
  switch (id) {
    case ID_SAVE:
      if (canSave()) save();
      break;
    case ID_LANG:
      cycleLanguage();
      break;
    default:
      close();
      break;
  }
}

void BlendFooterActivity::backdrop() {
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
}

void BlendFooterActivity::build(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int side = sidePadding();
  const int innerW = w - 2 * side;
  const bool inBank = mode != Mode::New;
  const std::string lemma = inBank && !bankLemma.empty() ? bankLemma : tap.link.lemma;
  const std::string gloss = inBank && !bankSense.empty() ? bankSense : tap.link.translation;
  const std::string pos = inBank && !bankPos.empty() ? bankPos : tap.link.note;
  const bool showReview = mode == Mode::InBank && !nextReview.empty();
  const bool canSwitch = languages.size() > 1 && mode == Mode::New;

  const int gap = theme.spaceMd;
  const int titleH = renderer.getLineHeight(fontFor(2));
  const int bodyH = renderer.getLineHeight(fontFor(1));
  const int smallH = renderer.getLineHeight(fontFor(0));
  const int glossLines = static_cast<int>(renderer.wrappedText(fontFor(1), gloss.c_str(), innerW, 2).size());

  fui::SheetProps sheet;
  const int band = sheet.grabberMargin + sheet.grabberHeight + sheet.grabberInset;
  int height = band + titleH + gap + glossLines * bodyH + gap;
  if (showReview) height += smallH + gap;
  if (!status.empty()) height += smallH + gap;
  height += theme.rowHeight + gap;  // destination row
  height += theme.rowHeight + gap;  // buttons

  // The strip is at the bottom, unless the word sits under it: then it hangs from the top.
  bool top = false;
  for (const auto& b : tap.linkBoxes) {
    if (b.y + b.h > h - height - 8) top = true;
  }
  sheet.anchor = top ? fui::SheetEdge::Top : fui::SheetEdge::Bottom;
  sheet.dismissAction = ui::ACTION_ITEM;  // a tap outside the sheet: onAction(ID_CLOSE)
  ui.sheet(sheet, static_cast<int16_t>(height));

  // Lemma, its note, and where it stands.
  const fui::Rect head = ui.takeTop(static_cast<int16_t>(titleH), gap);
  const int x0 = head.x + side;
  const char* tag = mode == Mode::InBank ? "NO BANCO" : mode == Mode::Saved ? "SALVO" : nullptr;
  const int tagW = tag ? renderer.getTextWidth(fontFor(0), tag, EpdFontFamily::BOLD) + 2 * gap : 0;
  const std::string lemmaText = renderer.truncatedText(fontFor(2), lemma.c_str(), innerW - tagW - gap, EpdFontFamily::BOLD);
  ui.target().text(fui::Rect{static_cast<int16_t>(x0), head.y, static_cast<int16_t>(innerW), head.height}, lemmaText.c_str(),
                   style(ui, 2, true));
  if (!pos.empty()) {
    const int lw = renderer.getTextWidth(fontFor(2), lemmaText.c_str(), EpdFontFamily::BOLD) + gap;
    const int posMax = innerW - lw - (tagW ? tagW + gap : 0);
    if (posMax > 30) {
      const std::string p = renderer.truncatedText(fontFor(0), pos.c_str(), posMax);
      ui.target().text(fui::Rect{static_cast<int16_t>(x0 + lw), head.y, static_cast<int16_t>(posMax), head.height}, p.c_str(),
                       style(ui, 0));
    }
  }
  if (tag) {
    const fui::Rect tagRect{static_cast<int16_t>(x0 + innerW - tagW), static_cast<int16_t>(head.y + (head.height - smallH - 8) / 2),
                            static_cast<int16_t>(tagW), static_cast<int16_t>(smallH + 8)};
    ui.target().stroke(tagRect, fui::Paint::solid(fui::Color::Black), 1, 4);
    ui.target().text(tagRect, tag, style(ui, 0, true, fui::TextAlign::Center));
  }

  paragraph(ui, gloss, 1, false, fui::TextAlign::Left, 2);
  ui.spacer(gap);
  if (showReview) {
    paragraph(ui, nextReview, 0);
    ui.spacer(gap);
  }
  if (!status.empty()) {
    paragraph(ui, status, 0, true);
    ui.spacer(gap);
  }

  // Destination deck; with more than one study language a tap switches it for this book.
  const fui::Rect dest = ui.takeTop(theme.rowHeight, gap);
  const char* prefix = mode == Mode::New ? "Salvar em" : mode == Mode::Saved ? "Salvo em" : "No baralho";
  const std::string deckName = languageName(language);  // "Italiano": the deck name is too long here
  const int switchW = canSwitch ? renderer.getTextWidth(fontFor(1), "trocar", EpdFontFamily::BOLD) + 4 * gap : 0;
  ui.target().text(fui::Rect{static_cast<int16_t>(dest.x + side), dest.y,
                             static_cast<int16_t>(innerW - switchW - (switchW ? gap : 0)), dest.height},
                   (std::string(prefix) + " " + deckName).c_str(), style(ui, 1));
  if (canSwitch) {
    button(ui, fui::Rect{static_cast<int16_t>(dest.x + side + innerW - switchW), dest.y, static_cast<int16_t>(switchW), dest.height},
           "trocar", ID_LANG);
  }

  const fui::Rect buttons = ui.takeTop(theme.rowHeight, gap);
  if (canSave()) {
    const int saveW = (innerW - gap) * 2 / 3;
    button(ui, fui::Rect{static_cast<int16_t>(buttons.x + side), buttons.y, static_cast<int16_t>(saveW), buttons.height},
           mode == Mode::New ? "Salvar" : "Salvar encontro", ID_SAVE, true);
    button(ui, fui::Rect{static_cast<int16_t>(buttons.x + side + saveW + gap), buttons.y,
                         static_cast<int16_t>(innerW - saveW - gap), buttons.height},
           "Fechar", ID_CLOSE);
    if (focusedId() < 0) setFocus(ID_SAVE);
  } else {
    button(ui, fui::Rect{static_cast<int16_t>(buttons.x + side), buttons.y, static_cast<int16_t>(innerW), buttons.height},
           "Fechar", ID_CLOSE, true);
    if (focusedId() < 0) setFocus(ID_CLOSE);
  }
}

}  // namespace linguetta
