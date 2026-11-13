#include "ReviewActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>

#include "fontIds.h"
#include "linguetta/core/History.h"
#include "linguetta/core/LocalView.h"
#include "linguetta/core/Ops.h"
#include "linguetta/core/Sources.h"
#include "linguetta/core/Text.h"
#include "linguetta/device/Linguetta.h"

namespace linguetta {

namespace fui = freeink::ui;

namespace {

enum : int { ID_REVEAL = 1, ID_GRADE = 10, ID_DONE = 20, ID_EXTRA = 21, ID_CONTINUE = 22 };

constexpr int EXTRA_NEW_STEP = 5;

// "Faltam 3": counts the repeats too, so the header always moves as you answer.
std::string remainingLabel(int n) { return (n == 1 ? "Falta " : "Faltam ") + std::to_string(n); }

const char* const GRADE_NAMES[5] = {"", "Errei", "Dif\xC3\xAD" "cil", "Bom", "F\xC3\xA1" "cil"};

std::string leechText(int lapses) {
  return "Teimosa: errou " + std::to_string(lapses) + (lapses == 1 ? " vez" : " vezes") +
         ". Vai esperar at\xC3\xA9 voc\xC3\xAA encontr\xC3\xA1-la num livro ou s\xC3\xA9rie.";
}

std::string join(std::initializer_list<std::string> parts) {
  std::string out;
  for (const auto& p : parts) {
    if (p.empty()) continue;
    if (!out.empty()) out += " \xC2\xB7 ";
    out += p;
  }
  return out;
}

}  // namespace

void ReviewActivity::onEnter() {
  Screen::onEnter();
  Service& svc = Service::get();
  shortTitle = ref.linguetta ? languageName(ref.language) : ref.name;
  deckTitle = ref.linguetta ? svc.deckName(ref.language) : ref.name;
  if (ref.linguetta && ref.deck >= 0) {
    const BankHeader& hdr = svc.view(ref.language).header();
    if (ref.deck < static_cast<int>(hdr.decks.size())) deckTitle = shortTitle = hdr.decks[ref.deck].name;
  }
  const int64_t now = svc.nowMs();
  if (now <= 0) {
    mode = Mode::Empty;
    emptyText = "Conecte ao Wi-Fi para acertar a hora antes de revisar.";
    requestUpdate();
    return;
  }
  if (ref.linguetta) {
    LocalView& view = svc.view(ref.language);
    source = std::make_unique<ViewSource>(
        view, svc.fs(), [](const std::string& line) { return Service::get().record(line); },
        [] { return Service::get().newId(); });
  } else {
    if (!tsv.load(svc.fs(), ref.name)) {
      mode = Mode::Empty;
      emptyText = "N\xC3\xA3o foi poss\xC3\xADvel ler o deck /flashcards/" + ref.name;
      requestUpdate();
      return;
    }
    // The account's retention, when a bank is synced.
    for (const auto& lang : svc.languages()) {
      const BankHeader& hdr = svc.view(lang).header();
      if (hdr.present) {
        tsv.setRetention(hdr.desiredRetention);
        break;
      }
    }
    source = std::make_unique<TsvSource>(tsv, svc.fs());
  }
  startQueue(now);
  if (session.done()) {
    mode = Mode::Empty;
    emptyText = canAddExtra() ? "As novas de hoje acabaram." : "Nada para revisar hoje neste deck.";
    if (canAddExtra()) setFocus(ID_EXTRA);
  }
  requestUpdate();
}

void ReviewActivity::startQueue(int64_t now) {
  Service& svc = Service::get();
  std::vector<int> queue;
  if (ref.linguetta) {
    queue = svc.view(ref.language).queue(now, ref.deck);
  } else {
    int perDay = 0;
    for (const auto& lang : svc.languages()) {
      const BankHeader& hdr = svc.view(lang).header();
      if (hdr.present) {
        perDay = hdr.newPerDay;
        break;
      }
    }
    queue = tsv.queue(now, svc.dayStart("", now), perDay > 0 ? perDay : 10);
  }
  session.start(*source, deckTitle, std::move(queue), now);
  historySaved = false;
  if (!session.done()) {
    loadCard();
    mode = Mode::Front;
    session.shown(now);
  }
}

bool ReviewActivity::canAddExtra() const {
  if (!ref.linguetta) return false;
  Service& svc = Service::get();
  const int64_t now = svc.nowMs();
  if (now <= 0) return false;
  LocalView& view = svc.view(ref.language);
  return view.header().serverQueue() && view.newAllowance(now) == 0 && view.newBeyondBudget(now, ref.deck) > 0;
}

void ReviewActivity::addExtraNew() {
  Service& svc = Service::get();
  const int64_t now = svc.nowMs();
  if (now <= 0) return;
  const std::string line = buildExtraNew(svc.newId(), now, ref.language, EXTRA_NEW_STEP);
  if (line.empty() || !svc.record(line)) {
    status = "N\xC3\xA3o foi poss\xC3\xADvel gravar no cart\xC3\xA3o SD";
    requestUpdate();
    return;
  }
  status.clear();
  leeches.clear();
  startQueue(now);
  if (session.done()) {
    mode = Mode::Empty;
    emptyText = "Nada para revisar hoje neste deck.";
    setFocus(ID_DONE);
  }
  requestFullRefresh();
  requestUpdate();
}

void ReviewActivity::loadCard() {
  face = Face{};
  const int i = session.current();
  if (i < 0) return;
  Service& svc = Service::get();
  if (ref.linguetta) {
    LocalView& view = svc.view(ref.language);
    ItemDetails d;
    if (view.details(svc.fs(), i, d)) {
      face.lemma = d.lemma;
      face.ipa = d.ipa;
      face.pos = d.pos.empty() ? d.note : d.pos;
      face.sense = d.sense;
      face.ficha = d.ficha;
      face.cloze = d.cloze;
      if (d.hasEncounter) {
        face.sentence = d.encounter.sentence;
        face.form = d.encounter.form;
        face.work = d.encounter.work;
        face.chapter = d.encounter.chapter;
      }
    } else {
      face.lemma = view.entry(i).key;
    }
    face.createdMs = view.entry(i).cardCreatedMs;
  } else {
    face.lemma = tsv.card(i).front;
    face.sense = tsv.card(i).back;
  }
  double retention = fsrs::DEFAULT_RETENTION;
  if (ref.linguetta) {
    retention = svc.view(ref.language).header().desiredRetention;
  } else {
    for (const auto& lang : svc.languages()) {
      const BankHeader& hdr = svc.view(lang).header();
      if (hdr.present) {
        retention = hdr.desiredRetention;
        break;
      }
    }
  }
  fsrs::previewDays(source->state(i), source->seed(i), svc.nowMs(), intervals, retention);
}

void ReviewActivity::answer(int rating) {
  Service& svc = Service::get();
  const int64_t now = svc.nowMs();
  if (now <= 0) {
    status = "Rel\xC3\xB3gio n\xC3\xA3o acertado: resposta n\xC3\xA3o gravada";
    requestUpdate();
    return;
  }
  if (!session.answer(rating, now)) {
    status = "N\xC3\xA3o foi poss\xC3\xADvel gravar no cart\xC3\xA3o SD";
    requestUpdate();
    return;
  }
  status.clear();
  if (session.lastLeech() >= 0) {
    // The teimosa leaves the session: say so before moving on.
    leeches.push_back({face.lemma, source->state(session.lastLeech()).lapses});
    mode = Mode::Leech;
    setFocus(ID_CONTINUE);
    requestFullRefresh();
    requestUpdate();
    return;
  }
  if (session.done()) {
    endSession();
    return;
  }
  loadCard();
  mode = Mode::Front;
  session.shown(now);
  // A cleaner refresh every 10 answers keeps e-ink ghosting down.
  if (session.answers() % 10 == 0) requestFullRefresh();
  requestUpdate();
}

void ReviewActivity::endSession() {
  if (!historySaved && session.answers() > 0) {
    historySaved = true;
    if (!appendHistory(Service::get().fs(), session.history())) LOG_ERR("LGT", "History append failed");
  }
  mode = Mode::Summary;
  setFocus(canAddExtra() ? ID_EXTRA : ID_DONE);
  requestFullRefresh();
  requestUpdate();
}

void ReviewActivity::onBack() {
  if ((mode == Mode::Front || mode == Mode::Back || mode == Mode::Leech) && session.answers() > 0) {
    endSession();
    return;
  }
  finish();
}

void ReviewActivity::onAction(int id) {
  if (mode == Mode::Front && id == ID_REVEAL) {
    mode = Mode::Back;
    // No grade starts focused: a filled "Bom" read as the app's suggestion. The side buttons
    // pick one first (Confirm does nothing until then); touch just taps.
    setFocus(-1);
    requestUpdate();
  } else if (mode == Mode::Back && id > ID_GRADE && id <= ID_GRADE + 4) {
    answer(id - ID_GRADE);
  } else if (mode == Mode::Leech && id == ID_CONTINUE) {
    if (session.done()) {
      endSession();
      return;
    }
    loadCard();
    mode = Mode::Front;
    session.shown(Service::get().nowMs());
    requestFullRefresh();
    requestUpdate();
  } else if ((mode == Mode::Summary || mode == Mode::Empty) && id == ID_EXTRA) {
    addExtraNew();
  } else if ((mode == Mode::Summary || mode == Mode::Empty) && id == ID_DONE) {
    finish();
  }
}

std::string ReviewActivity::title() const {
  switch (mode) {
    case Mode::Summary:
      return "Sess\xC3\xA3o conclu\xC3\xAD" "da";
    case Mode::Empty:
      return deckTitle;
    default:
      return shortTitle;
  }
}

std::string ReviewActivity::subtitle() const {
  return mode == Mode::Front || mode == Mode::Back || mode == Mode::Leech ? remainingLabel(session.remaining()) : "";
}

const char* ReviewActivity::confirmHint() const { return mode == Mode::Front ? "Mostrar" : "Escolher"; }

void ReviewActivity::build(ui::Ui& ui) {
  switch (mode) {
    case Mode::Front:
      buildFront(ui);
      break;
    case Mode::Back:
      buildBack(ui);
      break;
    case Mode::Leech:
      buildLeech(ui);
      break;
    case Mode::Summary:
      buildSummary(ui);
      break;
    case Mode::Empty:
      buildEmpty(ui);
      break;
  }
}

void ReviewActivity::buildFront(ui::Ui& ui) {
  const fui::Rect button = bottomButton(ui, "Mostrar resposta", ID_REVEAL, true);
  (void)button;
  const fui::Rect body = ui.body();
  const int side = sidePadding();
  if (focusedId() < 0) setFocus(ID_REVEAL);
  // Only the word, big and centred: the sentence is context for the answer, not a hint.
  int top = body.y;
  if (face.ficha) {
    if (face.cloze) {
      const fui::Rect hint = ui.takeTop(renderer.getLineHeight(fontFor(0)), ui.theme().spaceMd);
      ui.target().text(hint, "LACUNA", style(ui, 0, true, fui::TextAlign::Center));
      top = ui.body().y;
    }
    drawFicha(face.lemma, top, body.y + body.height, 10, side, fontFor(2));
  } else {
    drawWord(face.lemma, top, body.y + body.height, side);
  }
  // The whole body reveals too (touch).
  tapArea(ui, body, ID_REVEAL);
}

int ReviewActivity::drawFicha(const std::string& text, int top, int bottom, int maxLines, const int sidePad,
                              const int font) {
  const int w = renderer.getScreenWidth();
  const int innerW = w - 2 * sidePad;
  const int lh = renderer.getLineHeight(font);
  if (bottom >= 0) maxLines = std::max(1, std::min(maxLines, (bottom - top) / lh));
  const auto lines = renderer.wrappedText(font, text.c_str(), innerW, maxLines, EpdFontFamily::REGULAR);
  int y = bottom < 0 ? top : top + std::max(0, (bottom - top - static_cast<int>(lines.size()) * lh) / 2);
  for (const auto& line : lines) {
    const int tw = renderer.getTextWidth(font, line.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(font, (w - tw) / 2, y, line.c_str(), true, EpdFontFamily::REGULAR);
    y += lh;
  }
  return y;
}

int ReviewActivity::drawWord(const std::string& word, int top, int bottom, const int sidePad) {
  const int w = renderer.getScreenWidth();
  const int innerW = w - 2 * sidePad;
  // Display font unless a single token is too wide for it.
  int font = ui::fontDisplay();
  auto lines = renderer.wrappedText(font, word.c_str(), innerW, 3, EpdFontFamily::BOLD);
  for (const auto& l : lines) {
    if (renderer.getTextWidth(font, l.c_str(), EpdFontFamily::BOLD) > innerW) {
      font = fontFor(2);
      lines = renderer.wrappedText(font, word.c_str(), innerW, 3, EpdFontFamily::BOLD);
      break;
    }
  }
  const int lh = renderer.getLineHeight(font);
  int y = bottom < 0 ? top : top + std::max(0, (bottom - top - static_cast<int>(lines.size()) * lh) / 2);
  for (const auto& line : lines) {
    const int tw = renderer.getTextWidth(font, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(font, (w - tw) / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lh;
  }
  return y;
}

void ReviewActivity::buildBack(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int w = renderer.getScreenWidth();
  const int side = sidePadding();
  const int innerW = w - 2 * side;
  const auto centred = [&](int font, int y, const std::string& text, EpdFontFamily::Style style, int maxLines) {
    for (const auto& line : renderer.wrappedText(font, text.c_str(), innerW, maxLines, style)) {
      const int tw = renderer.getTextWidth(font, line.c_str(), style);
      renderer.drawText(font, (w - tw) / 2, y, line.c_str(), true, style);
      y += renderer.getLineHeight(font);
    }
    return y;
  };

  // Grades at the bottom, two by two: "Errei · 1 min".
  std::string labels[4];
  for (int g = 1; g <= 4; g++) labels[g - 1] = std::string(GRADE_NAMES[g]) + " \xC2\xB7 " + fsrs::formatInterval(intervals[g - 1]);
  const int gap = theme.spaceMd;
  for (int row = 1; row >= 0; row--) {
    const fui::Rect r = ui.takeBottom(theme.rowHeight, gap);
    const int bw = (r.width - 2 * side - gap) / 2;
    for (int col = 0; col < 2; col++) {
      const int g = row * 2 + col + 1;
      button(ui, fui::Rect{static_cast<int16_t>(r.x + side + col * (bw + gap)), r.y, static_cast<int16_t>(bw), r.height},
             labels[g - 1].c_str(), ID_GRADE + g);
    }
  }
  if (!status.empty()) {
    const fui::Rect st = ui.takeBottom(renderer.getLineHeight(fontFor(0)), gap);
    ui.target().text(st, status.c_str(), style(ui, 0, true, fui::TextAlign::Center));
  }

  const fui::Rect body = ui.body();
  const int bottom = body.y + body.height;
  const int fontTitle = fontFor(2), fontBody = fontFor(1), fontSmall = fontFor(0);
  int y;
  if (face.ficha) {
    // Front small, a rule, then the back as the answer.
    y = centred(fontBody, body.y + gap, face.lemma, EpdFontFamily::REGULAR, 4);
    y += gap;
    ui.target().fill(fui::Rect{static_cast<int16_t>(w / 2 - 40), static_cast<int16_t>(y), 80, 2},
                     fui::Paint::solid(fui::Color::Black));
    y += gap * 2;
    drawFicha(face.sense, y, bottom - gap, 12, side, fontTitle);
    return;
  }
  // Word, then the answer: the meaning is the biggest thing after the word.
  y = drawWord(face.lemma, body.y + gap * 2, -1, side);
  const std::string meta = join({face.ipa, face.pos});
  if (!meta.empty()) y = centred(ui::fontIpa(), y + 4, meta, EpdFontFamily::REGULAR, 1);
  y += gap * 2;
  ui.target().fill(fui::Rect{static_cast<int16_t>(w / 2 - 40), static_cast<int16_t>(y), 80, 2},
                   fui::Paint::solid(fui::Color::Black));
  y += gap * 2;
  if (!face.sense.empty()) y = centred(fontTitle, y, face.sense, EpdFontFamily::REGULAR, 3);

  // Context, small and below: the sentence it was met in (3 lines at most) and the work.
  const int lh = renderer.getLineHeight(fontSmall);
  if (!face.sentence.empty()) {
    const size_t at = findCaseless(face.sentence, face.form.empty() ? face.lemma : face.form);
    const size_t len = face.form.empty() ? face.lemma.size() : face.form.size();
    const int room = std::max(1, (bottom - y - gap * 3 - 2 * lh) / lh);
    y = ui::highlightedSentence(renderer, fontSmall, y + gap * 2, innerW, face.sentence, at,
                                at == std::string::npos ? at : at + len, std::min(room, 3));
  }
  if (!face.work.empty() && y + lh <= bottom) centred(fontSmall, y + 6, face.work, EpdFontFamily::REGULAR, 1);
}

void ReviewActivity::buildSummary(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int side = sidePadding();
  const int gap = theme.spaceMd;
  const bool extra = canAddExtra();

  // Buttons first (bottom), then the body fills from the top.
  bottomButton(ui, "Voltar aos decks", ID_DONE, true);
  if (extra) {
    const fui::Rect r = ui.takeBottom(theme.rowHeight, gap);
    button(ui, fui::Rect{static_cast<int16_t>(r.x + side), r.y, static_cast<int16_t>(r.width - 2 * side), r.height},
           ("+" + std::to_string(EXTRA_NEW_STEP) + " novas hoje").c_str(), ID_EXTRA);
    const fui::Rect note = ui.takeBottom(renderer.getLineHeight(fontFor(0)), gap);
    ui.target().text(fui::Rect{static_cast<int16_t>(note.x + side), note.y, static_cast<int16_t>(note.width - 2 * side), note.height},
                     "As novas de hoje acabaram.", style(ui, 0, true));
  }

  paragraph(ui, deckTitle, 2, true);
  const int cards = session.cards();
  paragraph(ui, std::to_string(cards) + (cards == 1 ? " cart\xC3\xA3o" : " cart\xC3\xB5" "es") + " \xC2\xB7 " +
                    formatDuration(session.durationMs()),
            1);
  ui.spacer(gap);

  const int errors = session.firstGrades(1);
  const fui::Rect boxes = ui.takeTop(static_cast<int16_t>(theme.rowHeight * 3 / 2), gap);
  const int bw = (boxes.width - 2 * side - gap) / 2;
  const std::string hits = std::to_string(cards - errors), misses = std::to_string(errors);
  fui::MetricCardProps card;
  card.valueText = style(ui, 2, true, fui::TextAlign::Center);
  card.captionText = style(ui, 0, false, fui::TextAlign::Center);
  card.value = hits.c_str();
  card.caption = "acertos";
  fui::metricCard(ui.frame(), fui::Rect{static_cast<int16_t>(boxes.x + side), boxes.y, static_cast<int16_t>(bw), boxes.height}, card);
  card.value = misses.c_str();
  card.caption = "erros";
  fui::metricCard(ui.frame(),
                  fui::Rect{static_cast<int16_t>(boxes.x + side + bw + gap), boxes.y, static_cast<int16_t>(bw), boxes.height},
                  card);

  // Teimosas or "+5" need the room: the grades shrink to one line.
  const bool compact = extra || !leeches.empty();
  if (compact) {
    std::string grades;
    for (int g = 1; g <= 4; g++) {
      grades += (g > 1 ? " \xC2\xB7 " : "") + std::string(GRADE_NAMES[g]) + " " + std::to_string(session.firstGrades(g));
    }
    paragraph(ui, grades, 0);
  } else {
    for (int g = 1; g <= 4; g++) {
      const fui::Rect row = ui.takeTop(theme.rowHeight);
      const int n = session.firstGrades(g);
      const int labelW = renderer.getTextWidth(fontFor(1), "Dif\xC3\xAD" "cil") + gap;
      const int countW = renderer.getTextWidth(fontFor(1), "0000");
      ui.target().text(fui::Rect{static_cast<int16_t>(row.x + side), row.y, static_cast<int16_t>(labelW), row.height},
                       GRADE_NAMES[g], style(ui, 1));
      const int barX = row.x + side + labelW, barW = row.width - 2 * side - labelW - countW - gap;
      const int barH = std::max(8, row.height / 3);
      fui::ProgressBarProps bar;
      bar.value = n;
      bar.max = std::max(1, cards);
      bar.border = fui::Paint::solid(fui::Color::Black);
      bar.borderWidth = 1;
      bar.minFill = n > 0 ? 4 : 0;
      fui::progressBar(ui.frame(), fui::Rect{static_cast<int16_t>(barX), static_cast<int16_t>(row.y + (row.height - barH) / 2),
                                             static_cast<int16_t>(barW), static_cast<int16_t>(barH)},
                       bar);
      ui.target().text(fui::Rect{static_cast<int16_t>(row.right() - side - countW), row.y, static_cast<int16_t>(countW), row.height},
                       std::to_string(n).c_str(), style(ui, 1, false, fui::TextAlign::Right));
    }
  }
  std::string note;
  if (ref.linguetta) {
    const int n = session.answers();
    note = std::to_string(n) + (n == 1 ? " revis\xC3\xA3o na fila de envio. Ela sobe" : " revis\xC3\xB5" "es na fila de envio. Elas sobem") +
           " no pr\xC3\xB3ximo sync.";
  } else {
    note = "Deck local: o progresso fica s\xC3\xB3 neste aparelho.";
  }
  paragraph(ui, note, 0, false, fui::TextAlign::Left, compact ? 2 : 3);

  if (!leeches.empty()) {
    // One line per teimosa ("ostinato · errou 8 vezes"), then what happens to them.
    ui.spacer(gap);
    paragraph(ui, leeches.size() == 1 ? "Teimosa" : "Teimosas", 1, true);
    const int lh = renderer.getLineHeight(fontFor(1));
    for (size_t k = 0; k < leeches.size(); k++) {
      if (ui.body().height < lh * 3 && k + 1 < leeches.size()) {
        paragraph(ui, "e mais " + std::to_string(leeches.size() - k), 1);
        break;
      }
      const Leech& l = leeches[k];
      paragraph(ui, l.word + " \xC2\xB7 errou " + std::to_string(l.lapses) + (l.lapses == 1 ? " vez" : " vezes"), 1);
    }
    paragraph(ui,
              leeches.size() == 1 ? "Vai esperar at\xC3\xA9 voc\xC3\xAA encontr\xC3\xA1-la num livro ou s\xC3\xA9rie."
                                  : "V\xC3\xA3o esperar at\xC3\xA9 voc\xC3\xAA encontr\xC3\xA1-las num livro ou s\xC3\xA9rie.",
              0, false, fui::TextAlign::Left, 2);
  }
}

void ReviewActivity::buildLeech(ui::Ui& ui) {
  const auto& theme = ui.theme();
  bottomButton(ui, "Continuar", ID_CONTINUE, true);
  if (focusedId() < 0) setFocus(ID_CONTINUE);
  const Leech& l = leeches.back();
  const fui::Rect body = ui.body();
  int y = drawWord(l.word, body.y + theme.spaceLg, -1, sidePadding()) + theme.spaceLg * 2;
  ui.takeTop(static_cast<int16_t>(y - body.y));
  paragraph(ui, leechText(l.lapses), 1, false, fui::TextAlign::Center, 5);
  ui.spacer(theme.spaceMd);
  paragraph(ui, "Ela sai da fila de hoje. Quando voc\xC3\xAA a salvar de novo numa frase, volta para a revis\xC3\xA3o.", 0,
            false, fui::TextAlign::Center, 3);
}

void ReviewActivity::buildEmpty(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int side = sidePadding();
  bottomButton(ui, "Voltar aos decks", ID_DONE, true);
  const bool extra = canAddExtra();
  if (extra) {
    const fui::Rect r = ui.takeBottom(theme.rowHeight, theme.spaceMd);
    button(ui, fui::Rect{static_cast<int16_t>(r.x + side), r.y, static_cast<int16_t>(r.width - 2 * side), r.height},
           ("+" + std::to_string(EXTRA_NEW_STEP) + " novas hoje").c_str(), ID_EXTRA);
  }
  if (focusedId() < 0) setFocus(extra ? ID_EXTRA : ID_DONE);
  ui.spacer(static_cast<int16_t>(ui.body().height / 3));
  paragraph(ui, emptyText, 1, false, fui::TextAlign::Center, 4);
}

}  // namespace linguetta
