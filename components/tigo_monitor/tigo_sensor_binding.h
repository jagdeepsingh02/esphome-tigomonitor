#pragma once

// Which configured per-panel sensor entry a bus address feeds (#74).
//
// A per-panel `sensor:` entry names its panel with `address:`. That used to be
// the 4-hex-char short address only — but the gateway hands those out at
// commissioning and can reassign every one of them at once, after which the
// firmware kept publishing whatever panel now answers at `0004` into the
// entities configured for `0004`. Home Assistant has no way to notice: an
// ESPHome entity is identified by device + name, so the history just carries
// on with another panel's numbers in it.
//
// `address:` may now also name the panel's barcode — its 16-hex-char Frame 27
// long address, or a tail of it at least kBarcodeMinLen chars long (the web
// UI's node table shows the last eight). A barcode binding follows the
// physical panel through a renumbering. The functions here turn the node table
// (short address <-> long address) into "which configured key does this short
// address publish to". Header-only and free of ESPHome dependencies so the
// renumbering cases can be exercised on the host
// (tests/host/sensor_binding_test.cpp); Node is NodeTableData in the firmware.
//
// Keys arrive already normalised by sensor.py: upper-case hex, separators and
// the web UI's leading ellipsis stripped. normalize() below is the same rule,
// kept here so the host test pins both sides to one definition.

#include <cstddef>

namespace esphome {
namespace tigo_monitor {
namespace sensor_binding {

static const size_t kShortAddrLen = 4;
// Shortest barcode tail accepted. A 4-char tail cannot be told apart from a
// short address, and 6 is what the history slots already key panels by.
static const size_t kBarcodeMinLen = 6;
static const size_t kBarcodeMaxLen = 16;

enum class KeyKind { SHORT_ADDRESS, BARCODE, INVALID };

inline bool is_hex_(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

template<class Str> KeyKind classify(const Str &key) {
  if (key.size() == kShortAddrLen) return KeyKind::SHORT_ADDRESS;
  if (key.size() >= kBarcodeMinLen && key.size() <= kBarcodeMaxLen) return KeyKind::BARCODE;
  return KeyKind::INVALID;
}

// Upper-case the hex digits of `in` into `out`, dropping '-', ' ', '.', and the
// UTF-8 bytes of U+2026 (the "…" the web UI prefixes a truncated barcode
// with). Any other character makes the key INVALID.
template<class Str> KeyKind normalize(const char *in, Str &out) {
  out.clear();
  for (const char *p = in; *p != '\0'; ++p) {
    char c = *p;
    if (c == '-' || c == ' ' || c == '.') continue;
    if (static_cast<unsigned char>(p[0]) == 0xE2 && static_cast<unsigned char>(p[1]) == 0x80 &&
        static_cast<unsigned char>(p[2]) == 0xA6) {
      p += 2;
      continue;
    }
    if (!is_hex_(c)) return KeyKind::INVALID;
    out.push_back((c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c);
  }
  return classify(out);
}

// True if barcode key `tail` names the panel with long address `long_address`.
// Both are upper-case hex (Frame 27 long addresses are formatted "%02X").
template<class StrA, class StrB> bool barcode_matches(const StrA &long_address, const StrB &tail) {
  if (tail.size() < kBarcodeMinLen || tail.size() > long_address.size()) return false;
  const size_t off = long_address.size() - tail.size();
  for (size_t i = 0; i < tail.size(); ++i)
    if (long_address[off + i] != tail[i]) return false;
  return true;
}

struct BuildStats {
  int barcode_bound = 0;   // nodes resolved to a barcode key
  int held = 0;            // barcode-bound nodes held back (pairing unconfirmed)
  int ambiguous = 0;       // nodes a second barcode key also matched
};

// Rebuild `out` (short address -> key) from the node table.
//
// For each node: if its long address matches a configured barcode key, the
// node publishes to that key — unless `renumbering_seen` and the gateway has
// not re-confirmed this node's pairing since (`seen_in_frame27`), in which
// case the stored pairing may belong to the panel that used to hold this
// address, and the node maps to `held` (an empty key that matches no sensor):
// publishing nothing beats publishing another panel's data. Otherwise, if its
// short address is itself a configured key, it publishes there (legacy
// behaviour). Nodes that match neither get no entry; the caller falls back to
// the short address, which keeps devices not yet in the node table working
// exactly as before.
//
// `barcode_keys` / `short_keys` are any iterable of Str; `short_keys` needs
// count(). Out needs clear() and operator[].
template<class Table, class Keys, class ShortKeys, class Map, class Str>
BuildStats build(const Table &nodes, const Keys &barcode_keys, const ShortKeys &short_keys,
                 bool renumbering_seen, const Str &held, Map &out) {
  BuildStats stats;
  out.clear();
  for (const auto &node : nodes) {
    const Str *match = nullptr;
    if (!node.long_address.empty()) {
      for (const auto &key : barcode_keys) {
        if (!barcode_matches(node.long_address, key)) continue;
        if (match == nullptr) {
          match = &key;
        } else {
          stats.ambiguous++;  // first wins; sensor.py rejects nested tails
          break;
        }
      }
    }
    if (match != nullptr) {
      if (renumbering_seen && !node.seen_in_frame27) {
        out[node.addr] = held;
        stats.held++;
      } else {
        out[node.addr] = *match;
        stats.barcode_bound++;
      }
    } else if (short_keys.count(node.addr) > 0) {
      out[node.addr] = node.addr;
    }
  }
  return stats;
}

}  // namespace sensor_binding
}  // namespace tigo_monitor
}  // namespace esphome
