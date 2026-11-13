#pragma once
// SHA-256 (FIPS 180-4), streaming. Used for the stable content hash of a deckImport.

#include <cstddef>
#include <cstdint>
#include <string>

namespace linguetta {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t len);
  void update(const std::string& s) { update(s.data(), s.size()); }
  // Lowercase hex digest (64 chars). The object must not be updated afterwards.
  std::string hex();

 private:
  void block(const uint8_t* p);
  uint32_t h[8];
  uint8_t buf[64];
  size_t used = 0;
  uint64_t bits = 0;
};

}  // namespace linguetta
