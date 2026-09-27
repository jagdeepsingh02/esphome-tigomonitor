// Host test for components/tigo_monitor/tigo_panel_energy.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/panel_energy_test.cpp -o /tmp/panel_energy_test && /tmp/panel_energy_test

#include "tigo_panel_energy.h"

#include <cmath>
#include <cstdio>

using namespace esphome::tigo_monitor::panel_energy;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static const uint32_t GAP = 600000;  // 10 min, the default stale_timeout

static void test_constant_power_hour() {
  std::puts("300 W for an hour in 5 s frames = 0.3 kWh");
  State s;
  uint32_t t = 1000;
  for (int i = 0; i <= 720; ++i, t += 5000) integrate(s, 300.0f, t, GAP);
  CHECK(near(s.rec.lifetime_kwh, 0.3f, 1e-3f));
}

static void test_trapezoid() {
  std::puts("a ramp is integrated as a trapezoid");
  State s;
  integrate(s, 0.0f, 1000, GAP);
  float added = integrate(s, 360.0f, 1000 + 10000, GAP);  // 0 -> 360 W over 10 s
  CHECK(near(added, 180.0f * 10.0f / 3600.0f / 1000.0f, 1e-7f));
}

static void test_gap_is_not_integrated() {
  std::puts("a gap longer than max_gap is skipped, integration resumes after it");
  State s;
  integrate(s, 400.0f, 1000, GAP);
  CHECK(integrate(s, 400.0f, 1000 + GAP + 1, GAP) == 0.0f);  // overnight / outage
  CHECK(s.rec.lifetime_kwh == 0.0f);
  CHECK(integrate(s, 400.0f, 1000 + GAP + 1 + 5000, GAP) > 0.0f);
}

static void test_first_frame_adds_nothing() {
  std::puts("the first frame after boot only sets the starting point");
  State s;
  CHECK(integrate(s, 500.0f, 123456, GAP) == 0.0f);
  CHECK(!s.dirty);
}

static void test_bad_power_counts_as_zero() {
  std::puts("negative and NaN power count as zero");
  State s;
  integrate(s, -50.0f, 1000, GAP);
  integrate(s, NAN, 6000, GAP);
  integrate(s, -1.0f, 11000, GAP);
  CHECK(s.rec.lifetime_kwh == 0.0f);
}

static void test_millis_wrap() {
  std::puts("integration survives millis() wrapping");
  State s;
  integrate(s, 360.0f, 0xFFFFF000u, GAP);
  float added = integrate(s, 360.0f, 0x00000388u, GAP);  // 5000 ms later
  CHECK(near(added, 360.0f * 5.0f / 3600.0f / 1000.0f, 1e-7f));
}

static void test_week_keys() {
  std::puts("week key: Monday vs Sunday start");
  // 2026-09-27 is a Sunday (dow 1), day_of_year 270.
  Keys mon = keys_for(2026, 9, 27, 270, 1, true);
  Keys sun = keys_for(2026, 9, 27, 270, 1, false);
  CHECK(mon.key[WEEK] == 2026264);  // Monday 2026-09-21 = doy 264
  CHECK(sun.key[WEEK] == 2026270);  // the Sunday itself
  // Monday 2026-09-28 (dow 2, doy 271) starts a new Monday week.
  CHECK(keys_for(2026, 9, 28, 271, 2, true).key[WEEK] == 2026271);
  CHECK(keys_for(2026, 9, 28, 271, 2, false).key[WEEK] == 2026270);
  CHECK(mon.key[DAY] == 20260927 && mon.key[MONTH] == 202609 && mon.key[YEAR] == 2026);
}

static void test_week_across_new_year() {
  std::puts("a week that began last year keys to last year's date");
  // 2027-01-01 is a Friday (dow 6), doy 1. Its Monday week began 2026-12-28 = doy 362.
  CHECK(keys_for(2027, 1, 1, 1, 6, true).key[WEEK] == 2026362);
  // Leap year: 2029-01-01 is a Monday; 2028-12-31 (Sunday, doy 366) belongs to the week of 2028-12-25 (doy 360).
  CHECK(keys_for(2028, 12, 31, 366, 1, true).key[WEEK] == 2028360);
}

static void test_rollover() {
  std::puts("rollover: today restarts, week/month/year keep counting");
  State s;
  Keys d1 = keys_for(2026, 9, 27, 270, 1, true);  // Sunday
  roll(s, d1);  // fresh record: all periods start at 0
  s.rec.lifetime_kwh = 1.5f;
  CHECK(near(period_kwh(s.rec, DAY), 1.5f) && near(period_kwh(s.rec, YEAR), 1.5f));

  Keys d2 = keys_for(2026, 9, 28, 271, 2, true);  // Monday: new day AND new week
  CHECK(roll(s, d2));
  CHECK(period_kwh(s.rec, DAY) == 0.0f);
  CHECK(period_kwh(s.rec, WEEK) == 0.0f);
  CHECK(near(period_kwh(s.rec, MONTH), 1.5f));
  s.rec.lifetime_kwh = 2.0f;
  CHECK(near(period_kwh(s.rec, DAY), 0.5f) && near(period_kwh(s.rec, MONTH), 2.0f));

  CHECK(!roll(s, d2));  // same day: no-op
  Keys oct = keys_for(2026, 10, 1, 274, 5, true);
  roll(s, oct);
  CHECK(period_kwh(s.rec, MONTH) == 0.0f && near(period_kwh(s.rec, YEAR), 2.0f));
}

static void test_loaded_record_rolls_from_lifetime() {
  std::puts("a record restored from flash rolls from its lifetime, not from zero");
  State s;
  s.rec.lifetime_kwh = 812.0f;
  Keys yesterday = keys_for(2026, 9, 26, 269, 7, true);
  for (int p = 0; p < PERIOD_COUNT; ++p) {
    s.rec.period_key[p] = yesterday.key[p];
    s.rec.baseline_kwh[p] = 800.0f;
  }
  roll(s, keys_for(2026, 9, 27, 270, 1, true));
  CHECK(period_kwh(s.rec, DAY) == 0.0f);
  CHECK(near(period_kwh(s.rec, WEEK), 12.0f));  // Sunday: same Monday week as Saturday
}

int main() {
  test_constant_power_hour();
  test_trapezoid();
  test_gap_is_not_integrated();
  test_first_frame_adds_nothing();
  test_bad_power_counts_as_zero();
  test_millis_wrap();
  test_week_keys();
  test_week_across_new_year();
  test_rollover();
  test_loaded_record_rolls_from_lifetime();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
