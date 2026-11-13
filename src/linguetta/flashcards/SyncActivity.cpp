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

namespace fui = freeink::ui;

namespace {

enum : int { ID_PRIMARY = 1, ID_SECONDARY = 2 };

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
  Screen::onEnter();
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
                           requestFullRefresh();
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
  requestFullRefresh();
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
        requestFullRefresh();
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
  requestFullRefresh();
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
  requestFullRefresh();
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
  if (id == ID_SECONDARY) {
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

std::string SyncActivity::title() const {
  const bool pairing = phase == Phase::Pairing || phase == Phase::PairEnded || phase == Phase::OtherAccount;
  return pairing ? "Conectar ao Linguetta" : "Sincronizar";
}

void SyncActivity::build(ui::Ui& ui) {
  switch (phase) {
    case Phase::NeedWifi:
      buildNeedWifi(ui);
      break;
    case Phase::Wifi:
      ui.centeredText("Conectando\xE2\x80\xA6", ui.theme().bodyText);
      break;
    case Phase::Pairing:
    case Phase::PairEnded:
    case Phase::OtherAccount:
      buildPairing(ui);
      break;
    case Phase::Syncing:
    case Phase::Done:
      buildSyncing(ui);
      break;
    case Phase::Offline:
      buildOffline(ui);
      break;
  }
}

void SyncActivity::buildPairing(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int w = renderer.getScreenWidth();
  const int side = sidePadding();

  if (phase == Phase::Pairing) {
    bottomButton(ui, "Cancelar", ID_SECONDARY);
    if (focusedId() < 0) setFocus(ID_SECONDARY);
    paragraph(ui, "Escaneie com o celular", 1, true, fui::TextAlign::Center);
    ui.spacer(theme.spaceMd);
    // The QR code takes what the lines below leave, up to 250 px.
    const int below = renderer.getLineHeight(fontFor(0)) * 2 + renderer.getLineHeight(fontFor(2)) + theme.spaceMd * 3;
    const int qr = std::max(120, std::min(250, static_cast<int>(ui.body().height) - below));
    const fui::Rect qrBand = ui.takeTop(static_cast<int16_t>(qr), theme.spaceMd);
    QrUtils::drawQrCode(renderer, Rect((w - qr) / 2, qrBand.y, qr, qr), code.verificationUriComplete);
    paragraph(ui, "ou abra " + displayUri(code.verificationUri) + " e digite", 0, false, fui::TextAlign::Center);
    const fui::Rect codeRow = ui.takeTop(static_cast<int16_t>(renderer.getLineHeight(fontFor(2))), theme.spaceSm);
    ui.target().text(codeRow, code.userCode.c_str(), style(ui, 2, true, fui::TextAlign::Center));
    const unsigned long now = millis();
    const long left = static_cast<long>(expiresAt - now) > 0 ? static_cast<long>(expiresAt - now) / 1000 : 0;
    char buf[64];
    snprintf(buf, sizeof buf, "Aguardando aprova\xC3\xA7\xC3\xA3o \xC2\xB7 expira em %ld:%02ld", left / 60, left % 60);
    paragraph(ui, buf, 0, false, fui::TextAlign::Center);
    return;
  }

  std::string heading, text, primary;
  if (phase == Phase::PairEnded) {
    heading = pairEndTitle;
    text = pairEndText;
    primary = "Gerar outro c\xC3\xB3" "digo";
  } else {
    const SyncState& st = Service::get().state();
    const int queued = queueSaves + queueReviews + queueSeen;
    heading = "Outra conta";
    text = "Este cart\xC3\xA3o tem dados de " + (st.account.email.empty() ? std::string("outra conta") : st.account.email) +
           ". Para usar " + approved.account.email + ", o banco e a fila deste cart\xC3\xA3o (" +
           plural(queued, "opera\xC3\xA7\xC3\xA3o", "opera\xC3\xA7\xC3\xB5" "es") + ") ser\xC3\xA3o apagados.";
    primary = "Apagar e continuar";
  }
  bottomButton(ui, "Cancelar", ID_SECONDARY);
  const fui::Rect r = ui.takeBottom(theme.rowHeight, theme.spaceMd);
  button(ui, fui::Rect{static_cast<int16_t>(r.x + side), r.y, static_cast<int16_t>(r.width - 2 * side), r.height},
         primary.c_str(), ID_PRIMARY, true);
  if (focusedId() < 0) setFocus(ID_PRIMARY);
  paragraph(ui, heading, 2, true);
  ui.spacer(theme.spaceSm);
  paragraph(ui, text, 1, false, fui::TextAlign::Left, 8);
}

void SyncActivity::buildSyncing(ui::Ui& ui) {
  const auto& theme = ui.theme();
  const int side = sidePadding();
  if (phase == Phase::Done) {
    bottomButton(ui, "Concluir", ID_PRIMARY, true);
    if (focusedId() < 0) setFocus(ID_PRIMARY);
  }
  paragraph(ui, phase == Phase::Done ? "Sincronizado" : "Sincronizando\xE2\x80\xA6", 2, true);
  ui.spacer(theme.spaceMd);

  std::vector<std::string> parts;
  if (queueItems > 0) parts.push_back(plural(queueItems, "item", "itens"));
  if (queueSaves > 0) parts.push_back(plural(queueSaves, "encontro", "encontros"));
  if (queueReviews > 0) parts.push_back(plural(queueReviews, "revis\xC3\xA3o", "revis\xC3\xB5" "es"));
  if (queueSeen > 0) parts.push_back(plural(queueSeen, "visto", "vistos"));
  const std::string up = phase == Phase::Done ? (pushTotal > 0 ? "Enviado" : "Nada para enviar")
                         : parts.empty()      ? "Nada para enviar"
                                              : "Enviando " + joinPt(parts);
  std::string down = "Recebendo o banco";
  if (recvTotal > 1 || recvDone > 1) down += " (" + plural(std::max(recvTotal, recvDone), "item", "itens") + ")";

  const auto progress = [&](const std::string& label, int done, int total) {
    paragraph(ui, label, 1, false, fui::TextAlign::Left, 2);
    const int barH = std::max(8, theme.rowHeight / 3);
    const fui::Rect bar = ui.takeTop(static_cast<int16_t>(barH), theme.spaceLg);
    fui::ProgressBarProps props;
    props.value = done;
    props.max = std::max(1, total);
    props.border = fui::Paint::solid(fui::Color::Black);
    props.borderWidth = 1;
    fui::progressBar(ui.frame(),
                     fui::Rect{static_cast<int16_t>(bar.x + side), bar.y, static_cast<int16_t>(bar.width - 2 * side), bar.height},
                     props);
  };
  progress(up, pushDone, pushTotal);
  progress(down, recvDone, recvTotal);

  if (phase != Phase::Done) return;
  paragraph(ui, "Resultado", 1, true);
  size_t start = 0;
  while (start < resultText.size()) {
    size_t nl = resultText.find('\n', start);
    if (nl == std::string::npos) nl = resultText.size();
    paragraph(ui, resultText.substr(start, nl - start), 1, false, fui::TextAlign::Left, 3);
    start = nl + 1;
  }
}

void SyncActivity::buildNeedWifi(ui::Ui& ui) {
  const auto& theme = ui.theme();
  bottomButton(ui, "Agora n\xC3\xA3o", ID_SECONDARY);
  const int side = sidePadding();
  const fui::Rect r = ui.takeBottom(theme.rowHeight, theme.spaceMd);
  button(ui, fui::Rect{static_cast<int16_t>(r.x + side), r.y, static_cast<int16_t>(r.width - 2 * side), r.height},
         "Escolher rede", ID_PRIMARY, true);
  if (focusedId() < 0) setFocus(ID_PRIMARY);
  paragraph(ui, "Conectar ao Wi-Fi", 2, true);
  ui.spacer(theme.spaceSm);
  paragraph(ui,
            "Para sincronizar, o Linguetta precisa de uma rede Wi-Fi. Escolha a rede e digite a senha "
            "uma vez: nas pr\xC3\xB3ximas, ele conecta sozinho.",
            1, false, fui::TextAlign::Left, 6);
  const int pending = queueSaves + queueReviews;  // a save already carries its new item
  if (pending > 0) {
    ui.spacer(theme.spaceMd);
    paragraph(ui,
              std::to_string(pending) + (pending == 1 ? " altera\xC3\xA7\xC3\xA3o espera" : " altera\xC3\xA7\xC3\xB5" "es esperam") +
                  " no cart\xC3\xA3o pelo pr\xC3\xB3ximo sync.",
              0, false, fui::TextAlign::Left, 2);
  }
}

void SyncActivity::buildOffline(ui::Ui& ui) {
  Service& svc = Service::get();
  const auto& theme = ui.theme();
  const bool repair = error.kind == SyncErr::Unauthorized || error.kind == SyncErr::NotPaired;
  bottomButton(ui, repair ? "Parear de novo" : "Tentar de novo", ID_PRIMARY, true);
  if (focusedId() < 0) setFocus(ID_PRIMARY);

  paragraph(ui, noConnection ? "Sem conex\xC3\xA3o" : "N\xC3\xA3o sincronizou", 2, true);
  ui.spacer(theme.spaceSm);
  if (!noConnection) paragraph(ui, errorText(error), 1, true, fui::TextAlign::Left, 3);
  paragraph(ui, "Tudo continua funcionando. Isto fica guardado no cart\xC3\xA3o at\xC3\xA9 o pr\xC3\xB3ximo sync:", 1, false,
            fui::TextAlign::Left, 3);
  ui.spacer(theme.spaceSm);

  // "Na fila": a heading and one row per count.
  std::vector<fui::ListItem> items;
  std::vector<std::string> values;
  values.reserve(4);
  const auto row = [&](const char* label, int n) {
    values.push_back(std::to_string(n));
    fui::ListItem item;
    item.label = label;
    item.value = values.back().c_str();
    items.push_back(item);
  };
  row("Itens novos", queueItems);
  row("Encontros", queueSaves);
  row("Revis\xC3\xB5" "es", queueReviews);
  if (queueSeen > 0) row("Vistos", queueSeen);
  items[0].sectionHeading = "Na fila";

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
  const int lastH = renderer.getLineHeight(fontFor(0)) + theme.spaceMd;
  const fui::Rect lastRow = ui.takeBottom(static_cast<int16_t>(lastH), theme.spaceSm);
  ui.target().text(fui::Rect{static_cast<int16_t>(lastRow.x + sidePadding()), lastRow.y,
                             static_cast<int16_t>(lastRow.width - 2 * sidePadding()), lastRow.height},
                   last.c_str(), style(ui, 0));

  // The counts are information, not choices: a list with no action.
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = -1;
  props.action = fui::NO_ACTION;
  props.labelText = theme.bodyText;
  props.valueText = theme.bodyText;
  props.valueText.bold = true;
  configureUiList(props, theme, ui.body(), UiListRowType::SingleLine);
  ui.list(props);
}

}  // namespace linguetta
