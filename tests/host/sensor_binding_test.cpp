// Host test for components/tigo_monitor/tigo_sensor_binding.h (#74).
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/sensor_binding_test.cpp -o /tmp/sensor_binding_test && /tmp/sensor_binding_test
//
// Drives the real Frame 27 handling from tigo_node_identity.h, then asks the
// binding which configured sensor key each short address publishes to — the
// question that decides which Home Assistant entity gets which panel's data.
// Barcodes here are made up; none belong to a real installation.

#include "tigo_node_identity.h"
#include "tigo_sensor_binding.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor;
using sensor_binding::KeyKind;

struct Node {  // the fields of NodeTableData the helpers touch
  std::string long_address, addr;
  int sensor_index = -1;
  bool is_persistent = true;
  std::string cca_label, cca_string_label, cca_mppt_label, cca_channel, cca_object_id;
  bool cca_validated = false;
  bool seen_in_frame27 = false;
};

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const std::string HELD;  // empty key: matches no sensor

struct Harness {
  std::vector<Node> table;
  std::vector<Node> pool;
  std::vector<std::string> barcode_keys;
  std::set<std::string> short_keys;
  bool renumbering_seen = false;
  std::map<std::string, std::string> binding;
  sensor_binding::BuildStats stats;

  void feed(const std::string &addr, const std::string &bc) {
    auto r = node_identity::apply_frame27_entry(
        table, pool, addr, bc, 64,
        [](const std::string &a, const std::string &l) {
          Node n;
          n.addr = a;
          n.long_address = l;
          return n;
        },
        [](const Node &) {});
    if (r.renumbered) renumbering_seen = true;
    rebuild();
  }
  void rebuild() {
    stats = sensor_binding::build(table, barcode_keys, short_keys, renumbering_seen, HELD, binding);
  }
  // What the firmware's binding_key_() returns: cached key, else the address.
  std::string key_for(const std::string &addr) const {
    auto it = binding.find(addr);
    return it != binding.end() ? it->second : addr;
  }
  // A table restored from flash at boot: pairings known, none confirmed yet.
  void restore(const std::string &addr, const std::string &bc) {
    Node n;
    n.addr = addr;
    n.long_address = bc;
    table.push_back(n);
  }
};

static void test_normalize() {
  std::puts("normalize: the forms a user will paste");
  std::string out;
  CHECK(sensor_binding::normalize("04C05B4000899AA0", out) == KeyKind::BARCODE && out == "04C05B4000899AA0");
  CHECK(sensor_binding::normalize("04c0-5b40-0089-9aa0", out) == KeyKind::BARCODE && out == "04C05B4000899AA0");
  CHECK(sensor_binding::normalize("\xE2\x80\xA6" "00899AA0", out) == KeyKind::BARCODE && out == "00899AA0");
  CHECK(sensor_binding::normalize("...00899AA0", out) == KeyKind::BARCODE && out == "00899AA0");
  CHECK(sensor_binding::normalize("899aa0", out) == KeyKind::BARCODE && out == "899AA0");
  CHECK(sensor_binding::normalize("0004", out) == KeyKind::SHORT_ADDRESS && out == "0004");
  CHECK(sensor_binding::normalize("9AA0", out) == KeyKind::SHORT_ADDRESS);  // 4 chars is always an address
  CHECK(sensor_binding::normalize("99AA0", out) == KeyKind::INVALID);       // 5: neither
  CHECK(sensor_binding::normalize("104C05B4000899AA0", out) == KeyKind::INVALID);  // 17
  CHECK(sensor_binding::normalize("device_1", out) == KeyKind::INVALID);
  CHECK(sensor_binding::normalize("", out) == KeyKind::INVALID);
}

static void test_barcode_matches() {
  std::puts("barcode_matches: tail of the long address only");
  const std::string la = "04C05B4000899AA0";
  CHECK(sensor_binding::barcode_matches(la, std::string("04C05B4000899AA0")));
  CHECK(sensor_binding::barcode_matches(la, std::string("00899AA0")));
  CHECK(sensor_binding::barcode_matches(la, std::string("899AA0")));
  CHECK(!sensor_binding::barcode_matches(la, std::string("9AA0")));       // too short
  CHECK(!sensor_binding::barcode_matches(la, std::string("04C05B")));     // head, not tail
  CHECK(!sensor_binding::barcode_matches(la, std::string("F04C05B4000899AA0")));  // longer
  CHECK(!sensor_binding::barcode_matches(std::string(""), std::string("899AA0")));
}

static void test_short_address_binding_unchanged() {
  std::puts("a 4-char address binding behaves exactly as before");
  Harness h;
  h.short_keys = {"0004"};
  h.restore("0004", "BC0000000000000A");
  h.rebuild();
  CHECK(h.key_for("0004") == "0004");
  CHECK(h.key_for("0009") == "0009");  // not in the table: falls back to itself
}

