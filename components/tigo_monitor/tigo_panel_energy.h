#pragma once

// Per-panel energy: one monotonic lifetime counter per physical panel, with
// today / week / month / year derived from it.
//
// Each period stores the lifetime value at the moment the period began (its
// baseline) plus a key naming the period. The period's energy is lifetime
// minus baseline, and a rollover just moves the baseline up to lifetime. One
// counter and four subtractions means a missed save can lose energy but can
// never make "today" exceed "month", and nothing ever needs resetting to zero.
//
// Header-only and free of ESPHome dependencies so the arithmetic — the
// trapezoid, the gap guard, the week-start and year-boundary cases — can be
// exercised on the host (tests/host/panel_energy_test.cpp).

#include <cmath>
#include <cstdint>

namespace esphome {
namespace tigo_monitor {
namespace panel_energy {

enum Period { DAY = 0, WEEK, MONTH, YEAR, PERIOD_COUNT };

// What goes to flash, one per panel. Fixed layout: version first so a future
// change can be told apart from an old record rather than misread.
struct Record {
  static const uint32_t kVersion = 1;
  uint32_t version = kVersion;
  float lifetime_kwh = 0.0f;
  float baseline_kwh[PERIOD_COUNT] = {0, 0, 0, 0};
  uint32_t period_key[PERIOD_COUNT] = {0, 0, 0, 0};  // 0 = period not started yet
};

struct State {
  Record rec;
  // Runtime only.
  float last_power_w = 0.0f;
  uint32_t last_ms = 0;  // millis() of the last integrated frame; 0 = none yet
  bool loaded = false;   // NVS read attempted
  bool dirty = false;    // rec differs from what was last saved
};

// Integrate one frame. Trapezoid between this panel's previous frame and this
// one. An interval longer than max_gap_ms is not integrated: the panel was
// silent (stale, night, reboot), and crediting the whole gap at either
// endpoint's power would invent energy. Negative or non-finite power counts
// as zero. Returns the kWh added.
inline float integrate(State &s, float power_w, uint32_t now_ms, uint32_t max_gap_ms) {
  if (!std::isfinite(power_w) || power_w < 0.0f) power_w = 0.0f;
  float added = 0.0f;
  if (s.last_ms != 0) {
    uint32_t dt = now_ms - s.last_ms;  // unsigned: survives millis() wrap
    if (dt > 0 && dt <= max_gap_ms) {
      added = (s.last_power_w + power_w) * 0.5f * (dt / 3600000.0f) / 1000.0f;
      s.rec.lifetime_kwh += added;
      s.dirty = true;
    }
  }
  s.last_power_w = power_w;
  s.last_ms = now_ms;
  return added;
}

inline float period_kwh(const Record &r, Period p) {
  float v = r.lifetime_kwh - r.baseline_kwh[p];
  return v > 0.0f ? v : 0.0f;
}

inline bool is_leap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

// Period keys from a local calendar date. day_of_week is ESPTime's
// (sunday = 1 .. saturday = 7). The week key is YYYYDDD of the week's first
// day, computed on the calendar rather than by subtracting seconds from a
// timestamp, so a DST change inside the week cannot shift it by a day.
struct Keys {
  uint32_t key[PERIOD_COUNT];
};

inline Keys keys_for(int year, int month, int day_of_month, int day_of_year, int day_of_week,
                     bool week_starts_monday) {
  Keys k;
  k.key[DAY] = (uint32_t) (year * 10000 + month * 100 + day_of_month);
  k.key[MONTH] = (uint32_t) (year * 100 + month);
  k.key[YEAR] = (uint32_t) year;

  // Days since the start of the week: monday=2 is day 0 of a Monday week.
  int back = week_starts_monday ? (day_of_week + 5) % 7 : (day_of_week - 1) % 7;
  int wy = year;
  int wd = day_of_year - back;
  if (wd < 1) {  // week began in the previous year
    wy -= 1;
    wd += is_leap(wy) ? 366 : 365;
  }
  k.key[WEEK] = (uint32_t) (wy * 1000 + wd);
  return k;
}

// Move each period whose key changed onto the new period. A period that has
// never started (key 0) starts at baseline 0: everything this panel has
// produced since its record was created is inside the current period, which
// is exactly what a freshly created record has seen. Returns true if any
// period rolled.
inline bool roll(State &s, const Keys &k) {
  bool rolled = false;
  for (int p = 0; p < PERIOD_COUNT; ++p) {
    if (s.rec.period_key[p] == k.key[p]) continue;
    s.rec.baseline_kwh[p] = (s.rec.period_key[p] == 0) ? 0.0f : s.rec.lifetime_kwh;
    s.rec.period_key[p] = k.key[p];
    rolled = true;
  }
  if (rolled) s.dirty = true;
  return rolled;
}

}  // namespace panel_energy
}  // namespace tigo_monitor
}  // namespace esphome
