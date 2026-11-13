#include "SyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <HalClock.h>
#include <WiFi.h>

#include <ctime>

#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "linguetta/core/History.h"
#include "linguetta/core/LocalView.h"
#include "linguetta/core/Text.h"
#include "linguetta/device/Linguetta.h"
#include "linguetta/device/SyncHttp.h"
#include "util/QrUtils.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif

namespace linguetta {

namespace {

enum : int { ID_BACK = 9000, ID_PRIMARY = 9001, ID_SECONDARY = 9002 };

constexpr uint32_t NTP_TIMEOUT_MS = 5000;
constexpr unsigned long REDRAW_MS = 2000;  // progress redraws during the sync

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// "a, b e c"
std::string joinPt(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) out += i + 1 == parts.size() ? " e " : ", ";
    out += parts[i];
  }
  return out;
}

// "https://host/parear" -> "host/parear"
std::string displayUri(const std::string& uri) {
  const size_t p = uri.find("://");
  return p == std::string::npos ? uri : uri.substr(p + 3);
}

}  // namespace

SyncActivity::SyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : ui::Screen("LinguettaSync", renderer, mappedInput) {}

SyncActivity::~SyncActivity() = default;

int64_t SyncActivity::clockMs() const {
  const int64_t trusted = Service::get().nowMs();
  return trusted > 0 ? trusted : static_cast<int64_t>(time(nullptr)) * 1000;
}

void SyncActivity::onEnter() {
  Activity::onEnter();
  loadQueue();
  startPending = true;  // blocking work starts from the loop, once this screen is current
  requestUpdate();
}

void SyncActivity::onExit() {
  Activity::onExit();
  http.reset();
  // Upstream reboots after a Wi-Fi session to defragment the heap; the X4 Pro has PSRAM and
  // a reboot would leave Flashcards, so the radio is just switched off (LINGUETTA.md).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
}

void SyncActivity::connect() {
  if (WiFi.status() != WL_CONNECTED && WIFI_STORE.getCredentialCount() == 0) {
    // No network saved yet: say why before the (upstream) network list shows up.
    {
      RenderLock lock(*this);
      phase = Phase::NeedWifi;
      setFocus(ID_PRIMARY);
    }
    requestUpdate();
    return;
  }
  openWifi();
}

void SyncActivity::openWifi() {
  {
    RenderLock lock(*this);
    phase = Phase::Wifi;
  }
  requestUpdate();
  if (WiFi.status() == WL_CONNECTED) {
    afterWifi(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& r) {
                           firstRender = true;
                           afterWifi(!r.isCancelled);
                         });
}

void SyncActivity::afterWifi(bool connected) {
  if (!connected) {
    SyncError e;
    e.kind = SyncErr::Network;
    e.code = "network";
    fail(e, true);
    return;
  }
  requestUpdateAndWait();  // "Conectando…" while NTP runs
  if (!halClock.syncFromNTP()) LOG_INF("LGT", "NTP failed; using the RTC");
  if (!http) http = std::make_unique<SyncHttp>();
  if (Service::get().state().paired()) {
    doSync();
  } else {
    startPairing();
  }
}

// ---- pairing ----

void SyncActivity::startPairing() {
  Service& svc = Service::get();
  PairStart ps;
  const SyncError e = pairStart(*http, svc.state().server, svc.state().deviceName, "linguetta-x4/" CROSSPOINT_VERSION, ps);
  if (!e.ok()) {
    fail(e);
    return;
  }
  {
    RenderLock lock(*this);
    code = ps;
    interval = std::max(1, ps.interval);
    expiresAt = millis() + static_cast<unsigned long>(std::max(1, ps.expiresIn)) * 1000UL;
    nextPollAt = millis() + static_cast<unsigned long>(interval) * 1000UL;
    phase = Phase::Pairing;
    setFocus(ID_SECONDARY);
  }
  firstRender = true;
  requestUpdate();
}

