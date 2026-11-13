#pragma once
// "Sincronizar": Wi-Fi, clock (NTP), pairing by QR code when there is no token, then push of
// the outbox and the bank snapshot, with a result screen. Without a connection (or on any
// error) it shows what stays queued on the card. Spec: section 3 "Parear e sincronizar".

#include <memory>
#include <string>

#include "linguetta/core/Sync.h"
#include "linguetta/ui/Screen.h"

namespace linguetta {

class SyncHttp;

class SyncActivity final : public ui::Screen {
 public:
  SyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~SyncActivity() override;

 private:
  enum class Phase { NeedWifi, Wifi, Pairing, PairEnded, OtherAccount, Syncing, Done, Offline };

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  void onAction(int id) override;
  void onTick() override;
  bool preventAutoSleep() override { return phase == Phase::Pairing || phase == Phase::Syncing; }

  void connect();
  void openWifi();
  void renderNeedWifi(int y);
  void afterWifi(bool connected);
  void startPairing();
  void poll();
  void acceptPairing(bool discardLocal);
  void doSync();
  void fail(const SyncError& e, bool offline = false);
  void loadQueue();
  int64_t clockMs() const;

  void renderPairing(int y);
  void renderSyncing(int y);
  void renderOffline(int y);

  Phase phase = Phase::Wifi;
  bool firstRender = true;
  bool startPending = false;
  std::unique_ptr<SyncHttp> http;  // one TLS session for the whole visit

  // pairing
  PairStart code;
  PairPoll approved;
  unsigned long expiresAt = 0, nextPollAt = 0;
  int interval = 5;
  std::string pairEndTitle, pairEndText;

  // sync
  int queueItems = 0, queueSaves = 0, queueReviews = 0, queueSeen = 0;
  int pushDone = 0, pushTotal = 0, recvDone = 0, recvTotal = 0;
  unsigned long lastDraw = 0;
  std::string resultText;

  // offline / error
  SyncError error;
  bool noConnection = false;
};

}  // namespace linguetta
