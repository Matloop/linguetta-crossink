#pragma once
// linguetta::Fs over the upstream HalStorage (SD card).

#include "linguetta/core/Fs.h"

namespace linguetta {

class HalFs final : public Fs {
 public:
  std::unique_ptr<File> open(const std::string& path, OpenMode mode) override;
  bool exists(const std::string& path) override;
  bool remove(const std::string& path) override;
  bool replace(const std::string& from, const std::string& to) override;
  bool mkdirs(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;
  bool stat(const std::string& path, uint64_t& size, uint32_t& mtime) override;
};

}  // namespace linguetta