void SyncActivity::onTick() {
  if (startPending) {
    startPending = false;
    connect();
    return;
  }
  if (phase != Phase::Pairing) return;
  const unsigned long now = millis();
  if (static_cast<long>(now - expiresAt) >= 0) {
    {
      RenderLock lock(*this);
      phase = Phase::PairEnded;
      pairEndTitle = "C\xC3\xB3" "digo expirado";
      pairEndText = "O c\xC3\xB3" "digo valia por 10 minutos.";
      setFocus(ID_PRIMARY);
    }
    requestUpdate();
    return;
  }
  if (static_cast<long>(now - nextPollAt) >= 0) poll();
}

void SyncActivity::poll() {
  Service& svc = Service::get();
  PairPoll p;
  const SyncError e = pairPoll(*http, svc.state().server, code.deviceCode, p);
  if (!e.ok()) {
    // Transient problems keep the code on screen; the rest end the pairing.
    if (e.kind == SyncErr::Protocol || e.kind == SyncErr::BadRequest || e.kind == SyncErr::Unauthorized) {
      fail(e);
      return;
    }
    if (e.kind == SyncErr::RateLimited && e.retryAtMs > 0) {
      const int64_t wait = e.retryAtMs - clockMs();
      if (wait > 0) interval = std::max(interval, static_cast<int>(wait / 1000));
    }
    nextPollAt = millis() + static_cast<unsigned long>(interval) * 1000UL;
    requestUpdate();
    return;
  }
  switch (p.status) {
    case PairStatus::Pending:
      if (p.interval > 0) interval = p.interval;
      break;
    case PairStatus::SlowDown:
      interval = p.interval > 0 ? p.interval : interval + 5;
      break;
    case PairStatus::Denied:
    case PairStatus::Expired: {
      RenderLock lock(*this);
      phase = Phase::PairEnded;
      if (p.status == PairStatus::Denied) {
        pairEndTitle = "Recusado";
        pairEndText = "O pedido foi recusado na web.";
      } else {
        pairEndTitle = "C\xC3\xB3" "digo expirado";
        pairEndText = "O c\xC3\xB3" "digo valia por 10 minutos.";
      }
      setFocus(ID_PRIMARY);
      break;
    }
    case PairStatus::Approved:
      approved = p;
      if (isOtherAccount(svc.state(), p)) {
        {
          RenderLock lock(*this);
          phase = Phase::OtherAccount;
          setFocus(ID_SECONDARY);
        }
        firstRender = true;
        requestUpdate();
      } else {
        acceptPairing(false);
      }
      return;
  }
  nextPollAt = millis() + static_cast<unsigned long>(interval) * 1000UL;
  requestUpdate();
}

void SyncActivity::acceptPairing(bool discardLocal) {
  Service& svc = Service::get();
  if (!applyPairing(svc.fs(), svc.state(), approved, discardLocal)) {
    SyncError e;
    e.kind = SyncErr::Disk;
    e.code = "storage";
    fail(e);
    return;
  }
  approved = PairPoll{};  // the token now lives in state.json only
  svc.invalidateViews();
  loadQueue();
  doSync();
}

// ---- sync ----

