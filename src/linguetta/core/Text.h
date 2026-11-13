#pragma once
// Small text and time helpers shared by the Linguetta core (pure C++, host-testable).

#include <cstdint>
#include <string>

namespace linguetta {

// ---- time (UTC, milliseconds since the epoch) ----

// Parses "2026-11-11T08:15:30.000Z" (milliseconds optional, 'Z' or +hh:mm offset).
bool parseIso(const char* s, int64_t& outMs);
inline int64_t parseIsoOr(const char* s, int64_t fallback) {
  int64_t v;
  return (s && parseIso(s, v)) ? v : fallback;
}
// "2026-11-11T08:15:30.000Z"
std::string formatIso(int64_t ms);

struct CivilTime {
  int year, month, day, hour, minute, second, weekday;  // weekday 0 = Sunday
};
CivilTime toCivil(int64_t ms);
int64_t fromCivil(int year, int month, int day);  // midnight UTC, ms

// The contract refuses to record operations before 2026 or with an unset clock.
constexpr int64_t MIN_TRUSTED_MS = 1767225600000LL;  // 2026-01-01T00:00:00Z

// ---- UTF-8 ----

// Cuts to at most maxCodePoints code points without splitting a character.
std::string truncateCodePoints(const std::string& s, size_t maxCodePoints);
size_t codePointCount(const std::string& s);
// Cuts to at most maxBytes bytes at a character boundary.
std::string truncateBytes(const std::string& s, size_t maxBytes);
// Lower-case for matching lemmas: ASCII plus Latin-1 / Latin Extended-A capitals.
std::string lowerKey(const std::string& s);
// Byte offset of `needle` in `hay`, ignoring case as lowerKey does; npos if absent.
size_t findCaseless(const std::string& hay, const std::string& needle);
// Trims ASCII whitespace.
std::string trim(const std::string& s);

// ---- ids ----

// UUID v4 from 16 random bytes.
std::string uuidFromBytes(const uint8_t bytes[16]);

}  // namespace linguetta
