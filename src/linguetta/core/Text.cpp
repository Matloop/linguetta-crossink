#include "Text.h"

#include <cstdio>
#include <cstring>

namespace linguetta {

namespace {

// Howard Hinnant's days_from_civil / civil_from_days.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

bool digits(const char*& p, int n, int& out) {
  out = 0;
  for (int i = 0; i < n; i++) {
    if (*p < '0' || *p > '9') return false;
    out = out * 10 + (*p - '0');
    p++;
  }
  return true;
}

size_t seqLen(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

}  // namespace

bool parseIso(const char* s, int64_t& outMs) {
  if (!s) return false;
  const char* p = s;
  int y, mo, d, h = 0, mi = 0, se = 0, ms = 0;
  if (!digits(p, 4, y) || *p++ != '-' || !digits(p, 2, mo) || *p++ != '-' || !digits(p, 2, d)) return false;
  if (*p == 'T' || *p == ' ') {
    p++;
    if (!digits(p, 2, h) || *p++ != ':' || !digits(p, 2, mi)) return false;
    if (*p == ':') {
      p++;
      if (!digits(p, 2, se)) return false;
      if (*p == '.') {
        p++;
        int scale = 100;
        while (*p >= '0' && *p <= '9') {
          ms += (*p - '0') * scale;
          scale /= 10;
          p++;
        }
      }
    }
  }
  int offsetMin = 0;
  if (*p == 'Z') {
    p++;
  } else if (*p == '+' || *p == '-') {
    const int sign = *p == '-' ? -1 : 1;
    p++;
    int oh, om = 0;
    if (!digits(p, 2, oh)) return false;
    if (*p == ':') p++;
    if (*p >= '0' && *p <= '9' && !digits(p, 2, om)) return false;
    offsetMin = sign * (oh * 60 + om);
  }
  if (*p != '\0') return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
  const int64_t days = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  outMs = ((days * 24 + h) * 60 + mi - offsetMin) * 60000LL + se * 1000LL + ms;
  return true;
}

CivilTime toCivil(int64_t ms) {
  int64_t days = ms / 86400000LL;
  int64_t rem = ms % 86400000LL;
  if (rem < 0) {
    rem += 86400000LL;
    days--;
  }
  CivilTime t{};
  civilFromDays(days, t.year, t.month, t.day);
  const int secs = static_cast<int>(rem / 1000);
  t.hour = secs / 3600;
  t.minute = (secs / 60) % 60;
  t.second = secs % 60;
  t.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  return t;
}

int64_t fromCivil(int year, int month, int day) {
  return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400000LL;
}

std::string formatIso(int64_t ms) {
  const CivilTime t = toCivil(ms);
  int64_t milli = ms % 1000;
  if (milli < 0) milli += 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.year, t.month, t.day, t.hour, t.minute, t.second,
           static_cast<int>(milli));
  return buf;
}

std::string truncateCodePoints(const std::string& s, size_t maxCodePoints) {
  size_t i = 0, count = 0;
  while (i < s.size() && count < maxCodePoints) {
    i += seqLen(static_cast<unsigned char>(s[i]));
    count++;
  }
  if (i > s.size()) i = s.size();
  return s.substr(0, i);
}

size_t codePointCount(const std::string& s) {
  size_t i = 0, count = 0;
  while (i < s.size()) {
    i += seqLen(static_cast<unsigned char>(s[i]));
    count++;
  }
  return count;
}

std::string truncateBytes(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t i = maxBytes;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
  return s.substr(0, i);
}

std::string lowerKey(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c + 32));
    } else if (c == 0xC3 && i + 1 < s.size()) {
      // U+00C0..U+00DE (except U+00D7 ×) -> +0x20
      auto c2 = static_cast<unsigned char>(s[i + 1]);
      if (c2 >= 0x80 && c2 <= 0x9E && c2 != 0x97) c2 += 0x20;
      out.push_back(static_cast<char>(c));
      out.push_back(static_cast<char>(c2));
      i++;
    } else if (c >= 0xC4 && c <= 0xC5 && i + 1 < s.size()) {
      // Latin Extended-A U+0100..U+017F: mostly even = capital, odd = small.
      const auto c2 = static_cast<unsigned char>(s[i + 1]);
      unsigned cp = ((c & 0x1F) << 6) | (c2 & 0x3F);
      const bool oddPairs = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
      if (oddPairs ? (cp & 1) : !(cp & 1)) {
        if (cp != 0x130 && cp != 0x138 && cp != 0x178) cp += 1;
      }
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      i++;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

std::string uuidFromBytes(const uint8_t in[16]) {
  uint8_t b[16];
  memcpy(b, in, 16);
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
  char out[37];
  snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
           b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return out;
}

size_t findCaseless(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return std::string::npos;
  const std::string h = lowerKey(hay), n = lowerKey(needle);
  // lowerKey keeps byte lengths for the letters it folds; if not, fall back to an exact match.
  if (h.size() != hay.size() || n.size() != needle.size()) return hay.find(needle);
  return h.find(n);
}

}  // namespace linguetta