void SyncActivity::doSync() {
  Service& svc = Service::get();
  {
    RenderLock lock(*this);
    phase = Phase::Syncing;
    pushDone = pushTotal = recvDone = recvTotal = 0;
    resultText.clear();
  }
  firstRender = true;
  requestUpdateAndWait();
  lastDraw = millis();

  SyncOptions opt;
  opt.nowMs = clockMs();
  // CSV/TSV decks of /flashcards go up as deckImport ops (once per file).
  opt.newId = [] { return Service::get().newId(); };
  SyncProgress::Phase lastPhase = SyncProgress::Push;
  opt.progress = [&](const SyncProgress& p) {
    {
      RenderLock lock(*this);
      if (p.phase == SyncProgress::Push) {
        pushDone = p.done;
        pushTotal = p.total;
      } else {
        pushDone = pushTotal;
        recvDone = p.done;
        recvTotal = p.total;
      }
    }
    if (p.phase != lastPhase || millis() - lastDraw >= REDRAW_MS) {
      lastPhase = p.phase;
      lastDraw = millis();
      requestUpdate();
    }
  };

  // The views still hold the local prediction of every card: compare after the sync.
  const std::vector<std::string> langs = svc.languages();
  for (const auto& l : langs) svc.view(l);
  SyncStats stats;
  const SyncError e = linguetta::runSync(svc.fs(), *http, svc.state(), opt, stats);
  int adjusted = 0;
  if (e.ok()) {
    for (const auto& l : langs) adjusted += countAdjusted(svc.view(l), stats.serverDue);
  }
  svc.invalidateViews();
  if (!e.ok()) {
    LOG_ERR("LGT", "sync failed: %s (%d)", e.code.c_str(), e.status);
    fail(e);
    return;
  }
  LOG_INF("LGT", "sync ok: pushed %d, %s %d lines, bank %d", stats.pushed, stats.mode.c_str(), stats.received,
          stats.bankItems);

  std::string text;
  if (stats.reviewsResolved > 0) {
    text += "O servidor refez " + plural(stats.reviewsResolved, "revis\xC3\xA3o", "revis\xC3\xB5" "es") + ".\n";
  }
  if (adjusted > 0) {
    text += adjusted == 1 ? "1 cart\xC3\xA3o teve o agendamento ajustado.\n"
                          : std::to_string(adjusted) + " cart\xC3\xB5" "es tiveram o agendamento ajustado.\n";
  }
  if (stats.removed > 0) {
    text += stats.removed == 1 ? "1 item apagado na web saiu daqui.\n"
                               : std::to_string(stats.removed) + " itens apagados na web sa\xC3\xADram daqui.\n";
  }
  if (stats.rejected > 0) {
    text += plural(stats.rejected, "opera\xC3\xA7\xC3\xA3o recusada", "opera\xC3\xA7\xC3\xB5" "es recusadas") +
            " pelo servidor (.linguetta/rejected.jsonl).\n";
  }
  if (stats.decksImported > 0) {
    text += stats.decksImported == 1 ? "1 deck do cart\xC3\xA3o SD subiu para a conta.\n"
                                     : std::to_string(stats.decksImported) + " decks do cart\xC3\xA3o SD subiram para a conta.\n";
  }
  if (stats.extraNew > 0) {
    text += "Novas extras de hoje enviadas.\n";
  }
  text += "Banco: " + plural(stats.bankItems, "item", "itens");
  if (stats.bankFichas > 0) text += " e " + plural(stats.bankFichas, "ficha", "fichas");
  if (stats.itemsByLanguage.size() > 1) {
    std::string parts;
    for (const auto& kv : stats.itemsByLanguage) {
      parts += (parts.empty() ? " (" : ", ") + languageName(kv.first) + " " + std::to_string(kv.second);
    }
    text += parts + ")";
  }
  text += ".";

  loadQueue();
  {
    RenderLock lock(*this);
    pushDone = pushTotal;
    recvDone = recvTotal = std::max(recvTotal, stats.received);
    if (recvTotal == 0) recvDone = recvTotal = 1;
    resultText = std::move(text);
    phase = Phase::Done;
    setFocus(ID_PRIMARY);
  }
  requestUpdate();
}

void SyncActivity::fail(const SyncError& e, bool offline) {
  loadQueue();
  {
    RenderLock lock(*this);
    error = e;
    noConnection = offline || e.kind == SyncErr::Network;
    phase = Phase::Offline;
    setFocus(ID_PRIMARY);
  }
  firstRender = true;
  requestUpdate();
}

void SyncActivity::loadQueue() {
  Service& svc = Service::get();
  const OutboxCounts c = countOutbox(svc.fs());
  int items = 0;
  for (const auto& l : svc.languages()) {
    const LocalView& v = svc.view(l);
    for (size_t i = 0; i < v.size(); i++) {
      if (v.entry(i).bankOffset < 0) items++;
    }
  }
  RenderLock lock(*this);
  queueItems = items;
  queueSaves = c.saves;
  queueReviews = c.reviews;
  queueSeen = c.seen;
}

// ---- input ----

