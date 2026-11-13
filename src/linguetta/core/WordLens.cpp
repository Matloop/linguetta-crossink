#include "WordLens.h"

#include <ArduinoJson.h>

#include <cstring>

#include "Store.h"
#include "Text.h"

namespace linguetta::lens {

namespace {

std::string s(JsonVariantConst v) { return v.is<const char*>() ? std::string(v.as<const char*>()) : std::string(); }

// Punctuation that may cling to a word: ASCII marks plus the common Unicode ones
// (« » “ ” „ ‘ ’ ‹ › — – … ¿ ¡ ·). Returns its byte length at `p`, or 0.
size_t punctAt(const std::string& t, size_t p) {
  const auto c = static_cast<unsigned char>(t[p]);
  if (c < 0x80) {
    return (std::strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", c) && c != 0) ? 1 : 0;
  }
  if (c == 0xC2 && p + 1 < t.size()) {
    const auto c2 = static_cast<unsigned char>(t[p + 1]);
    // « » ¿ ¡ · and NBSP
    return (c2 == 0xAB || c2 == 0xBB || c2 == 0xBF || c2 == 0xA1 || c2 == 0xB7 || c2 == 0xA0) ? 2 : 0;
  }
  if (c == 0xE2 && p + 2 < t.size() && static_cast<unsigned char>(t[p + 1]) == 0x80) {
    const auto c3 = static_cast<unsigned char>(t[p + 2]);
    // U+2010..U+201F dashes and quotes, U+2026 …, U+2039 ‹, U+203A ›, U+2003 em space
    if ((c3 >= 0x90 && c3 <= 0x9F) || c3 == 0xA6 || c3 == 0xB9 || c3 == 0xBA || c3 == 0x83) return 3;
  }
  return 0;
}

// Start of the UTF-8 character that ends right before byte `end`.
size_t prevCharStart(const std::string& t, size_t end) {
  size_t p = end - 1;
  while (p > 0 && (static_cast<unsigned char>(t[p]) & 0xC0) == 0x80) p--;
  return p;
}

bool hasLetter(const std::string& t) {
  for (const char ch : t) {
    const auto c = static_cast<unsigned char>(ch);
    if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  }
  return false;
}

}  // namespace

const char* levelLabel(const Level level) {
  switch (level) {
    case Level::Off:
      return "Desligado";
    case Level::Muita:
      return "Muita";
    case Level::Media:
      return "M\xC3\xA9" "dia";
    case Level::Pouca:
      return "Pouca";
  }
  return "";
}

const char* levelCode(const Level level) {
  switch (level) {
    case Level::Off:
      return "off";
    case Level::Muita:
      return "muita";
    case Level::Media:
      return "media";
    case Level::Pouca:
      return "pouca";
  }
  return "";
}

bool parseLevel(const std::string& code, Level& out) {
  for (const Level l : {Level::Off, Level::Muita, Level::Media, Level::Pouca}) {
    if (code == levelCode(l)) {
      out = l;
      return true;
    }
  }
  return false;
}

bool shows(const ViewEntry& e, const Level level) {
  if (level == Level::Off || e.known) return false;
  if (e.card.state != 2) return true;  // new, learning, relearning (offline saves are new)
  switch (level) {
    case Level::Muita:
      return true;
    case Level::Media:
      return e.card.stability < MEDIA_MAX_STABILITY;
    case Level::Pouca:
      return e.card.stability < POUCA_MAX_STABILITY;
    case Level::Off:
      break;
  }
  return false;
}

WordKey wordKey(const std::string& text) {
  WordKey out;
  size_t a = 0, b = text.size();
  while (a < b) {
    const size_t n = punctAt(text, a);
    if (n == 0) break;
    a += n;
    out.breakBefore = true;
  }
  while (b > a) {
    const size_t p = prevCharStart(text, b);
    if (punctAt(text, p) == 0) break;
    b = p;
    out.breakAfter = true;
  }
  std::string core = text.substr(a, b - a);
  // Typographic apostrophe -> ASCII, so "l’acqua" and "l'acqua" are one key.
  for (size_t p = 0; (p = core.find("\xE2\x80\x99", p)) != std::string::npos;) core.replace(p, 3, "'");
  if (!hasLetter(core)) return out;
  out.key = lowerKey(core);
  const size_t ap = out.key.find_last_of('\'');
  if (ap != std::string::npos && ap > 0 && ap + 1 < out.key.size()) out.elided = out.key.substr(ap + 1);
  return out;
}

std::string itemKey(const std::string& text) {
  std::string out;
  size_t start = 0;
  const std::string t = trim(text);
  while (start <= t.size()) {
    size_t sp = t.find(' ', start);
    if (sp == std::string::npos) sp = t.size();
    if (sp > start) {
      const WordKey w = wordKey(t.substr(start, sp - start));
      if (!w.key.empty()) {
        if (!out.empty()) out.push_back(' ');
        out += w.key;
      }
    }
    start = sp + 1;
  }
  return out;
}

void Index::clear() {
  glosses.clear();
  byKey.clear();
  longest = 1;
}

void Index::add(const std::string& raw, const int gloss) {
  const std::string key = itemKey(raw);
  if (key.empty() || byKey.count(key)) return;
  size_t words = 1;
  for (const char c : key) words += c == ' ';
  if (words > MAX_WORDS) return;
  byKey.emplace(key, gloss);
  if (words > longest) longest = words;
}

int Index::find(const std::string& key) const {
  auto it = byKey.find(key);
  return it == byKey.end() ? -1 : it->second;
}

void Index::build(Fs& fs, const LocalView& view, const Level level) {
  clear();
  if (level == Level::Off || view.size() == 0) return;
  std::vector<char> done(view.size(), 0);

  const auto addGloss = [&](const int idx, const std::string& lemma, const std::string& text) {
    Gloss g;
    g.entry = idx;
    g.lemma = lemma;
    g.text = text;
    glosses.push_back(std::move(g));
    return static_cast<int>(glosses.size() - 1);
  };

  const std::string path = bankPath(view.language());
  if (auto f = path.empty() ? nullptr : fs.open(path, OpenMode::Read)) {
    LineReader reader(*f);
    std::string line;
    bool complete;
    bool first = true;
    while (reader.next(line, complete)) {
      if (reader.overlong() || line.empty()) continue;
      if (first) {
        first = false;
        continue;  // header (LocalView already checked the language)
      }
      JsonDocument doc;
      if (deserializeJson(doc, line) != DeserializationError::Ok || s(doc["t"]) != "item") continue;
      const std::string lemma = s(doc["lemma"]);
      const int idx = view.find(lemma);
      if (idx < 0 || done[static_cast<size_t>(idx)]) continue;
      done[static_cast<size_t>(idx)] = 1;
      const ViewEntry& e = view.entry(static_cast<size_t>(idx));
      if (!shows(e, level)) continue;
      std::string text = s(doc["sense"]);
      if (text.empty() && e.local) text = e.local->sense;
      if (text.empty() && e.local) text = e.local->encounter.sense;
      if (text.empty()) text = s(doc["encounters"][0]["sense"]);
      if (text.empty()) continue;
      const int g = addGloss(idx, lemma, text);
      add(lemma, g);
      for (JsonVariantConst form : doc["forms"].as<JsonArrayConst>()) add(s(form), g);
      for (JsonVariantConst enc : doc["encounters"].as<JsonArrayConst>()) add(s(enc["form"]), g);
      if (e.local) add(e.local->encounter.form, g);
    }
  }

  // Items saved offline and not in the bank yet.
  for (size_t i = 0; i < view.size(); i++) {
    const ViewEntry& e = view.entry(i);
    if (done[i] || e.bankOffset >= 0 || !e.local || !shows(e, level)) continue;
    const ItemDetails& d = *e.local;
    const std::string text = !d.sense.empty() ? d.sense : d.encounter.sense;
    if (text.empty()) continue;
    const int g = addGloss(static_cast<int>(i), d.lemma, text);
    add(d.lemma, g);
    add(d.encounter.form, g);
  }
}

std::vector<Span> Index::match(const std::vector<Token>& tokens) const {
  std::vector<Span> out;
  if (byKey.empty()) return out;
  size_t i = 0;
  while (i < tokens.size()) {
    if (tokens[i].word.key.empty()) {
      i++;
      continue;
    }
    bool found = false;
    for (size_t n = std::min(longest, tokens.size() - i); n >= 1 && !found; n--) {
      // The words after the first must continue the same phrase.
      bool ok = true;
      for (size_t k = 1; k < n && ok; k++) {
        const Token& prev = tokens[i + k - 1];
        const Token& t = tokens[i + k];
        ok = !t.word.key.empty() && !t.paragraphStart && !t.word.breakBefore && !prev.word.breakAfter;
      }
      if (!ok) continue;
      std::string rest;
      for (size_t k = 1; k < n; k++) rest += " " + tokens[i + k].word.key;
      int g = find(tokens[i].word.key + rest);
      if (g < 0 && !tokens[i].word.elided.empty()) g = find(tokens[i].word.elided + rest);
      if (g >= 0) {
        out.push_back(Span{i, n, g});
        i += n;
        found = true;
      }
    }
    if (!found) i++;
  }
  return out;
}

bool State::markSeen(const int64_t dayStart, const std::string& book, const std::string& key) {
  if (day != dayStart) {
    day = dayStart;
    seen.clear();
  }
  return seen.insert(book + "|" + key).second;
}

State loadState(Fs& fs) {
  State st;
  std::string text;
  if (!readFile(fs, STATE_FILE, text)) return st;
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return st;
  Level l;
  if (parseLevel(s(doc["level"]), l)) st.level = l;
  st.day = doc["day"] | static_cast<int64_t>(0);
  for (JsonVariantConst v : doc["seen"].as<JsonArrayConst>()) {
    if (v.is<const char*>()) st.seen.insert(v.as<const char*>());
  }
  return st;
}

bool saveState(Fs& fs, const State& st) {
  JsonDocument doc;
  doc["level"] = levelCode(st.level);
  doc["day"] = st.day;
  JsonArray arr = doc["seen"].to<JsonArray>();
  for (const auto& k : st.seen) arr.add(k);
  std::string out;
  serializeJson(doc, out);
  return writeFileAtomic(fs, STATE_FILE, out);
}

}  // namespace linguetta::lens
