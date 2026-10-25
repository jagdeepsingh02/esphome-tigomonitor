#pragma once

// Per-panel alert rules, published to Home Assistant as a problem binary
// sensor plus a reason.
//
// The production checks follow renderAlerts() in
// components/tigo_server/web/app.html: a panel is compared with the median of
// its string (panels under kMedianFloorW excluded, as in the UI), below 30 %
// is low and below 60 % underperforming. Two checks deliberately differ from
// the UI, because a Home Assistant alert notifies someone and a dashboard tile
// does not:
//
//  * stale — the UI flags any panel silent past stale_timeout. Every panel
//    goes silent at dusk, 10 min later they are all stale, and night mode
//    only starts after night_mode_timeout (60 min) of total silence: a nightly
//    50-minute alert on every panel. Here a silent panel is only a fault while
//    other panels of its string are still reporting *and producing* (median
//    at least min_string_power). Panels on one roof face wake and sleep
//    together, so orientation needs no configuration; and the few that go
//    quiet minutes before their string-mates at dusk aren't flagged, because
//    those string-mates are down to a watt or two by then.
//  * string_down — the UI flags a flat string whenever any other string makes
//    5 W. On an east/west roof that is the west string every morning. Here the
//    other string must be producing properly: kStrongStringW per panel. A
//    tripped string reads ~0 W; a face turned away from the sun still makes
//    well over 5 W by the time another face reaches 100 W.
//
// Every check that lacks the panels to compare against (a one-panel system,
// a one-panel string) stays at ok: a missed alert, never a false one.
//
// Header-only and free of ESPHome dependencies so the rules can be exercised
// on the host (tests/host/panel_alerts_test.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace tigo_monitor {
namespace panel_alerts {

// Worse reasons first; the numeric order is the priority order.
enum class Reason : uint8_t { OK = 0, STALE, STRING_DOWN, LOW_OUTPUT, UNDERPERFORMING };

inline const char *reason_str(Reason r) {
  switch (r) {
    case Reason::STALE: return "stale";
    case Reason::STRING_DOWN: return "string_down";
    case Reason::LOW_OUTPUT: return "low";
    case Reason::UNDERPERFORMING: return "underperforming";
    default: return "ok";
  }
}

// Panels at or below this never count toward a median, so a few dead or
// shaded-to-zero panels can't drag the reference down (app.html: p > 5).
static const float kMedianFloorW = 5.0f;
// A string is only "down" while another string's median panel makes this
// much. Fixed rather than configurable: per panel, so independent of system
// size; panels too small to ever reach it just never raise string_down.
static const float kStrongStringW = 100.0f;

struct Thresholds {
  float low = 0.30f;             // below this fraction of the median: LOW_OUTPUT
  float underperform = 0.60f;    // below this: UNDERPERFORMING
  float min_string_w = 5.0f;     // string median below this: the string is flat
  float hysteresis = 0.10f;      // extra margin needed to clear LOW_OUTPUT / UNDERPERFORMING
};

// Median of the first n values above kMedianFloorW. Reorders `v` in place
// (the caller passes a scratch buffer). 0 when no value qualifies.
inline float median_above_floor(float *v, size_t n) {
  size_t m = 0;
  for (size_t i = 0; i < n; ++i)
    if (v[i] > kMedianFloorW) v[m++] = v[i];
  if (m == 0) return 0.0f;
  std::sort(v, v + m);
  return (m % 2) ? v[m / 2] : (v[m / 2 - 1] + v[m / 2]) * 0.5f;
}

// One panel's inputs and result.
struct Panel {
  int group = -1;          // string index, -1 = no string label
  bool silent = false;     // stale, or never reported (past the boot grace)
  float power_w = 0.0f;
  Reason prev = Reason::OK;  // last published, for hysteresis
  Reason out = Reason::OK;
};

struct GroupStat {
  float median = 0.0f;  // of reporting panels, above kMedianFloorW
  int size = 0;
  int reporting = 0;    // panels not silent
};

// One panel, given what it is compared against. `peer_reporting`: another
// panel of its group is still reporting, and `peer_median` is what that group
// produces. `best_median`: the strongest string's median panel power.
inline Reason evaluate(bool silent, bool peer_reporting, float peer_median, float power_w,
                       float group_median, float best_median, Reason prev, const Thresholds &t) {
  if (silent && peer_reporting && peer_median >= t.min_string_w) return Reason::STALE;
  if (silent || group_median < t.min_string_w) {
    // Silent along with its whole group, or the group is flat. A fault only
    // if another string is producing properly; otherwise it's night, dawn,
    // dusk, snow, cloud, or a face turned away from the sun.
    return best_median >= kStrongStringW ? Reason::STRING_DOWN : Reason::OK;
  }
  float ratio = power_w / group_median;
  float low = t.low + (prev == Reason::LOW_OUTPUT ? t.hysteresis : 0.0f);
  float under = t.underperform +
                ((prev == Reason::LOW_OUTPUT || prev == Reason::UNDERPERFORMING) ? t.hysteresis : 0.0f);
  if (ratio < low) return Reason::LOW_OUTPUT;
  if (ratio < under) return Reason::UNDERPERFORMING;
  return Reason::OK;
}

// Evaluate every panel, setting Panel::out. `stats` and `scratch` are work
// buffers the caller keeps between calls (resize/clear only, so steady state
// allocates nothing). A panel alone on its string — or with no string label —
// takes its peers from the whole site for `stale`; its string median is then
// its own power (one-panel string) or the site median (no label).
template<class PanelVec, class StatVec, class FloatVec>
void evaluate_all(PanelVec &panels, size_t group_count, StatVec &stats, FloatVec &scratch,
                  const Thresholds &t) {
  const size_t site = group_count;  // stats[site] covers every panel
  stats.assign(group_count + 1, GroupStat{});
  for (const auto &p : panels) {
    if (p.group >= 0 && (size_t) p.group < group_count) {
      stats[p.group].size++;
      if (!p.silent) stats[p.group].reporting++;
    }
    stats[site].size++;
    if (!p.silent) stats[site].reporting++;
  }
  for (size_t g = 0; g <= group_count; ++g) {
    scratch.clear();
    for (const auto &p : panels) {
      bool member = (g == site) || p.group == (int) g;
      if (member && !p.silent) scratch.push_back(p.power_w);
    }
    stats[g].median = median_above_floor(scratch.data(), scratch.size());
  }
  float best = 0.0f;
  for (size_t g = 0; g < group_count; ++g) best = std::max(best, stats[g].median);
  if (group_count == 0) best = stats[site].median;

  for (auto &p : panels) {
    bool own_group = p.group >= 0 && (size_t) p.group < group_count;
    const GroupStat &peers = (own_group && stats[p.group].size > 1) ? stats[p.group] : stats[site];
    bool peer_reporting = (peers.reporting - (p.silent ? 0 : 1)) > 0;
    float median = own_group ? stats[p.group].median : stats[site].median;
    p.out = evaluate(p.silent, peer_reporting, peers.median, p.power_w, median, best, p.prev, t);
  }
}

}  // namespace panel_alerts
}  // namespace tigo_monitor
}  // namespace esphome