void SyncActivity::onAction(int id) {
  if (id == ID_BACK || id == ID_SECONDARY) {
    finish();
    return;
  }
  if (id != ID_PRIMARY) return;
  switch (phase) {
    case Phase::PairEnded:
      startPairing();
      break;
    case Phase::OtherAccount:
      acceptPairing(true);
      break;
    case Phase::Done:
      finish();
      break;
    case Phase::Offline:
      connect();
      break;
    case Phase::NeedWifi:
      openWifi();
      break;
    default:
      break;
  }
}

// ---- drawing ----

void SyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  clearTargets();
  const bool pairing = phase == Phase::Pairing || phase == Phase::PairEnded || phase == Phase::OtherAccount;
  target(ui::header(renderer, pairing ? "Conectar ao Linguetta" : "Sincronizar"), ID_BACK, false);
  const int y = ui::headerH() + 18;
  switch (phase) {
    case Phase::NeedWifi:
      renderNeedWifi(y);
      break;
    case Phase::Wifi:
      ui::message(renderer, ui::screenH(renderer) / 2 - 40, "Conectando\xE2\x80\xA6");
      break;
    case Phase::Pairing:
    case Phase::PairEnded:
    case Phase::OtherAccount:
      renderPairing(y);
      break;
    case Phase::Syncing:
    case Phase::Done:
      renderSyncing(y);
      break;
    case Phase::Offline:
      renderOffline(y);
      break;
  }
  renderer.displayBuffer(firstRender ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}

