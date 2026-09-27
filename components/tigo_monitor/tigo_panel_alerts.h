#pragma once

// Per-panel alert rules, published to Home Assistant as a problem binary
// sensor plus a reason.
//
// These mirror renderAlerts() in components/tigo_server/web/app.html, so the
// dashboard's Alerts list and Home Assistant agree about which panel is in
// trouble: a panel is compared with the median of its string (panels under
// kMedianFloorW excluded, as in the UI), and a string that is flat while the
// rest of the site produces is a tripped string, not nightfall. Keep the two
// in step.
//
// Header-only and free of ESPHome dependencies so the thresholds and the
// hysteresis can be exercised on the host (tests/host/panel_alerts_test.cpp).

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

struct Thresholds {
  float low = 0.30f;             // below this fraction of the median: LOW_OUTPUT
  float underperform = 0.60f;    // below this: UNDERPERFORMING
  float min_string_w = 5.0f;     // median below this: no peer comparison
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

// One panel. `group_median` is its string's median_above_floor();
// `site_active` whether any group's median reaches min_string_w. `prev` is
// the reason published last time — a panel already flagged must climb
// `hysteresis` above a threshold to clear it, so it doesn't flap on the line.
inline Reason evaluate(bool stale, float power_w, float group_median, bool site_active, Reason prev,
                       const Thresholds &t) {
  if (stale) return Reason::STALE;
  if (group_median < t.min_string_w) {
    // The whole string is flat. Only a fault if the rest of the site is
    // producing; otherwise it's dawn, dusk, snow or heavy cloud.
    return site_active ? Reason::STRING_DOWN : Reason::OK;
  }
  float ratio = power_w / group_median;
  float low = t.low + (prev == Reason::LOW_OUTPUT ? t.hysteresis : 0.0f);
  float under = t.underperform +
                ((prev == Reason::LOW_OUTPUT || prev == Reason::UNDERPERFORMING) ? t.hysteresis : 0.0f);
  if (ratio < low) return Reason::LOW_OUTPUT;
  if (ratio < under) return Reason::UNDERPERFORMING;
  return Reason::OK;
}

}  // namespace panel_alerts
}  // namespace tigo_monitor
}  // namespace esphome
