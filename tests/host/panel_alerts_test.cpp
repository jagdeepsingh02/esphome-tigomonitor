// Host test for components/tigo_monitor/tigo_panel_alerts.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/panel_alerts_test.cpp -o /tmp/panel_alerts_test && /tmp/panel_alerts_test

#include "tigo_panel_alerts.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace esphome::tigo_monitor::panel_alerts;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const Thresholds T;  // defaults: 30 % / 60 % / 5 W / 10 %

// Panel shorthand: group, watts; silent() for a panel that stopped reporting.
static Panel on(int group, float w) {
  Panel p;
  p.group = group;
  p.power_w = w;
  return p;
}
static Panel silent(int group) {
  Panel p;
  p.group = group;
  p.silent = true;
  return p;
}

static std::vector<Panel> run(std::vector<Panel> panels, size_t groups) {
  std::vector<GroupStat> stats;
  std::vector<float> scratch;
  evaluate_all(panels, groups, stats, scratch, T);
  return panels;
}

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
  // Five healthy panels keep the median at 300 W.
  auto r = run({on(0, 300), on(0, 300), on(0, 300), on(0, 300), on(0, 300),
                on(0, 185), on(0, 170), on(0, 80), on(0, 0)}, 1);
  CHECK(r[0].out == Reason::OK);
  CHECK(r[5].out == Reason::OK);               // 61.7 % of the 300 W median
  CHECK(r[6].out == Reason::UNDERPERFORMING);  // 56.7 %
  CHECK(r[7].out == Reason::LOW_OUTPUT);       // 26.7 %
  CHECK(r[8].out == Reason::LOW_OUTPUT);
}

static void test_dead_optimizer_at_noon() {
  std::puts("a panel silent while its string reports is stale");
  auto r = run({on(0, 300), on(0, 310), silent(0), on(0, 305)}, 1);
  CHECK(r[2].out == Reason::STALE);
  CHECK(r[0].out == Reason::OK);
}

static void test_dusk_is_not_stale() {
  std::puts("dusk: every panel silent, no alert (the 50-minute window before night mode)");
  auto r = run({silent(0), silent(0), silent(1), silent(1)}, 2);
  for (const auto &p : r) CHECK(p.out == Reason::OK);
}

static void test_dusk_stragglers() {
  std::puts("dusk: early sleepers while string-mates trickle a few watts — no alert");
  auto r = run({silent(0), on(0, 2), silent(0), on(0, 4), silent(1), on(1, 3)}, 2);
  for (const auto &p : r) CHECK(p.out == Reason::OK);
  // Dawn is the mirror image: early risers at a few watts, the rest asleep.
  r = run({on(0, 3), silent(0), silent(0), on(1, 1), silent(1)}, 2);
  for (const auto &p : r) CHECK(p.out == Reason::OK);
}

static void test_east_west_morning() {
  std::puts("east/west morning: west in diffuse light is not string_down");
  // East at 150 W, west at 20 W diffuse: west flat? No — 20 W > 5 W, and
  // west panels are compared only with each other.
  auto r = run({on(0, 150), on(0, 155), on(1, 20), on(1, 22)}, 2);
  for (const auto &p : r) CHECK(p.out == Reason::OK);
  // West still under 5 W while east is weak (60 W): not strong enough to judge.
  r = run({on(0, 60), on(0, 62), on(1, 3), on(1, 2)}, 2);
  for (const auto &p : r) CHECK(p.out == Reason::OK);
}

static void test_tripped_string() {
  std::puts("a flat string while another makes >= 100 W per panel is string_down");
  auto r = run({on(0, 300), on(0, 310), on(1, 0), on(1, 1)}, 2);
  CHECK(r[2].out == Reason::STRING_DOWN && r[3].out == Reason::STRING_DOWN);
  CHECK(r[0].out == Reason::OK);
  // A whole string gone silent at noon is string_down too, not stale.
  r = run({on(0, 300), on(0, 310), silent(1), silent(1)}, 2);
  CHECK(r[2].out == Reason::STRING_DOWN && r[3].out == Reason::STRING_DOWN);
}

