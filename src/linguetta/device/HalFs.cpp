#include "HalFs.h"

#include <HalStorage.h>

namespace linguetta {

namespace {

class HalFsFile final : public File {
 public:
  explicit HalFsFile(HalFile f) : f(std::move(f)) {}
  ~HalFsFile() override { f.close(); }
  int read(void* buf, size_t n) override { return f.read(buf, n); }
  bool write(const void* buf, size_t n) override { return f.write(buf, n) == n; }
  bool sync() override {
    f.flush();
    return true;
  }
  bool seek(uint64_t pos) override { return f.seek64(pos); }
  uint64_t size() override { return f.fileSize64(); }

 private:
  HalFile f;
};

}  // namespace

std::unique_ptr<File> HalFs::open(const std::string& path, OpenMode mode) {
  oflag_t flags = O_RDONLY;
  if (mode == OpenMode::Write) flags = O_WRONLY | O_CREAT | O_TRUNC;
  if (mode == OpenMode::Append) flags = O_WRONLY | O_CREAT | O_APPEND;
  HalFile f = Storage.open(path.c_str(), flags);
  if (!f || f.isDirectory()) return nullptr;
  return std::unique_ptr<File>(new HalFsFile(std::move(f)));
}

bool HalFs::exists(const std::string& path) { return Storage.exists(path.c_str()); }

bool HalFs::remove(const std::string& path) { return Storage.remove(path.c_str()); }

bool HalFs::replace(const std::string& from, const std::string& to) {
  // CrossInk's HalStorage has no replaceFile(): keep the old target as .bak until the rename lands.
  const std::string bak = to + ".bak";
  const bool hadTarget = Storage.exists(to.c_str());
  if (hadTarget) {
    if (Storage.exists(bak.c_str()) && !Storage.remove(bak.c_str())) return false;
    if (!Storage.rename(to.c_str(), bak.c_str())) return false;
  }
  if (!Storage.rename(from.c_str(), to.c_str())) {
    if (hadTarget) Storage.rename(bak.c_str(), to.c_str());
    return false;
  }
  if (hadTarget) Storage.remove(bak.c_str());
  return true;
}

bool HalFs::mkdirs(const std::string& path) { return Storage.ensureDirectoryExists(path.c_str()); }

std::vector<std::string> HalFs::list(const std::string& dir) {
  std::vector<std::string> out;
  HalFile d = Storage.open(dir.c_str());
  if (!d || !d.isDirectory()) return out;
  char name[256];
  while (true) {
    HalFile e = d.openNextFile();
    if (!e) break;
    if (!e.isDirectory() && e.getName(name, sizeof(name)) > 0) out.emplace_back(name);
    e.close();
  }
  d.close();
  return out;
}

bool HalFs::stat(const std::string& path, uint64_t& size, uint32_t& mtime) {
  HalFile f = Storage.open(path.c_str());
  if (!f) return false;
  size = f.fileSize64();
  mtime = f.modificationTime();
  f.close();
  return true;
}

}  // namespace linguetta
