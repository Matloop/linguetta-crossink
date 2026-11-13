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

namespace {

enum : int { ID_BACK = 9000, ID_REVEAL = 1, ID_GRADE = 10, ID_DONE = 20, ID_EXTRA = 21, ID_CONTINUE = 22 };

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
  Activity::onEnter();
  Service& svc = Service::get();
  shortTitle = ref.linguetta ? languageName(ref.language) : ref.name;
  title = ref.linguetta ? svc.deckName(ref.language) : ref.name;
  if (ref.linguetta && ref.deck >= 0) {
    const BankHeader& hdr = svc.view(ref.language).header();
    if (ref.deck < static_cast<int>(hdr.decks.size())) title = shortTitle = hdr.decks[ref.deck].name;
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
  session.start(*source, title, std::move(queue), now);
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
  fullRefresh = true;
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
    fullRefresh = true;
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
  fullRefresh = session.answers() % 10 == 0;
  requestUpdate();
}

void ReviewActivity::endSession() {
  if (!historySaved && session.answers() > 0) {
    historySaved = true;
    if (!appendHistory(Service::get().fs(), session.history())) LOG_ERR("LGT", "History append failed");
  }
  mode = Mode::Summary;
  setFocus(canAddExtra() ? ID_EXTRA : ID_DONE);
  fullRefresh = true;
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
  if (id == ID_BACK) {
    onBack();
  } else if (mode == Mode::Front && id == ID_REVEAL) {
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
    fullRefresh = true;
    requestUpdate();
  } else if ((mode == Mode::Summary || mode == Mode::Empty) && id == ID_EXTRA) {
    addExtraNew();
  } else if ((mode == Mode::Summary || mode == Mode::Empty) && id == ID_DONE) {
    finish();
  }
}

void ReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  clearTargets();
  switch (mode) {
    case Mode::Front:
      renderFront();
      break;
    case Mode::Back:
      renderBack();
      break;
    case Mode::Leech:
      renderLeech();
      break;
    case Mode::Summary:
      renderSummary();
      break;
    case Mode::Empty:
      renderEmpty();
      break;
  }
  renderer.displayBuffer(fullRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  fullRefresh = false;
}

void ReviewActivity::renderFront() {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const std::string pos = remainingLabel(session.remaining());
  target(ui::header(renderer, shortTitle, pos), ID_BACK, false);

  const ui::Box button{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, w - 2 * ui::MARGIN, ui::BUTTON_H};
  // Only the word, big and centred: the sentence is context for the answer, not a hint.
  const int bodyTop = ui::headerH(), bodyBottom = button.y - 10;
  if (face.ficha) {
    int top = bodyTop;
    if (face.cloze) {
      const char* hint = "LACUNA";
      const int tw = renderer.getTextWidth(ui::fontSmall(), hint, EpdFontFamily::BOLD);
      renderer.drawText(ui::fontSmall(), (w - tw) / 2, bodyTop + 24, hint, true, EpdFontFamily::BOLD);
      top += 24 + renderer.getLineHeight(ui::fontSmall());
    }
    drawFicha(face.lemma, top, bodyBottom, 10);
  } else {
    drawWord(face.lemma, bodyTop, bodyBottom);
  }
  // The whole body reveals too (touch); the button is the focus target.
  target(ui::Box{0, bodyTop, w, bodyBottom - bodyTop}, ID_REVEAL, false);
  target(ui::button(renderer, button, "Mostrar resposta", true), ID_REVEAL);
  setFocus(ID_REVEAL);
}

int ReviewActivity::drawFicha(const std::string& text, int top, int bottom, int maxLines) {
  const int w = ui::screenW(renderer);
  const int innerW = w - 2 * ui::MARGIN - 20;
  const int font = ui::fontTitle();
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

int ReviewActivity::drawWord(const std::string& word, int top, int bottom) {
  const int w = ui::screenW(renderer);
  const int innerW = w - 2 * ui::MARGIN - 20;
  // Display font unless a single token is too wide for it.
  int font = ui::fontDisplay();
  auto lines = renderer.wrappedText(font, word.c_str(), innerW, 3, EpdFontFamily::BOLD);
  for (const auto& l : lines) {
    if (renderer.getTextWidth(font, l.c_str(), EpdFontFamily::BOLD) > innerW) {
      font = ui::fontTitle();
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

void ReviewActivity::renderBack() {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const std::string pos = remainingLabel(session.remaining());
  target(ui::header(renderer, shortTitle, pos), ID_BACK, false);
  const int innerW = w - 2 * ui::MARGIN - 20;
  const auto centred = [&](int font, int y, const std::string& text, EpdFontFamily::Style style, int maxLines) {
    for (const auto& line : renderer.wrappedText(font, text.c_str(), innerW, maxLines, style)) {
      const int tw = renderer.getTextWidth(font, line.c_str(), style);
      renderer.drawText(font, (w - tw) / 2, y, line.c_str(), true, style);
      y += renderer.getLineHeight(font);
    }
    return y;
  };

  // Grades at the bottom.
  const int gradesH = ui::BUTTON_H + 8;
  const int gradesY = h - ui::MARGIN - gradesH;
  const int statusH = status.empty() ? 0 : renderer.getLineHeight(ui::fontSmall()) + 8;

  int y;
  if (face.ficha) {
    // Front small, a rule, then the back as the answer.
    y = centred(ui::fontBody(), ui::headerH() + 30, face.lemma, EpdFontFamily::REGULAR, 4);
    y += 18;
    renderer.fillRect(w / 2 - 40, y, 80, 2, true);
    y += 22;
    drawFicha(face.sense, y, gradesY - statusH - 16, 12);
  } else {
  // Word, then the answer: the meaning is the biggest thing after the word.
  y = drawWord(face.lemma, ui::headerH() + 40, -1);
  const std::string meta = join({face.ipa, face.pos});
  if (!meta.empty()) y = centred(ui::fontIpa(), y + 4, meta, EpdFontFamily::REGULAR, 1);
  y += 22;
  renderer.fillRect(w / 2 - 40, y, 80, 2, true);
  y += 24;
  if (!face.sense.empty()) y = centred(ui::fontTitle(), y, face.sense, EpdFontFamily::REGULAR, 3);

  // Context, small and below: the sentence it was met in (3 lines at most) and the work.
  if (!face.sentence.empty()) {
    const size_t at = findCaseless(face.sentence, face.form.empty() ? face.lemma : face.form);
    const size_t len = face.form.empty() ? face.lemma.size() : face.form.size();
    const int lh = renderer.getLineHeight(ui::fontSmall());
    const int room = std::max(1, (gradesY - statusH - y - 40 - 2 * lh) / lh);
    y = ui::highlightedSentence(renderer, ui::fontSmall(), y + 32, innerW, face.sentence, at,
                                at == std::string::npos ? at : at + len, std::min(room, 3));
  }
  if (!face.work.empty()) centred(ui::fontSmall(), y + 6, face.work, EpdFontFamily::REGULAR, 1);
  }

  if (!status.empty()) {
    const int tw = renderer.getTextWidth(ui::fontSmall(), status.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(ui::fontSmall(), (w - tw) / 2, gradesY - statusH, status.c_str(), true, EpdFontFamily::BOLD);
  }
  const int gap = 8;
  const int gw = (w - 2 * ui::MARGIN - 3 * gap) / 4;
  const int lh = renderer.getLineHeight(ui::fontBody()), sh = renderer.getLineHeight(ui::fontSmall());
  for (int g = 1; g <= 4; g++) {
    const ui::Box b{ui::MARGIN + (g - 1) * (gw + gap), gradesY, gw, gradesH};
    // All four look the same; the focus (side buttons only) is an outer ring.
    renderer.drawRoundedRect(b.x, b.y, b.w, b.h, 2, 10, true);
    if (focused(ID_GRADE + g)) drawFocus(b);
    const int top = b.y + (b.h - lh - sh) / 2;
    const std::string name = renderer.truncatedText(ui::fontBody(), GRADE_NAMES[g], b.w - 8, EpdFontFamily::BOLD);
    int tw = renderer.getTextWidth(ui::fontBody(), name.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(ui::fontBody(), b.x + (b.w - tw) / 2, top, name.c_str(), true, EpdFontFamily::BOLD);
    const std::string iv = fsrs::formatInterval(intervals[g - 1]);
    // "2 meses" is wider than a grade box at 12 pt; drop to the 10 pt UI font then.
    int ivFont = ui::fontSmall();
    tw = renderer.getTextWidth(ivFont, iv.c_str());
    if (tw > b.w - 16) {
      ivFont = UI_10_FONT_ID;
      tw = renderer.getTextWidth(ivFont, iv.c_str());
    }
    renderer.drawText(ivFont, b.x + (b.w - tw) / 2, top + lh, iv.c_str(), true);
    target(b, ID_GRADE + g);
  }
}

void ReviewActivity::renderSummary() {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  target(ui::header(renderer, "Sess\xC3\xA3o conclu\xC3\xAD" "da"), ID_BACK, false);
  int y = ui::headerH() + 20;
  const std::string t = renderer.truncatedText(ui::fontTitle(), title.c_str(), w - 2 * ui::MARGIN, EpdFontFamily::BOLD);
  renderer.drawText(ui::fontTitle(), ui::MARGIN, y, t.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(ui::fontTitle()) + 2;
  const int cards = session.cards();
  const std::string sub = std::to_string(cards) + (cards == 1 ? " cart\xC3\xA3o" : " cart\xC3\xB5" "es") + " \xC2\xB7 " +
                          formatDuration(session.durationMs());
  renderer.drawText(ui::fontBody(), ui::MARGIN, y, sub.c_str(), true);
  y += renderer.getLineHeight(ui::fontBody()) + 16;

  const int errors = session.firstGrades(1);
  const int boxW = (w - 2 * ui::MARGIN - 12) / 2;
  ui::statBox(renderer, ui::Box{ui::MARGIN, y, boxW, 84}, std::to_string(cards - errors), "acertos");
  ui::statBox(renderer, ui::Box{ui::MARGIN + boxW + 12, y, boxW, 84}, std::to_string(errors), "erros");
  y += 84 + 4;

  const bool extra = canAddExtra();
  // Teimosas or "+5" need the room: the grades shrink to one line.
  const bool compact = extra || !leeches.empty();
  if (compact) {
    std::string grades;
    for (int g = 1; g <= 4; g++) {
      grades += (g > 1 ? " \xC2\xB7 " : "") + std::string(GRADE_NAMES[g]) + " " + std::to_string(session.firstGrades(g));
    }
    y += 10;
    renderer.drawText(ui::fontSmall(), ui::MARGIN, y, grades.c_str(), true);
    y += renderer.getLineHeight(ui::fontSmall()) + 4;
  } else {
    y = ui::section(renderer, y, "Por nota");
    const int labelW = renderer.getTextWidth(ui::fontBody(), "Dif\xC3\xAD" "cil") + 14;
    const int countW = renderer.getTextWidth(ui::fontBody(), "0000");
    const int barX = ui::MARGIN + labelW, barW = w - 2 * ui::MARGIN - labelW - countW - 14;
    const int lh = renderer.getLineHeight(ui::fontBody());
    for (int g = 1; g <= 4; g++) {
      const int n = session.firstGrades(g);
      renderer.drawText(ui::fontBody(), ui::MARGIN, y, GRADE_NAMES[g], true);
      const int barY = y + (lh - 16) / 2;
      renderer.drawRect(barX, barY, barW, 16, 2, true);
      if (cards > 0 && n > 0) renderer.fillRect(barX, barY, std::max(4, barW * n / cards), 16, true);
      const std::string c = std::to_string(n);
      renderer.drawText(ui::fontBody(), w - ui::MARGIN - renderer.getTextWidth(ui::fontBody(), c.c_str()), y, c.c_str(),
                        true);
      y += lh + 12;
    }
    y += 8;
  }
  std::string note;
  if (ref.linguetta) {
    const int n = session.answers();
    note = std::to_string(n) + (n == 1 ? " revis\xC3\xA3o na fila de envio. Ela sobe" : " revis\xC3\xB5" "es na fila de envio. Elas sobem") +
           " no pr\xC3\xB3ximo sync.";
  } else {
    note = "Deck local: o progresso fica s\xC3\xB3 neste aparelho.";
  }
  y = ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y, w - 2 * ui::MARGIN, note, compact ? 2 : 3);

  const int lineH = renderer.getLineHeight(ui::fontSmall());
  const int buttonsTop = h - ui::MARGIN - ui::BUTTON_H - (extra ? ui::BUTTON_H + 12 + lineH + 8 : 0);
  if (!leeches.empty()) {
    // One line per teimosa ("ostinato · errou 8 vezes"), then what happens to them.
    y = ui::section(renderer, y, leeches.size() == 1 ? "Teimosa" : "Teimosas");
    const int bodyH = renderer.getLineHeight(ui::fontBody());
    for (size_t k = 0; k < leeches.size(); k++) {
      const Leech& l = leeches[k];
      if (y + bodyH * 2 + lineH * 2 > buttonsTop - 4 && k + 1 < leeches.size()) {
        const std::string more = "e mais " + std::to_string(leeches.size() - k);
        renderer.drawText(ui::fontBody(), ui::MARGIN, y, more.c_str(), true);
        y += bodyH;
        break;
      }
      const std::string line = l.word + " \xC2\xB7 errou " + std::to_string(l.lapses) + (l.lapses == 1 ? " vez" : " vezes");
      const std::string t = renderer.truncatedText(ui::fontBody(), line.c_str(), w - 2 * ui::MARGIN, EpdFontFamily::BOLD);
      renderer.drawText(ui::fontBody(), ui::MARGIN, y, t.c_str(), true, EpdFontFamily::BOLD);
      y += bodyH;
    }
    ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y + 2, w - 2 * ui::MARGIN,
                  leeches.size() == 1 ? "Vai esperar at\xC3\xA9 voc\xC3\xAA encontr\xC3\xA1-la num livro ou s\xC3\xA9rie."
                                      : "V\xC3\xA3o esperar at\xC3\xA9 voc\xC3\xAA encontr\xC3\xA1-las num livro ou s\xC3\xA9rie.",
                  2);
  }
  if (extra) {
    const char* done = "As novas de hoje acabaram.";
    renderer.drawText(ui::fontSmall(), ui::MARGIN, buttonsTop, done, true, EpdFontFamily::BOLD);
  }

  ui::Box b{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, w - 2 * ui::MARGIN, ui::BUTTON_H};
  if (extra) {
    const ui::Box e{b.x, b.y - ui::BUTTON_H - 12, b.w, ui::BUTTON_H};
    target(ui::button(renderer, e, "+" + std::to_string(EXTRA_NEW_STEP) + " novas hoje", false), ID_EXTRA);
    if (focused(ID_EXTRA)) drawFocus(e);
  }
  target(ui::button(renderer, b, "Voltar aos decks", true), ID_DONE);
}

void ReviewActivity::renderLeech() {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  target(ui::header(renderer, shortTitle, remainingLabel(session.remaining())), ID_BACK, false);
  const ui::Box button{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, w - 2 * ui::MARGIN, ui::BUTTON_H};
  const Leech& l = leeches.back();
  int y = drawWord(l.word, ui::headerH() + 80, -1) + 36;
  // The notice in a bordered box.
  const int boxX = ui::MARGIN, boxW = w - 2 * ui::MARGIN;
  const int pad = 18;
  const int lh = renderer.getLineHeight(ui::fontBody());
  const auto lines = renderer.wrappedText(ui::fontBody(), leechText(l.lapses).c_str(), boxW - 2 * pad, 5);
  const int boxH = static_cast<int>(lines.size()) * lh + 2 * pad;
  renderer.drawRoundedRect(boxX, y, boxW, boxH, 3, 10, true);
  int ty = y + pad;
  for (const auto& line : lines) {
    renderer.drawText(ui::fontBody(), boxX + pad, ty, line.c_str(), true);
    ty += lh;
  }
  y += boxH + 18;
  ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y, w - 2 * ui::MARGIN,
                "Ela sai da fila de hoje. Quando voc\xC3\xAA a salvar de novo numa frase, volta para a revis\xC3\xA3o.", 3);
  target(ui::button(renderer, button, "Continuar", true), ID_CONTINUE);
}

void ReviewActivity::renderEmpty() {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  target(ui::header(renderer, title), ID_BACK, false);
  ui::message(renderer, h / 2 - 60, emptyText);
  const ui::Box b{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, w - 2 * ui::MARGIN, ui::BUTTON_H};
  if (canAddExtra()) {
    const ui::Box e{b.x, b.y - ui::BUTTON_H - 12, b.w, ui::BUTTON_H};
    target(ui::button(renderer, e, "+" + std::to_string(EXTRA_NEW_STEP) + " novas hoje", false), ID_EXTRA);
    if (focused(ID_EXTRA)) drawFocus(e);
  }
  target(ui::button(renderer, b, "Voltar aos decks", true), ID_DONE);
  if (!focused(ID_EXTRA)) setFocus(ID_DONE);
}

}  // namespace linguetta