static void test_one_panel_system() {
  std::puts("1 panel: nothing to compare with — never alerts");
  CHECK(run({silent(0)}, 1)[0].out == Reason::OK);
  CHECK(run({on(0, 0)}, 1)[0].out == Reason::OK);
  CHECK(run({on(0, 300)}, 1)[0].out == Reason::OK);
  CHECK(run({silent(-1)}, 0)[0].out == Reason::OK);  // no string label
}

static void test_two_panels_one_string() {
  std::puts("2 panels, 1 string: stale and low work, string_down can't");
  CHECK(run({on(0, 300), silent(0)}, 1)[1].out == Reason::STALE);
  // Median of two is their mean: 300 and 60 -> 180, 60/180 = 33 %: underperforming.
  CHECK(run({on(0, 300), on(0, 60)}, 1)[1].out == Reason::UNDERPERFORMING);
  auto r = run({on(0, 1), on(0, 0)}, 1);  // both flat, no other string
  CHECK(r[0].out == Reason::OK && r[1].out == Reason::OK);
}

static void test_two_panels_two_strings() {
  std::puts("2 panels, 1 per string: stale and string_down work, low can't");
  // Alone on its string: peers come from the whole site.
  CHECK(run({on(0, 300), silent(1)}, 2)[1].out == Reason::STALE);  // silent while the other produces
  CHECK(run({on(0, 300), on(1, 0)}, 2)[1].out == Reason::STRING_DOWN);
  CHECK(run({on(0, 300), on(1, 60)}, 2)[1].out == Reason::OK);  // own median is itself
  CHECK(run({on(0, 50), silent(1)}, 2)[1].out == Reason::STALE);    // 50 W is producing
  CHECK(run({on(0, 3), silent(1)}, 2)[1].out == Reason::OK);        // dusk: other barely on
}

static void test_no_string_labels() {
  std::puts("no string labels: everything compared with the site");
  auto r = run({on(-1, 300), on(-1, 305), on(-1, 80), silent(-1)}, 0);
  CHECK(r[2].out == Reason::LOW_OUTPUT);
  CHECK(r[3].out == Reason::STALE);
}

static void test_hysteresis() {
  std::puts("a flagged panel must clear the threshold by the hysteresis margin");
  auto with_prev = [](float w, Reason prev) {
    std::vector<Panel> v = {on(0, 300), on(0, 300), on(0, w)};
    v[2].prev = prev;
    return run(v, 1)[2].out;
  };
  // Median 300. Underperforming: 65 % doesn't clear (needs 70 %), 71 % does.
  CHECK(with_prev(195, Reason::UNDERPERFORMING) == Reason::UNDERPERFORMING);
  CHECK(with_prev(213, Reason::UNDERPERFORMING) == Reason::OK);
  // Low: 35 % stays low (needs 40 %), 45 % steps up to underperforming.
  CHECK(with_prev(105, Reason::LOW_OUTPUT) == Reason::LOW_OUTPUT);
  CHECK(with_prev(135, Reason::LOW_OUTPUT) == Reason::UNDERPERFORMING);
  CHECK(with_prev(195, Reason::OK) == Reason::OK);
}

static void test_custom_thresholds() {
  std::puts("thresholds come from config");
  Thresholds t;
  t.low = 0.5f;
  t.underperform = 0.8f;
  t.hysteresis = 0.0f;
  CHECK(evaluate(false, true, 200, 140, 200, 200, Reason::OK, t) == Reason::UNDERPERFORMING);  // 70 %
  CHECK(evaluate(false, true, 200, 90, 200, 200, Reason::OK, t) == Reason::LOW_OUTPUT);         // 45 %
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
  test_dead_optimizer_at_noon();
  test_dusk_is_not_stale();
  test_dusk_stragglers();
  test_east_west_morning();
  test_tripped_string();
  test_one_panel_system();
  test_two_panels_one_string();
  test_two_panels_two_strings();
  test_no_string_labels();
  test_hysteresis();
  test_custom_thresholds();
  test_reason_strings();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
