#pragma once
// Host implementation of Fs over stdio, rooted at a directory (tests only).

#include "Fs.h"

namespace linguetta {

class StdFs : public Fs {
 public:
  explicit StdFs(std::string root) : root(std::move(root)) {}
  std::unique_ptr<File> open(const std::string& path, OpenMode mode) override;
  bool exists(const std::string& path) override;
  bool remove(const std::string& path) override;
  bool replace(const std::string& from, const std::string& to) override;
  bool mkdirs(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;
  bool stat(const std::string& path, uint64_t& size, uint32_t& mtime) override;
  std::string real(const std::string& path) const { return root + path; }

 private:
  std::string root;
};

}  // namespace linguetta
