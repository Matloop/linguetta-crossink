#pragma once
// Minimal file-system seam so the Linguetta core runs on the SD card (HalFs) and in
// host tests (StdFs) without touching upstream storage code.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace linguetta {

class File {
 public:
  virtual ~File() = default;
  virtual int read(void* buf, size_t n) = 0;  // bytes read, 0 at end, <0 on error
  virtual bool write(const void* buf, size_t n) = 0;
  virtual bool sync() = 0;  // flush + fsync
  virtual bool seek(uint64_t pos) = 0;
  virtual uint64_t size() = 0;
  bool writeStr(const std::string& s) { return write(s.data(), s.size()); }
};

enum class OpenMode { Read, Write, Append };

class Fs {
 public:
  virtual ~Fs() = default;
  virtual std::unique_ptr<File> open(const std::string& path, OpenMode mode) = 0;
  virtual bool exists(const std::string& path) = 0;
  virtual bool remove(const std::string& path) = 0;
  // Moves `from` over `to`, replacing it (the atomic step of a rewrite).
  virtual bool replace(const std::string& from, const std::string& to) = 0;
  virtual bool mkdirs(const std::string& path) = 0;
  // Plain file names (no directories) inside `dir`.
  virtual std::vector<std::string> list(const std::string& dir) = 0;
  // Size and an opaque modification stamp, for caches keyed by path + size + mtime.
  virtual bool stat(const std::string& path, uint64_t& size, uint32_t& mtime) = 0;
};

// Reads a whole (small) file. False if missing or larger than maxBytes.
bool readFile(Fs& fs, const std::string& path, std::string& out, size_t maxBytes = 64 * 1024);
// Appends one line (no '\n' inside) and syncs it; repairs a torn last line first.
bool appendLine(Fs& fs, const std::string& path, const std::string& line);
// tmp + sync + replace.
bool writeFileAtomic(Fs& fs, const std::string& path, const std::string& content);

// Line reader with a fixed maximum line length (the contract caps JSONL lines at 16 KB).
class LineReader {
 public:
  LineReader(File& f, size_t maxLine = 16384);
  // Next line without the '\n'. `complete` is false for a last line with no '\n'.
  // Lines longer than maxLine are skipped (returned as `overlong`).
  bool next(std::string& line, bool& complete);
  uint64_t lineOffset() const { return lineStart; }
  bool overlong() const { return lastOverlong; }

 private:
  bool fill();
  File& file;
  size_t maxLine;
  std::vector<char> buf;
  size_t pos = 0, len = 0;
  uint64_t consumed = 0;  // bytes before buf[0]
  uint64_t lineStart = 0;
  bool eof = false;
  bool lastOverlong = false;
};

}  // namespace linguetta