void SyncActivity::renderPairing(int y) {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const int contentW = w - 2 * ui::MARGIN;
  const ui::Box bottom{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, contentW, ui::BUTTON_H};

  if (phase == Phase::Pairing) {
    renderer.drawCenteredText(ui::fontLarge(), y, "Escaneie com o celular");
    y += renderer.getLineHeight(ui::fontLarge()) + 12;
    const int qr = 250;
    QrUtils::drawQrCode(renderer, Rect((w - qr) / 2, y, qr, qr), code.verificationUriComplete);
    y += qr + 16;
    const std::string open = "ou abra " + displayUri(code.verificationUri) + " e digite";
    const std::string line = renderer.truncatedText(ui::fontSmall(), open.c_str(), contentW);
    renderer.drawCenteredText(ui::fontSmall(), y, line.c_str());
    y += renderer.getLineHeight(ui::fontSmall()) + 8;
    renderer.drawCenteredText(ui::fontTitle(), y, code.userCode.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(ui::fontTitle()) + 10;
    const unsigned long now = millis();
    const long left = static_cast<long>(expiresAt - now) > 0 ? static_cast<long>(expiresAt - now) / 1000 : 0;
    char buf[64];
    snprintf(buf, sizeof buf, "Aguardando aprova\xC3\xA7\xC3\xA3o \xC2\xB7 expira em %ld:%02ld", left / 60, left % 60);
    renderer.drawCenteredText(ui::fontSmall(), y, buf);
    const ui::Box b = target(ui::button(renderer, bottom, "Cancelar", false), ID_SECONDARY);
    if (focused(ID_SECONDARY)) drawFocus(b);
    return;
  }

  std::string title, text, primary;
  if (phase == Phase::PairEnded) {
    title = pairEndTitle;
    text = pairEndText;
    primary = "Gerar outro c\xC3\xB3" "digo";
  } else {
    const SyncState& st = Service::get().state();
    const int queued = queueSaves + queueReviews + queueSeen;
    title = "Outra conta";
    text = "Este cart\xC3\xA3o tem dados de " + (st.account.email.empty() ? std::string("outra conta") : st.account.email) +
           ". Para usar " + approved.account.email + ", o banco e a fila deste cart\xC3\xA3o (" +
           plural(queued, "opera\xC3\xA7\xC3\xA3o", "opera\xC3\xA7\xC3\xB5" "es") + ") ser\xC3\xA3o apagados.";
    primary = "Apagar e continuar";
  }
  renderer.drawText(ui::fontTitle(), ui::MARGIN, y, title.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(ui::fontTitle()) + 8;
  ui::paragraph(renderer, ui::fontBody(), ui::MARGIN, y, contentW, text, 8);
  const ui::Box second = target(ui::button(renderer, bottom, "Cancelar", false), ID_SECONDARY);
  ui::Box first = bottom;
  first.y -= ui::BUTTON_H + 12;
  first = target(ui::button(renderer, first, primary, true), ID_PRIMARY);
  if (focused(ID_PRIMARY)) drawFocus(first);
  if (focused(ID_SECONDARY)) drawFocus(second);
}

void SyncActivity::renderSyncing(int y) {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const int contentW = w - 2 * ui::MARGIN;
  const int arrow = 20, textX = ui::MARGIN + arrow + 10;
  renderer.drawText(ui::fontTitle(), ui::MARGIN, y,
                    phase == Phase::Done ? "Sincronizado" : "Sincronizando\xE2\x80\xA6", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(ui::fontTitle()) + 18;

  std::vector<std::string> parts;
  if (queueItems > 0) parts.push_back(plural(queueItems, "item", "itens"));
  if (queueSaves > 0) parts.push_back(plural(queueSaves, "encontro", "encontros"));
  if (queueReviews > 0) parts.push_back(plural(queueReviews, "revis\xC3\xA3o", "revis\xC3\xB5" "es"));
  if (queueSeen > 0) parts.push_back(plural(queueSeen, "visto", "vistos"));
  const std::string up = phase == Phase::Done ? (pushTotal > 0 ? "Enviado" : "Nada para enviar")
                         : parts.empty()      ? "Nada para enviar"
                                              : "Enviando " + joinPt(parts);
  ui::vertArrow(renderer, ui::MARGIN, y + 2, arrow, true);
  y = ui::paragraph(renderer, ui::fontBody(), textX, y, contentW - arrow - 10, up, 2) + 8;
  ui::progressBar(renderer, ui::Box{ui::MARGIN, y, contentW, 16}, pushDone, pushTotal);
  y += 16 + 22;

  std::string down = "Recebendo o banco";
  if (recvTotal > 1 || recvDone > 1) down += " (" + plural(std::max(recvTotal, recvDone), "item", "itens") + ")";
  ui::vertArrow(renderer, ui::MARGIN, y + 2, arrow, false);
  y = ui::paragraph(renderer, ui::fontBody(), textX, y, contentW - arrow - 10, down, 2) + 8;
  ui::progressBar(renderer, ui::Box{ui::MARGIN, y, contentW, 16}, recvDone, recvTotal);
  y += 16 + 12;

  if (phase != Phase::Done) return;
  y = ui::section(renderer, y, "Resultado");
  size_t start = 0;
  while (start < resultText.size()) {
    size_t nl = resultText.find('\n', start);
    if (nl == std::string::npos) nl = resultText.size();
    y = ui::paragraph(renderer, ui::fontBody(), ui::MARGIN, y, contentW, resultText.substr(start, nl - start), 3) + 2;
    start = nl + 1;
  }
  const ui::Box b = target(
      ui::button(renderer, ui::Box{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, contentW, ui::BUTTON_H}, "Concluir", true),
      ID_PRIMARY);
  if (focused(ID_PRIMARY)) drawFocus(b);
}

void SyncActivity::renderNeedWifi(int y) {
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const int contentW = w - 2 * ui::MARGIN;
  renderer.drawText(ui::fontTitle(), ui::MARGIN, y, "Conectar ao Wi-Fi", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(ui::fontTitle()) + 8;
  y = ui::paragraph(renderer, ui::fontBody(), ui::MARGIN, y, contentW,
                    "Para sincronizar, o Linguetta precisa de uma rede Wi-Fi. Escolha a rede e digite a senha "
                    "uma vez: nas pr\xC3\xB3ximas, ele conecta sozinho.",
                    5) +
      12;
  const int pending = queueSaves + queueReviews;  // a save already carries its new item
  if (pending > 0) {
    const std::string n = std::to_string(pending) +
                          (pending == 1 ? " altera\xC3\xA7\xC3\xA3o espera" : " altera\xC3\xA7\xC3\xB5" "es esperam") +
                          " no cart\xC3\xA3o pelo pr\xC3\xB3ximo sync.";
    ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y, contentW, n, 2);
  }
  const ui::Box second = target(
      ui::button(renderer, ui::Box{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, contentW, ui::BUTTON_H}, "Agora n\xC3\xA3o",
                 false),
      ID_SECONDARY);
  const ui::Box first = target(ui::button(renderer,
                                          ui::Box{ui::MARGIN, second.y - 12 - ui::BUTTON_H, contentW, ui::BUTTON_H},
                                          "Escolher rede", true),
                               ID_PRIMARY);
  if (focused(ID_PRIMARY)) drawFocus(first);
  if (focused(ID_SECONDARY)) drawFocus(second);
}

void SyncActivity::renderOffline(int y) {
  Service& svc = Service::get();
  const int w = ui::screenW(renderer), h = ui::screenH(renderer);
  const int contentW = w - 2 * ui::MARGIN;
  const bool repair = error.kind == SyncErr::Unauthorized || error.kind == SyncErr::NotPaired;

  renderer.drawText(ui::fontTitle(), ui::MARGIN, y, noConnection ? "Sem conex\xC3\xA3o" : "N\xC3\xA3o sincronizou", true,
                    EpdFontFamily::BOLD);
  y += renderer.getLineHeight(ui::fontTitle()) + 8;
  if (!noConnection) {
    y = ui::paragraph(renderer, ui::fontBody(), ui::MARGIN, y, contentW, errorText(error), 3, true,
                      EpdFontFamily::BOLD) + 6;
  }
  y = ui::paragraph(renderer, ui::fontBody(), ui::MARGIN, y, contentW,
                    "Tudo continua funcionando. Isto fica guardado no cart\xC3\xA3o at\xC3\xA9 o pr\xC3\xB3ximo sync:", 3) +
      4;
  y = ui::section(renderer, y, "Na fila");

  const int lineH = renderer.getLineHeight(ui::fontBody()) + 16;
  auto countRow = [&](const char* label, int n) {
    renderer.drawText(ui::fontBody(), ui::MARGIN, y + 8, label, true);
    const std::string v = std::to_string(n);
    const int vw = renderer.getTextWidth(ui::fontBody(), v.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(ui::fontBody(), w - ui::MARGIN - vw, y + 8, v.c_str(), true, EpdFontFamily::BOLD);
    y += lineH;
    renderer.drawLine(ui::MARGIN, y - 1, w - ui::MARGIN, y - 1, true);
  };
  countRow("Itens novos", queueItems);
  countRow("Encontros", queueSaves);
  countRow("Revis\xC3\xB5" "es", queueReviews);
  if (queueSeen > 0) countRow("Vistos", queueSeen);
  y += 10;

  std::string last;
  int64_t lastMs = 0;
  if (parseIso(svc.state().lastSyncAt.c_str(), lastMs)) {
    const int64_t now = svc.nowMs();
    std::string label = sessionDayLabel(lastMs, now, svc.utcOffsetMs(lastMs));  // "Hoje · 08:12"
    if (label.rfind("Hoje", 0) == 0) label[0] = 'h';
    if (label.rfind("Ontem", 0) == 0) label[0] = 'o';
    const size_t dot = label.find(" \xC2\xB7 ");
    if (dot != std::string::npos) label.replace(dot, 4, ", ");
    last = "\xC3\x9Altimo sync: " + label;
  } else {
    last = "\xC3\x9Altimo sync: nunca";
  }
  ui::paragraph(renderer, ui::fontSmall(), ui::MARGIN, y, contentW, last, 1);

  const ui::Box b = target(
      ui::button(renderer, ui::Box{ui::MARGIN, h - ui::MARGIN - ui::BUTTON_H, contentW, ui::BUTTON_H},
                 repair ? "Parear de novo" : "Tentar de novo", true),
      ID_PRIMARY);
  if (focused(ID_PRIMARY)) drawFocus(b);
}

}  // namespace linguetta
