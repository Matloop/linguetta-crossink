// Host-only Fs over stdio, used by the Linguetta tests.
#include "StdFs.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace linguetta {

namespace {

class StdFile : public File {
 public:
  explicit StdFile(FILE* f) : f(f) {}
  ~StdFile() override { fclose(f); }
  int read(void* buf, size_t n) override { return static_cast<int>(fread(buf, 1, n, f)); }
  bool write(const void* buf, size_t n) override { return fwrite(buf, 1, n, f) == n; }
  bool sync() override { return fflush(f) == 0 && fsync(fileno(f)) == 0; }
  bool seek(uint64_t pos) override { return fseek(f, static_cast<long>(pos), SEEK_SET) == 0; }
  uint64_t size() override {
    struct stat st {};
    fflush(f);
    return fstat(fileno(f), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
  }

 private:
  FILE* f;
};

}  // namespace

std::unique_ptr<File> StdFs::open(const std::string& path, OpenMode mode) {
  const char* m = mode == OpenMode::Read ? "rb" : mode == OpenMode::Write ? "wb" : "ab";
  FILE* f = fopen(real(path).c_str(), m);
  if (!f) return nullptr;
  return std::make_unique<StdFile>(f);
}

bool StdFs::exists(const std::string& path) { return access(real(path).c_str(), F_OK) == 0; }
bool StdFs::remove(const std::string& path) { return ::remove(real(path).c_str()) == 0; }
bool StdFs::replace(const std::string& from, const std::string& to) {
  return ::rename(real(from).c_str(), real(to).c_str()) == 0;
}

bool StdFs::mkdirs(const std::string& path) {
  std::string acc;
  for (size_t i = 1; i <= path.size(); i++) {
    if (i == path.size() || path[i] == '/') {
      acc = path.substr(0, i);
      ::mkdir(real(acc).c_str(), 0755);
    }
  }
  struct stat st {};
  return ::stat(real(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> StdFs::list(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(real(dir).c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    struct stat st {};
    if (::stat(real(dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(name);
  }
  closedir(d);
  return out;
}

bool StdFs::stat(const std::string& path, uint64_t& size, uint32_t& mtime) {
  struct stat st {};
  if (::stat(real(path).c_str(), &st) != 0) return false;
  size = static_cast<uint64_t>(st.st_size);
  mtime = static_cast<uint32_t>(st.st_mtime);
  return true;
}

}  // namespace linguetta

