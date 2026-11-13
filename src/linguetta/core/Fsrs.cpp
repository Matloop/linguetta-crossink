#include "Fsrs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Port notes (reference: ts-fsrs 6.0.0 dist/models/fsrs-7.mjs, dist/middlewares-*.mjs,
// srs-kit 1.0.0 scheduler). The middleware pipeline for one review, in the server's order
// (desired retention, fuzz, stats, scheduled days, learning steps, max interval, monotonic):
//   interval(g) = learningStepWrapper(fuzzWrapper(modelInterval(step(g))))
//   terminal: sd = interval(grade); state = Review
//   monotonic: sd = scheduleDay(interval(Again..grade-1), sd)
//   max interval clamp; learning-step post (keep the exact step); state transition; stats.

namespace linguetta {

const char* CardState::scheduleStatus() const {
  switch (scheduleStatusCode) {
    case 1:
      return "learning";
    case 2:
      return "review";
    default:
      return "new";
  }
}

bool CardState::setScheduleStatus(const char* s) {
  if (!s) return false;
  if (strcmp(s, "new") == 0) {
    scheduleStatusCode = 0;
  } else if (strcmp(s, "learning") == 0) {
    scheduleStatusCode = 1;
  } else if (strcmp(s, "review") == 0) {
    scheduleStatusCode = 2;
  } else {
    return false;
  }
  return true;
}

namespace fsrs {
namespace {

constexpr double W[34] = {.1104,  2.2395, 3.9221, 11.7841, 6.1686, .6457, 3.6807, 1.9795, 0,     1.3826, .7024, .5999,
                          .8146,  .6398,  1,      1.3207,  .6707,  3.8668, .4416, .0934,  1.8631, .6162, 1.0869, .1567,
                          .0801,  .2421,  .9464,  .1433,   .7145,  0,      .5667, .3734,  .5333,  .3048};

constexpr double S_MIN = 1e-4;
constexpr double S_MAX = 36500;
constexpr double D_MIN = 1;
constexpr double D_MAX = 10;
constexpr double MIN_T = 1.0 / 86400.0;
constexpr double DR_MIN = 1e-4;
constexpr double DR_MAX = .9999;
constexpr double MAXIMUM_INTERVAL = 36500;
constexpr double GRADUATING_INTERVAL = 1;
// The server turns FSRS-7's default allowModelOverride off: a defined step is the interval,
// so new and relearning cards go through 1m/10m like Anki instead of jumping to the model.
constexpr bool ALLOW_MODEL_OVERRIDE = false;
constexpr double SECONDS_PER_DAY = 86400;

// Learning steps in minutes.
constexpr double LEARNING_STEPS[] = {1, 10};
constexpr int LEARNING_STEPS_LEN = 2;
constexpr double RELEARNING_STEPS[] = {10};
constexpr int RELEARNING_STEPS_LEN = 1;

enum Grade { Again = 1, Hard = 2, Good = 3, Easy = 4 };

double clampd(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

// JS Math.round: halves go towards +infinity.
double jsRound(double v) { return std::floor(v + 0.5); }

double roundTo8(double v) { return jsRound(v * 1e8) / 1e8; }

struct Memory {
  double s = 0, sFast = 0, d = 0;
};

struct FastParams {
  double decay, scale;
};

FastParams prepareFast(double s) {
  const double decay = -clampd(W[23] * std::pow(s, W[33] - .3), .01, .95);
  return {decay, (std::exp(std::min(std::log(W[25]) / decay, 60.0)) - 1) / s};
}

struct Fast {
  double recall, derivative;
};

Fast fastComponent(double t, const FastParams& p) {
  const double days = std::max(t, 0.0);
  const double base = 1 + p.scale * days;
  return {std::pow(base, p.decay), p.decay * std::pow(base, p.decay - 1) * p.scale};
}

struct CurveParams {
  FastParams fast;
  double decay, scale, weight1, weight2, total;
};

CurveParams prepareCurve(const Memory& m) {
  const double s = clampd(m.s, S_MIN, S_MAX);
  const double sFast = clampd(m.sFast, S_MIN, S_MAX);
  const double d = clampd(m.d, D_MIN, D_MAX);
  CurveParams p;
  p.fast = prepareFast(sFast);
  p.decay = -clampd(W[24], .01, .95);
  p.scale = (std::pow(W[26], 1 / p.decay) - 1) * std::exp((d - 5) * (W[32] - .3)) / s;
  p.weight1 = W[27] * std::pow(sFast, -W[29]);
  p.weight2 = W[28] * std::pow(s, W[30]) * std::exp((d - 5) * (W[31] - .5));
  p.total = p.weight1 + p.weight2;
  return p;
}

struct Curve {
  double retrievability, derivative;
};

Curve computeCurve(double t, const CurveParams& p) {
  const double days = std::max(t, 0.0);
  const Fast fast = fastComponent(days, p.fast);
  const double base = 1 + p.scale * days;
  const double slow = std::pow(base, p.decay);
  const double derivative = p.decay * std::pow(base, p.decay - 1) * p.scale;
  return {(p.weight1 * fast.recall + p.weight2 * slow) / p.total * .99998 + 1e-5,
          (p.weight1 * fast.derivative + p.weight2 * derivative) / p.total * .99998};
}

double forgettingCurve(const Memory& m, double elapsedDays) {
  return computeCurve(elapsedDays, prepareCurve(m)).retrievability;
}

double modelNextInterval(const Memory& m, double desiredRetention) {
  const double target = clampd(desiredRetention, DR_MIN, DR_MAX);
  if (target >= .9999) return 0;
  const double s = clampd(m.s, S_MIN, S_MAX);
  const double sFast = clampd(m.sFast, S_MIN, S_MAX);
  const double maxStability = std::max(s, sFast);
  const CurveParams params = prepareCurve(m);
  const double logMinT = std::log(MIN_T);
  const double logSMax = std::log(S_MAX);
  double logT = std::log(maxStability);
  for (int i = 0; i < 7; i++) {
    logT = clampd(logT, logMinT, logSMax);
    const double t = clampd(std::exp(logT), MIN_T, S_MAX);
    const Curve c = computeCurve(t, params);
    logT -= clampd((c.retrievability - target) / std::min(c.derivative * t, -1e-12), -4, 4);
    if (!std::isfinite(logT)) break;
  }
  const double interval = clampd(std::exp(logT), 0, S_MAX);
  if (std::fabs(computeCurve(interval, params).retrievability - target) <= .001) return interval;
  double low = 0;
  double high = std::min(std::max(maxStability, 1.0), S_MAX);
  while (computeCurve(high, params).retrievability > target && high < S_MAX) high = std::min(high * 2, S_MAX);
  for (int i = 0; i < 50; i++) {
    const double mid = (low + high) / 2;
    if (computeCurve(mid, params).retrievability > target) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return (low + high) / 2;
}

double initStability(int g) { return clampd(W[g - 1], S_MIN, S_MAX); }
double initDifficulty(int g) { return W[4] - std::exp(W[5] * (g - 1)) + 1; }

double nextStability(double s, double d, double r, int g, int start) {
  const double fail = W[start + 3] * (std::pow(s + 1, W[start + 4]) - 1) * std::exp((1 - r) * W[start + 5]);
  const double pls = std::min(s, fail);
  if (g == Again) return pls;
  const double hardPenalty = g == Hard ? W[start + 6] : 1;
  const double easyBonus = g == Easy ? W[start + 7] : 1;
  const double increase = std::exp(W[start] - 1.5) * (11 - d) * std::pow(s, -W[start + 1]) *
                              (std::exp((1 - r) * W[start + 2]) - 1) * hardPenalty * easyBonus +
                          1;
  return std::max(pls, s * increase);
}

Memory nextState(const Memory& state, double elapsedDays, int g, double r) {
  Memory out;
  if (state.s == 0 && state.sFast == 0 && state.d == 0) {
    const double stability = initStability(g);
    out.s = clampd(roundTo8(stability), S_MIN, S_MAX);
    out.sFast = clampd(roundTo8(stability * .8), S_MIN, S_MAX);
    out.d = clampd(roundTo8(initDifficulty(g)), D_MIN, D_MAX);
    return out;
  }
  const double s = clampd(state.s, S_MIN, S_MAX);
  const double sFast = clampd(state.sFast, S_MIN, S_MAX);
  const double d = clampd(state.d, D_MIN, D_MAX);
  const double stability = nextStability(s, d, r, g, 7);
  double stabilityFast = nextStability(sFast, d, fastComponent(elapsedDays, prepareFast(sFast)).recall, g, 15);
  if (g == Again) stabilityFast = std::min(stabilityFast, stability * .8);
  double deltaD = -W[6] * (g - 3);
  if (g == Again) deltaD *= r + .1;
  const double difficulty = .01 * initDifficulty(Easy) + .99 * (d + (10 - d) * deltaD / 9);
  out.s = clampd(roundTo8(stability), S_MIN, S_MAX);
  out.sFast = clampd(roundTo8(stabilityFast), S_MIN, S_MAX);
  out.d = clampd(roundTo8(difficulty), D_MIN, D_MAX);
  return out;
}

// ---- fuzz ----

uint32_t fnv1a32(const std::string& utf8) {
  // Over UTF-16 code units, like String.prototype.charCodeAt.
  uint32_t hash = 2166136261u;
  auto mix = [&hash](uint32_t unit) {
    hash ^= unit;
    hash *= 16777619u;
  };
  const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
  const size_t n = utf8.size();
  for (size_t i = 0; i < n;) {
    uint32_t cp = p[i];
    int len = 1;
    if (cp >= 0xF0 && i + 3 < n) {
      cp = ((cp & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) | ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
      len = 4;
    } else if (cp >= 0xE0 && i + 2 < n) {
      cp = ((cp & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
      len = 3;
    } else if (cp >= 0xC0 && i + 1 < n) {
      cp = ((cp & 0x1F) << 6) | (p[i + 1] & 0x3F);
      len = 2;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      mix(0xD800 + (cp >> 10));
      mix(0xDC00 + (cp & 0x3FF));
    } else {
      mix(cp);
    }
    i += len;
  }
  return hash;
}

double mulberry32First(uint32_t seed) {
  uint32_t a = seed + 1831565813u;
  uint32_t t = a;
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
}

double fuzz(double interval, double elapsedDays, const std::string& seed) {
  if (interval < 2.5) return interval;  // fractionalDays: unfuzzed intervals stay fractional
  struct Range {
    double start, end, factor;
  };
  static constexpr Range RANGES[] = {{2.5, 7, .15}, {7, 20, .1}, {20, INFINITY, .05}};
  double delta = 1;
  for (const auto& r : RANGES) delta += r.factor * std::max(std::min(interval, r.end) - r.start, 0.0);
  const double capped = std::min(interval, MAXIMUM_INTERVAL);
  double minInterval = std::max(2.0, jsRound(capped - delta));
  const double maxInterval = std::min(jsRound(capped + delta), MAXIMUM_INTERVAL);
  if (capped > elapsedDays) minInterval = std::max(minInterval, elapsedDays + 1);
  minInterval = std::min(minInterval, maxInterval);
  const double factor = mulberry32First(fnv1a32(seed));
  return std::floor(factor * (maxInterval - minInterval + 1) + minInterval);
}

// ---- learning steps ----

struct Step {
  bool defined = false;
  double minutes = 0;
  int nextStep = 0;
};

struct Resolved {
  Step steps[5];            // by grade
  bool hasStepInterval[5] = {};
  double stepInterval[5] = {};
  bool hasSteps = false;
  bool overridesStep[5] = {};
};

Resolved resolveLearningSteps(int state, int learningStep) {
  Resolved r;
  const bool relearn = state == 3 || state == 2;
  const double* steps = relearn ? RELEARNING_STEPS : LEARNING_STEPS;
  const int len = relearn ? RELEARNING_STEPS_LEN : LEARNING_STEPS_LEN;
  r.hasSteps = len > 0;
  if (len > 0 && learningStep < len) {
    if (state == 2) {
      r.steps[Again] = {true, steps[std::max(0, learningStep)], 0};
    } else {
      const double first = steps[0];
      const bool hasSecond = len > 1;
      const double second = hasSecond ? steps[1] : 0;
      r.steps[Again] = {true, first, 0};
      r.steps[Hard] = {true, hasSecond ? (first + second) / 2 : first * 1.5, learningStep};
      if (learningStep + 1 < len) {
        const double nextMinutes = (learningStep == 0 && hasSecond) ? second : steps[learningStep + 1];
        if (nextMinutes > 0) r.steps[Good] = {true, nextMinutes, learningStep + 1};
      }
    }
  }
  for (int g = Again; g <= Easy; g++) {
    if (!r.steps[g].defined) continue;
    const double minutes = jsRound(std::max(0.0, r.steps[g].minutes) * 60) / 60;
    if (minutes > 0) {
      r.hasStepInterval[g] = true;
      r.stepInterval[g] = minutes / 1440;
    }
  }
  return r;
}

struct Ctx {
  Memory memory;
  double elapsedDays;
  double retrievability;
  Memory stepped[5];
  bool haveStepped[5] = {};
  std::string seed;
  Resolved resolved;
  double retention = DEFAULT_RETENTION;
};

const Memory& stepFor(Ctx& c, int g) {
  if (!c.haveStepped[g]) {
    c.stepped[g] = nextState(c.memory, c.elapsedDays, g, c.retrievability);
    c.haveStepped[g] = true;
  }
  return c.stepped[g];
}

// Full candidate.nextInterval for a grade: learning-step wrapper around fuzz around model.
double gradeInterval(Ctx& c, int g) {
  const Memory& m = stepFor(c, g);
  auto model = [&]() { return fuzz(modelNextInterval(m, c.retention), c.elapsedDays, c.seed); };
  double interval;
  const bool has = c.resolved.hasStepInterval[g];
  const double step = c.resolved.stepInterval[g];
  if (g == Again) {
    interval = !has ? model() : ALLOW_MODEL_OVERRIDE ? std::max(step, std::min(model(), GRADUATING_INTERVAL)) : step;
  } else if (!has) {
    interval = c.resolved.hasSteps ? std::max(model(), GRADUATING_INTERVAL) : model();
  } else {
    interval = ALLOW_MODEL_OVERRIDE ? std::max(step, model()) : step;
  }
  c.resolved.overridesStep[g] = has && interval != step;
  return interval;
}

double toSeconds(double days) { return jsRound(days * SECONDS_PER_DAY); }

double scheduleDay(const double* candidates, int count) {
  bool haveScheduled = false;
  double scheduledDay = 0;
  double subDay = candidates[0];
  for (int i = 0; i < count; i++) {
    const double interval = candidates[i];
    if (interval >= 1) {
      scheduledDay = std::min(haveScheduled ? std::max(interval, scheduledDay + 1) : interval, MAXIMUM_INTERVAL);
      haveScheduled = true;
    } else if (!haveScheduled && i > 0) {
      const double raised = std::max(interval, subDay);
      subDay = (subDay > 0 && toSeconds(interval) >= toSeconds(candidates[i - 1]) && toSeconds(raised) == toSeconds(subDay))
                   ? subDay + 1 / SECONDS_PER_DAY
                   : raised;
    }
  }
  return haveScheduled ? scheduledDay : subDay;
}

Ctx prepare(const CardState& card, const std::string& seedId, int64_t nowMs, double retention) {
  Ctx c;
  c.retention = clampd(retention, DR_MIN, DR_MAX);
  c.memory = {card.stability, card.stabilityFast, card.difficulty};
  if (card.state == 0) {
    c.elapsedDays = 0;
  } else {
    const int64_t prev = card.lastReviewMs >= 0 ? card.lastReviewMs : card.dueMs;
    c.elapsedDays = std::max(0.0, static_cast<double>(nowMs - prev) / MS_PER_DAY);
  }
  c.retrievability = forgettingCurve(c.memory, c.elapsedDays);
  c.seed = seedId + std::to_string(card.reps + 1);
  c.resolved = resolveLearningSteps(card.state, card.learningStep);
  return c;
}

double scheduledFor(Ctx& c, int grade) {
  // terminal
  const double terminal = gradeInterval(c, grade);
  // monotonic
  double cands[4];
  int n = 0;
  for (int g = Again; g < grade; g++) cands[n++] = gradeInterval(c, g);
  cands[n++] = terminal;
  double sd = scheduleDay(cands, n);
  // maximum interval
  sd = std::min(sd, MAXIMUM_INTERVAL);
  // learning steps keep the exact step unless the model overrode it
  if (c.resolved.hasStepInterval[grade] && !c.resolved.overridesStep[grade]) sd = c.resolved.stepInterval[grade];
  return sd;
}

}  // namespace

CardState newCard(int64_t createdMs) {
  CardState c;
  c.dueMs = createdMs;
  return c;
}

CardState review(const CardState& card, const std::string& seedId, int grade, int64_t nowMs, double retention) {
  grade = std::min(std::max(grade, 1), 4);
  Ctx c = prepare(card, seedId, nowMs, retention);
  const double sd = scheduledFor(c, grade);
  const Memory& m = stepFor(c, grade);

  CardState out = card;
  out.stability = m.s;
  out.stabilityFast = m.sFast;
  out.difficulty = m.d;
  out.state = 2;
  out.scheduleStatusCode = 2;
  out.learningStep = 0;
  const Step& step = c.resolved.steps[grade];
  const bool stepDefined = c.resolved.hasStepInterval[grade];
  if ((grade == Again && c.resolved.overridesStep[Again]) || sd < GRADUATING_INTERVAL) {
    if (card.state == 0) {
      out.state = 1;
    } else if (card.state == 2) {
      out.state = 3;
    } else {
      out.state = card.state;
    }
    out.scheduleStatusCode = 1;
    if (stepDefined) out.learningStep = std::max(0, step.nextStep);
  }
  out.scheduledDays = std::max(0.0, sd);
  out.reps = card.reps + 1;
  out.lapses = (grade == Again && card.state == 2) ? card.lapses + 1 : card.lapses;
  // new Date(ms) truncates the fractional millisecond.
  out.dueMs = nowMs + static_cast<int64_t>(std::trunc(out.scheduledDays * MS_PER_DAY));
  out.lastReviewMs = nowMs;
  return out;
}

CardState replay(const std::string& cardId, int64_t createdMs, std::vector<LogEntry> logs) {
  std::stable_sort(logs.begin(), logs.end(), [](const LogEntry& a, const LogEntry& b) { return a.atMs < b.atMs; });
  CardState c = newCard(createdMs);
  for (const auto& l : logs) c = review(c, cardId, l.grade, l.atMs);
  return c;
}

void previewDays(const CardState& card, const std::string& seedId, int64_t nowMs, double out[4], double retention) {
  for (int g = 1; g <= 4; g++) out[g - 1] = review(card, seedId, g, nowMs, retention).scheduledDays;
}

std::string formatInterval(double days) {
  char buf[24];
  const double ms = days * MS_PER_DAY;
  constexpr double MIN = 60000, HOUR = 3600000;
  if (ms < HOUR) {
    snprintf(buf, sizeof(buf), "%d min", static_cast<int>(std::max(1.0, jsRound(ms / MIN))));
  } else if (ms < MS_PER_DAY) {
    snprintf(buf, sizeof(buf), "%d h", static_cast<int>(jsRound(ms / HOUR)));
  } else if (days < 31) {
    snprintf(buf, sizeof(buf), "%d d", static_cast<int>(jsRound(days)));
  } else if (days < 365) {
    const int months = static_cast<int>(jsRound(days / 30));
    if (months == 1) return "1 m\xC3\xAAs";
    snprintf(buf, sizeof(buf), "%d meses", months);
  } else {
    const int tenths = static_cast<int>(jsRound(days / 365 * 10));
    const char* unit = tenths < 20 ? "ano" : "anos";
    if (tenths % 10 == 0) {
      snprintf(buf, sizeof(buf), "%d %s", tenths / 10, unit);
    } else {
      snprintf(buf, sizeof(buf), "%d,%d %s", tenths / 10, tenths % 10, unit);
    }
  }
  return buf;
}

}  // namespace fsrs
}  // namespace linguetta