static void test_barcode_binding_resolves_at_boot() {
  std::puts("a barcode binding publishes from boot using the stored table");
  Harness h;
  h.barcode_keys = {"899AA0"};
  h.restore("0004", "04C05B4000899AA0");
  h.rebuild();
  CHECK(h.key_for("0004") == "899AA0");
  CHECK(h.stats.barcode_bound == 1 && h.stats.held == 0);
}

static void test_two_panels_swap_addresses() {
  std::puts("two panels swap addresses: each entity follows its barcode, the address one does not");
  Harness h;
  h.barcode_keys = {"00000A", "00000B"};
  h.feed("0002", "BC0000000000000A");
  h.feed("0003", "BC0000000000000B");
  CHECK(h.key_for("0002") == "00000A");
  CHECK(h.key_for("0003") == "00000B");

  // The gateway swaps them, announced one entry per frame.
  h.feed("0002", "BC0000000000000B");
  CHECK(h.renumbering_seen);
  CHECK(h.key_for("0002") == "00000B");  // confirmed: follows B
  // 0003 is gone as an alias of B; A has not been announced anywhere yet.
  CHECK(h.key_for("0003") != "00000A");
  CHECK(h.key_for("0003") != "00000B");

  h.feed("0003", "BC0000000000000A");
  CHECK(h.key_for("0002") == "00000B");
  CHECK(h.key_for("0003") == "00000A");
}

static void test_stale_pairing_is_held_after_a_renumbering() {
  std::puts("after a renumbering, a stored-but-unconfirmed pairing publishes nothing");
  Harness h;
  h.barcode_keys = {"00000A", "00000C"};
  // Restored from flash: A at 0002, C at 0004. Neither confirmed yet.
  h.restore("0002", "BC0000000000000A");
  h.restore("0004", "BC0000000000000C");
  h.rebuild();
  CHECK(h.key_for("0004") == "00000C");  // trusted until a renumbering is seen

  // Gateway renumbers: 0002 is now panel B. From here on the stored 0004 -> C
  // pairing is suspect — 0004 may now be A, or anyone.
  h.feed("0002", "BC0000000000000B");
  CHECK(h.key_for("0004") == HELD);
  CHECK(h.stats.held == 1);

  // The gateway re-announces C (still at 0004): trusted again.
  h.feed("0004", "BC0000000000000C");
  CHECK(h.key_for("0004") == "00000C");
  CHECK(h.stats.held == 0);
}

static void test_full_shuffle() {
  std::puts("thirty panels, every address reassigned: every barcode entity lands on its panel");
  Harness h;
  auto bc = [](int i) {
    char b[17];
    std::snprintf(b, sizeof b, "04C05B40%08X", 0x00899A00 + i);
    return std::string(b);
  };
  auto addr = [](int i) {
    char a[5];
    std::snprintf(a, sizeof a, "%04X", i);
    return std::string(a);
  };
  for (int i = 0; i < 30; ++i) h.barcode_keys.push_back(bc(i).substr(8));  // last 8, as the web UI shows
  for (int i = 0; i < 30; ++i) h.feed(addr(i + 1), bc(i));
  // Rotate everyone by 7 addresses.
  for (int i = 0; i < 30; ++i) h.feed(addr(((i + 7) % 30) + 1), bc(i));
  int right = 0;
  for (int i = 0; i < 30; ++i)
    if (h.key_for(addr(((i + 7) % 30) + 1)) == bc(i).substr(8)) right++;
  CHECK(right == 30);
  CHECK(h.stats.held == 0 && h.stats.ambiguous == 0);
}

static void test_barcode_wins_over_address_for_the_same_node() {
  std::puts("a node matching both a barcode and an address key publishes to the barcode");
  Harness h;
  h.barcode_keys = {"00000A"};
  h.short_keys = {"0002"};
  h.restore("0002", "BC0000000000000A");
  h.rebuild();
  CHECK(h.key_for("0002") == "00000A");
}

static void test_ambiguous_tail_is_counted() {
  std::puts("two keys matching one panel are reported, first wins");
  Harness h;
  h.barcode_keys = {"00899AA0", "899AA0"};
  h.restore("0004", "04C05B4000899AA0");
  h.rebuild();
  CHECK(h.key_for("0004") == "00899AA0");
  CHECK(h.stats.ambiguous == 1);
}

int main() {
  test_normalize();
  test_barcode_matches();
  test_short_address_binding_unchanged();
  test_barcode_binding_resolves_at_boot();
  test_two_panels_swap_addresses();
  test_stale_pairing_is_held_after_a_renumbering();
  test_full_shuffle();
  test_barcode_wins_over_address_for_the_same_node();
  test_ambiguous_tail_is_counted();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
