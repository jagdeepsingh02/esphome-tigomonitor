// Host test for components/tigo_monitor/tigo_panel_alerts.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/panel_alerts_test.cpp -o /tmp/panel_alerts_test && /tmp/panel_alerts_test

#include "tigo_panel_alerts.h"

#include <cstdio>
#include <cstring>

using namespace esphome::tigo_monitor::panel_alerts;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const Thresholds T;  // defaults = the web UI's 30 % / 60 % / 5 W

static void test_median() {
  std::puts("median ignores panels at or below 5 W");
  float a[] = {300, 0, 310, 2, 290};
  CHECK(median_above_floor(a, 5) == 300.0f);
  float b[] = {100, 200, 300, 400};
  CHECK(median_above_floor(b, 4) == 250.0f);
  float c[] = {0, 5, 3};
  CHECK(median_above_floor(c, 3) == 0.0f);
  CHECK(median_above_floor(c, 0) == 0.0f);
}

static void test_bands() {
  std::puts("bands match the web UI: <30 % low, <60 % underperforming");
  CHECK(evaluate(false, 300, 300, true, Reason::OK, T) == Reason::OK);
  CHECK(evaluate(false, 185, 300, true, Reason::OK, T) == Reason::OK);           // 61.7 %
  CHECK(evaluate(false, 170, 300, true, Reason::OK, T) == Reason::UNDERPERFORMING);  // 56.7 %
  CHECK(evaluate(false, 80, 300, true, Reason::OK, T) == Reason::LOW_OUTPUT);           // 26.7 %
  CHECK(evaluate(false, 0, 300, true, Reason::OK, T) == Reason::LOW_OUTPUT);
}

static void test_stale_wins() {
  std::puts("stale outranks everything");
  CHECK(evaluate(true, 300, 300, true, Reason::OK, T) == Reason::STALE);
  CHECK(evaluate(true, 0, 0, true, Reason::OK, T) == Reason::STALE);
}

static void test_flat_string() {
  std::puts("a flat string is string_down only while the site produces");
  CHECK(evaluate(false, 0, 0, true, Reason::OK, T) == Reason::STRING_DOWN);
  CHECK(evaluate(false, 0, 0, false, Reason::OK, T) == Reason::OK);   // night / dawn / snow
  CHECK(evaluate(false, 4, 4.5f, false, Reason::OK, T) == Reason::OK);
}

static void test_hysteresis() {
  std::puts("a flagged panel must clear the threshold by the hysteresis margin");
  // Underperforming at 55 %: 65 % is not enough to clear (needs 70 %), 71 % is.
  CHECK(evaluate(false, 195, 300, true, Reason::UNDERPERFORMING, T) == Reason::UNDERPERFORMING);
  CHECK(evaluate(false, 213, 300, true, Reason::UNDERPERFORMING, T) == Reason::OK);
  // Low: 35 % stays low (needs 40 %), 45 % steps up to underperforming, not OK.
  CHECK(evaluate(false, 105, 300, true, Reason::LOW_OUTPUT, T) == Reason::LOW_OUTPUT);
  CHECK(evaluate(false, 135, 300, true, Reason::LOW_OUTPUT, T) == Reason::UNDERPERFORMING);
  // Without a prior flag the plain thresholds apply.
  CHECK(evaluate(false, 195, 300, true, Reason::OK, T) == Reason::OK);
}

static void test_custom_thresholds() {
  std::puts("thresholds come from config");
  Thresholds t;
  t.low = 0.5f;
  t.underperform = 0.8f;
  t.hysteresis = 0.0f;
  CHECK(evaluate(false, 140, 200, true, Reason::OK, t) == Reason::UNDERPERFORMING);  // 70 %
  CHECK(evaluate(false, 90, 200, true, Reason::OK, t) == Reason::LOW_OUTPUT);               // 45 %
}

static void test_reason_strings() {
  std::puts("reason strings are the documented values");
  CHECK(std::strcmp(reason_str(Reason::OK), "ok") == 0);
  CHECK(std::strcmp(reason_str(Reason::STALE), "stale") == 0);
  CHECK(std::strcmp(reason_str(Reason::STRING_DOWN), "string_down") == 0);
  CHECK(std::strcmp(reason_str(Reason::LOW_OUTPUT), "low") == 0);
  CHECK(std::strcmp(reason_str(Reason::UNDERPERFORMING), "underperforming") == 0);
}

int main() {
  test_median();
  test_bands();
  test_stale_wins();
  test_flat_string();
  test_hysteresis();
  test_custom_thresholds();
  test_reason_strings();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
