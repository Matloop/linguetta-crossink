#pragma once
// core::Http over the upstream HTTPS client (freeink::SecureHttpClient). One client per
// sync so the TLS session is reused across push batches and snapshot pages.

#include <SecureHttpClient.h>

#include "linguetta/core/Sync.h"

namespace linguetta {

class SyncHttp : public Http {
 public:
  SyncHttp();
  ~SyncHttp() override;

  HttpResponse postJson(const std::string& url, const std::string& body, const std::string& bearer) override;
  HttpResponse getStream(const std::string& url, const std::string& bearer,
                         const std::function<bool(const char*, size_t)>& onData) override;

 private:
  bool open(const std::string& url, const std::string& bearer, bool json);
  HttpResponse finish(int status);
  freeink::SecureHttpClient client;
};

}  // namespace linguetta
