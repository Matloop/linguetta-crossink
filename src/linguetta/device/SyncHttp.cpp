#include "SyncHttp.h"

#include <Logging.h>

#include <cstdlib>

#include "network/WifiPowerSaveGuard.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif

namespace linguetta {

namespace {
// Error and pairing bodies are small; anything bigger is not a contract response.
constexpr size_t MAX_BUFFERED = 64 * 1024;
}  // namespace

#ifdef SIMULATOR
// The simulator's SecureHttpClient shim has no real transport: sync always reports a network failure.
SyncHttp::SyncHttp() {}
SyncHttp::~SyncHttp() {}
HttpResponse SyncHttp::postJson(const std::string&, const std::string&, const std::string&) { return HttpResponse{}; }
HttpResponse SyncHttp::getStream(const std::string&, const std::string&,
                                 const std::function<bool(const char*, size_t)>&) {
  return HttpResponse{};
}
bool SyncHttp::open(const std::string&, const std::string&, bool) { return false; }
HttpResponse SyncHttp::finish(int) { return HttpResponse{}; }
#else
SyncHttp::SyncHttp() {
  client.setUserAgent("Linguetta-X4/" CROSSPOINT_VERSION);
  // Same trust posture as the rest of the firmware: no CA bundle ships, so the
  // certificate is not verified (traffic is still encrypted).
  client.setInsecure();
  client.setTimeout(30000);
  client.setReuse(true);
}

SyncHttp::~SyncHttp() { client.end(); }

bool SyncHttp::open(const std::string& url, const std::string& bearer, bool json) {
  if (!client.begin(url)) return false;
  client.addHeader("Accept-Encoding", "identity");
  if (json) client.addHeader("Content-Type", "application/json");
  if (!bearer.empty()) client.addHeader("Authorization", "Bearer " + bearer);
  return true;
}

HttpResponse SyncHttp::finish(int status) {
  HttpResponse r;
  r.status = status > 0 ? status : -1;
  r.complete = status > 0 && client.responseComplete();
  const std::string ra = client.getHeader("retry-after");
  if (!ra.empty()) {
    char* end = nullptr;
    const long v = strtol(ra.c_str(), &end, 10);
    if (end != ra.c_str() && v >= 0) r.retryAfterS = static_cast<int>(v);
  }
  return r;
}

HttpResponse SyncHttp::postJson(const std::string& url, const std::string& body, const std::string& bearer) {
  WifiPowerSaveGuard ps;
  if (!open(url, bearer, true)) {
    LOG_ERR("LGT", "connect failed: %s", url.c_str());
    return HttpResponse{};
  }
  std::string buf;
  const int status = client.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                        [&buf](const uint8_t* d, size_t n) {
                                          if (buf.size() + n > MAX_BUFFERED) return false;
                                          buf.append(reinterpret_cast<const char*>(d), n);
                                          return true;
                                        });
  HttpResponse r = finish(status);
  r.body = std::move(buf);
  LOG_INF("LGT", "POST %s -> %d (%u bytes)", url.c_str(), r.status, static_cast<unsigned>(r.body.size()));
  return r;
}

HttpResponse SyncHttp::getStream(const std::string& url, const std::string& bearer,
                                 const std::function<bool(const char*, size_t)>& onData) {
  WifiPowerSaveGuard ps;
  if (!open(url, bearer, false)) {
    LOG_ERR("LGT", "connect failed: %s", url.c_str());
    return HttpResponse{};
  }
  std::string buf;
  // The status line is parsed before the body, so getStatus() is valid inside the callback.
  const int status = client.sendRequest("GET", nullptr, 0, [&](const uint8_t* d, size_t n) {
    if (client.getStatus() == 200) return onData(reinterpret_cast<const char*>(d), n);
    if (buf.size() + n > MAX_BUFFERED) return false;
    buf.append(reinterpret_cast<const char*>(d), n);
    return true;
  });
  HttpResponse r = finish(status);
  r.body = std::move(buf);
  LOG_INF("LGT", "GET %s -> %d", url.c_str(), r.status);
  return r;
}

#endif  // SIMULATOR

}  // namespace linguetta
