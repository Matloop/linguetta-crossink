#include "Blend.h"

#include <cstring>

namespace linguetta {

namespace {

std::string decodeField(const std::string& f) {
  std::string out;
  out.reserve(f.size());
  for (size_t i = 0; i < f.size(); i++) {
    if (f[i] == '%' && i + 2 < f.size()) {
      const char a = f[i + 1], b = static_cast<char>(f[i + 2] | 0x20);
      if (a == '7' && b == 'c') {
        out.push_back('|');
        i += 2;
        continue;
      }
      if (a == '2' && f[i + 2] == '5') {
        out.push_back('%');
        i += 2;
        continue;
      }
    }
    out.push_back(f[i]);
  }
  return out;
}

std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

// Sentence end: . ! ? … (U+2026), optionally followed by closing quotes / brackets.
bool endsSentence(const std::string& w) {
  size_t n = w.size();
  while (n > 0) {
    const unsigned char c = static_cast<unsigned char>(w[n - 1]);
    if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '*') {
      n--;
      continue;
    }
    // » ” ’ (UTF-8 tails)
    if (n >= 2 && static_cast<unsigned char>(w[n - 2]) == 0xC2 && c == 0xBB) {
      n -= 2;
      continue;
    }
    if (n >= 3 && static_cast<unsigned char>(w[n - 3]) == 0xE2 && static_cast<unsigned char>(w[n - 2]) == 0x80 &&
        (c == 0x9D || c == 0x99)) {
      n -= 3;
      continue;
    }
    break;
  }
  if (n == 0) return false;
  const char last = w[n - 1];
  if (last == '.' || last == '!' || last == '?') return true;
  return n >= 3 && static_cast<unsigned char>(w[n - 3]) == 0xE2 && static_cast<unsigned char>(w[n - 2]) == 0x80 &&
         static_cast<unsigned char>(w[n - 1]) == 0xA6;
}

void append(std::string& out, const std::vector<PageWord>& words, size_t i, size_t first) {
  const PageWord& w = words[i];
  if (i > first) {
    const PageWord& prev = words[i - 1];
    if (prev.hyphenated) {
      if (!out.empty() && out.back() == '-') out.pop_back();
    } else if (w.spaceBefore || w.paragraphStart) {
      out.push_back(' ');
    }
  }
  out += w.text;
}

}  // namespace

bool parseBlendHref(const char* href, BlendLink& out) {
  if (!isBlendHref(href)) return false;
  const size_t len = strnlen(href, 256);
  if (len >= 256) return false;
  std::vector<std::string> fields(1);
  for (size_t i = 4; i < len; i++) {
    if (href[i] == '|') {
      fields.emplace_back();
    } else {
      fields.back().push_back(href[i]);
    }
  }
  if (fields.size() < 2 || fields.size() > 3) return false;
  out.lemma = trimmed(decodeField(fields[0]));
  out.translation = trimmed(decodeField(fields[1]));
  out.note = fields.size() == 3 ? trimmed(decodeField(fields[2])) : std::string();
  return !out.lemma.empty() && !out.translation.empty();
}

LinkContext extractLinkContext(const std::vector<PageWord>& words) {
  LinkContext ctx;
  size_t first = words.size(), last = 0;
  for (size_t i = 0; i < words.size(); i++) {
    if (!words[i].inLink) continue;
    if (first == words.size()) first = i;
    last = i;
  }
  if (first == words.size()) return ctx;
  // The link words must be contiguous; keep the first run.
  for (size_t i = first; i <= last; i++) {
    if (!words[i].inLink) {
      last = i - 1;
      break;
    }
  }
  for (size_t i = first; i <= last; i++) append(ctx.form, words, i, first);

  size_t start = first;
  while (start > 0 && !words[start].paragraphStart && !endsSentence(words[start - 1].text)) start--;
  size_t end = last;
  while (!endsSentence(words[end].text) && end + 1 < words.size() && !words[end + 1].paragraphStart) end++;
  for (size_t i = start; i <= end; i++) append(ctx.sentence, words, i, start);
  return ctx;
}

}  // namespace linguetta
